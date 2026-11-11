"""Self-play between policy generations (research-log S1), on any polyhost host; on the PC it runs
with POLYHOST=qemu (the Android build under QEMU) and the GPU. Two modes:

- **Training games** (`--out_dir`): the learner policy (`--net`) plays at `--temperature` against an
  opponent drawn per game from `--pool`: `self` (the same policy on both seats, both recorded), a
  checkpoint (an earlier generation) or `bot:<difficulty>` (the game's own AI); against those only the
  learner's seat is recorded. Files are py/dagger.py's
  layout, so py/train_rl.py reads them: `opponent` is -1 for self and 100 + the checkpoint's index
  in the pool otherwise (checkpoint or bot), `played`/`logp` are the learner's sampled rows, `v_old` the learner's own
  value estimate (probability of winning) before each of its moves, for py/train_rl.py --baseline
  value, `label` = `played` (there is no teacher), `source` = 'selfplay'. With `--starts`, a
  `--start_share` of the games (not those against bots) begin from a human midgame position
  (py/human_starts.py) instead of a new map; `start_turn` is the turn they began at (0 otherwise).
  Training games use only the positions of human games outside the held-out split (`--starts_split
  train`, train_bc.is_val's split of the human games); a game in which the learner made no recorded
  move (a start position the opponent wins at once) is counted but not saved. With --privileged, each
  recorded decision also stores what the opponent's own client shows at that moment (`opp_tiles`,
  `opp_globals`, `opp_techs`), for a training-only critic (py/train_rl.py --pcritic); the policy
  never sees it. With --search_share (research log S10, track s10x), that share of the learner's turns
  is chosen whole by the planner of py/search_loop.py (fog-safe samples, scored by --search_scorer);
  its commands are played as forced rows, `searched` marks those decisions and `search` holds one row
  per searched turn (search_loop.INFO).
- **Matches** (`--match B`): `--net` against B, both greedy (`--temperature` 0), each seed played
  twice with the seats swapped. B is a checkpoint or `bot:<difficulty>` (the game's own AI on the
  other seat, as py/eval_vs_bot.py; the same seeds and tribe draws give the same games). Writes one
  line per game to `--results` and prints the score with its Wilson interval. With `--starts`
  (checkpoints only), the games begin from held-out human positions instead (`--starts_split val`):
  one position from each of the first `--seeds` held-out games (ordered by a hash of the game id),
  each played twice with the seats swapped.

Every game process has its own worker thread that steps its slots; one inference thread batches
the workers' requests per network on the GPU. Tribes are drawn from the seed as in eval_vs_bot.py.
One Python process tops out at about one core (the GIL), so with many game processes the games are
split across `--shards` child processes (default: one per 10 game processes), each with its own
networks, game processes and ports; the parent combines their results.

    POLYHOST=qemu python selfplay.py gen004.pt --out_dir games/g005 --games 2000 \
        --pool self:0.4,gen003.pt:0.25,gen002.pt:0.25,bot:3:0.1 --temperature 0.5 --first_seed 50500000
    POLYHOST=qemu python selfplay.py gen005.pt --match gen004.pt --seeds 100 --first_seed 13000000 --results h2h.tsv
    POLYHOST=qemu python selfplay.py gen005.pt --match bot:3 --seeds 100 --first_seed 10000000 --results crazy.tsv
    POLYHOST=linux python selfplay.py gen005.pt --match gen000.pt --starts ~/polydata/human_starts_big --starts_split val \
        --seeds 100 --first_seed 17000000 --results h_gen0.tsv
"""
import argparse
import contextlib
import json
import os
import queue
import struct
import subprocess
import sys
import threading
import time
import zlib

import numpy as np
import torch

import dagger
import eval_vs_bot
import fast_infer
import gen_data
import model as M
import polyenv
import train_bc

REGULAR_TRIBES = sorted(polyenv.TRIBES.values())
TURN_CAP = 150
COMMANDS_PER_TURN_CAP = 400
END_TURN = 15


def load_starts(path, split):
    """The human positions of py/human_starts.py's index.tsv: [dict(file, game, tribes)]. split 'train'
    keeps the games outside the human held-out split (train_bc.is_val), 'val' only those, 'all' every one."""
    rows = [l.rstrip('\n').split('\t') for l in open(os.path.join(path, 'index.tsv'))][1:]
    out = [dict(file=os.path.join(path, r[0]), game=r[1], tribes=tuple(int(x) for x in r[4].split(','))) for r in rows]
    if split != 'all':
        out = [s for s in out if train_bc.is_val(s['game'] + '.npz') == (split == 'val')]
    return out


def match_starts(starts, n):
    """n positions for matches: one per game (the one whose file name hashes lowest), from the games
    whose ids hash lowest, so the set is fixed for a given index."""
    h = lambda x: zlib.crc32(x.encode())
    by_game = {}
    for s in starts:
        if s['game'] not in by_game or h(os.path.basename(s['file'])) < h(os.path.basename(by_game[s['game']]['file'])):
            by_game[s['game']] = s
    return [by_game[g] for g in sorted(by_game, key=h)[:n]]


def load_net(path, device):
    ck = torch.load(path, map_location='cpu')
    net = M.Net(**ck.get('config', {}))
    net.load_state_dict(ck['model'])
    return net.to(device).eval()


class Locked:
    """A game process shared by threads: one request at a time."""

    def __init__(self, env):
        self.env, self.lock = env, threading.Lock()

    def __getattr__(self, name):
        fn = getattr(self.env, name)

        def call(*a, **k):
            with self.lock:
                return fn(*a, **k)
        return call


class Inference:
    """Batches decision requests from the worker threads, per network. A request is (key, obs, statuses,
    temperature) and gets back (rows, log-probabilities, values); greedy rows have log-probability 0,
    values are the network's probability that the player to move wins."""

    def __init__(self, nets, device, bf16, max_wait=0.004):
        self.nets, self.device, self.bf16, self.max_wait = nets, device, bf16, max_wait
        # the same networks, run as CUDA graphs with densely scored legal rows (py/fast_infer.py)
        self.fast = {k: fast_infer.FastPolicy(net, device, bf16=bf16) for k, net in nets.items()}
        self.checked = set()
        self.q = queue.Queue()
        self.busy = 0.0
        self.batches = 0
        self.positions = 0
        threading.Thread(target=self.loop, daemon=True).start()

    def request(self, key, obs, statuses, temperature, force=None):
        """force (search in the loop): per position a row to play instead of sampling, or -1; the
        log-probability returned is the forced row's."""
        return self.collect(self.submit(key, obs, statuses, temperature, force))

    def submit(self, key, obs, statuses, temperature, force=None):
        done = threading.Event()
        box = {}
        self.q.put((key, obs, statuses, temperature, done, box, force))
        return done, box

    @staticmethod
    def collect(pending):
        done, box = pending
        done.wait()
        if 'error' in box:
            raise box['error']
        return box['rows'], box['logp'], box['value']

    def loop(self):
        while True:
            items = [self.q.get()]
            t_end = time.time() + self.max_wait
            while True:
                try:
                    items.append(self.q.get(timeout=max(0.0, t_end - time.time())))
                except queue.Empty:
                    break
            t0 = time.time()
            by_key = {}
            for it in items:
                by_key.setdefault((it[0], it[3]), []).append(it)
            for (key, temperature), group in by_key.items():
                try:
                    obs = [o for it in group for o in it[1]]
                    sts = [s for it in group for s in it[2]]
                    force = None
                    if any(it[6] is not None for it in group):
                        force = [x for it in group for x in (it[6] if it[6] is not None else [-1] * len(it[1]))]
                    rows, logp, value = self.run(key, obs, sts, temperature, force)
                    k = 0
                    for it in group:
                        n = len(it[1])
                        it[5]['rows'], it[5]['logp'], it[5]['value'] = rows[k:k + n], logp[k:k + n], value[k:k + n]
                        k += n
                except Exception as e:  # hand the error to the waiting workers
                    for it in group:
                        it[5]['error'] = e
                for it in group:
                    it[4].set()
            self.busy += time.time() - t0
            self.batches += 1
            self.positions += sum(len(it[1]) for it in items)

    @torch.no_grad()
    def run(self, key, obs, statuses, temperature, force=None):
        inputs = eval_vs_bot.batch_inputs(obs, statuses)
        logits, v = self.fast[key](inputs)
        if key not in self.checked:  # once per network: the same choices as the plain forward pass
            self.checked.add(key)
            t = {k: torch.from_numpy(x).to(self.device) for k, x in inputs.items()}
            with torch.autocast('cuda', dtype=torch.bfloat16, enabled=self.bf16 and self.device == 'cuda'):
                ref, _, _ = self.nets[key](t['tiles'], t['globals'], t['techs'], t['legal'], t['mask'])
            ref = ref.float().masked_fill(~t['mask'], float('-inf'))
            if not torch.equal(ref.argmax(1), logits.argmax(1)):
                print(f'warning: fast inference for {key} chose differently from Net.forward on '
                      f'{int((ref.argmax(1) != logits.argmax(1)).sum())} of {len(obs)} positions', flush=True)
        value = torch.sigmoid(v.float()).cpu().numpy()
        if temperature is None:  # the scores themselves, padded (planner v2 in the loop, py/search_v2.py)
            return logits.float().cpu().numpy(), np.zeros(len(obs), np.float32), value
        if temperature <= 0:
            return logits.argmax(1).cpu().numpy(), np.zeros(len(obs), np.float32), value
        lp = torch.log_softmax(logits / temperature, 1)
        rows = torch.multinomial(lp.exp(), 1).squeeze(1)
        if force is not None:
            f = torch.as_tensor(force, dtype=rows.dtype, device=rows.device)
            rows = torch.where(f >= 0, f, rows)
        return rows.cpu().numpy(), lp.gather(1, rows[:, None]).squeeze(1).cpu().numpy(), value


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('net', help='the learner policy (training games) or side A (matches)')
    ap.add_argument('--out_dir', default=None, help='training games: write them here')
    ap.add_argument('--pool', default='self', help='training games: opponents with shares, "self:0.5,a.pt:0.25,bot:3:0.25"')
    ap.add_argument('--games', type=int, default=1000)
    ap.add_argument('--starts', default=None, help='a directory of human midgame states (py/human_starts.py): training '
                                                   'games may begin from them; matches all begin from them')
    ap.add_argument('--starts_split', choices=['train', 'val', 'all'], default=None,
                    help='which human games\' positions to use (default: train for training games, val for matches)')
    ap.add_argument('--start_share', type=float, default=0.0, help='share of training games that begin from one of them')
    ap.add_argument('--match', default=None, help='side B: a checkpoint, or bot:<difficulty>')
    ap.add_argument('--seeds', type=int, default=100, help='matches: games = 2 x seeds, seats swapped')
    ap.add_argument('--results', default=None, help='matches: one line per game')
    ap.add_argument('--first_seed', type=int, default=50_000_000)
    ap.add_argument('--temperature', type=float, default=0.5, help='training games; matches are greedy')
    ap.add_argument('--procs', type=int, default=30)
    ap.add_argument('--slots', type=int, default=6)
    ap.add_argument('--port', type=int, default=9200)
    ap.add_argument('--device', default='cuda' if torch.cuda.is_available() else 'mps' if torch.backends.mps.is_available() else 'cpu')
    ap.add_argument('--save_games', default=None, help='matches: keep the games for the web app\'s watch mode, '
                                                       'as py/eval_vs_bot.py --save_games')
    ap.add_argument('--privileged', action='store_true', help='training games: also record the opponent\'s view of each recorded decision')
    ap.add_argument('--search_share', type=float, default=0.0,
                    help='training games: share of the learner\'s turns its whole turn is chosen by the planner (py/search_loop.py; '
                         'needs POLYHOST_BIN=polyhost-det on the PC)')
    ap.add_argument('--search_scorer', default=None, help='--search_share: checkpoint whose value head scores the lanes (a fog critic)')
    ap.add_argument('--search_plans', type=int, default=16, help='--search_share: lanes per searched turn (the top-move lane + sampled ones)')
    ap.add_argument('--search_samples', type=int, default=2, help='--search_share: fog-safe samples each lane is scored on')
    ap.add_argument('--search_tau', type=float, default=0.05,
                    help='--search_share: the played lane is drawn from the sampled lanes with probability softmax(score / tau); '
                         '0: the best of all lanes')
    ap.add_argument('--search_slots', type=int, default=100, help='--search_share: first scratch slot (each thread group uses 50)')
    ap.add_argument('--search_v2', default=None,
                    help='--search_share: plan with planner v2 (py/search_v2.py, py/tactics/planner2.py) instead of py/search_loop.py; '
                         'its options as JSON ("{}": its defaults) or a JSON file; it plays its best lane (--search_tau unused); '
                         'needs POLYHOST_BIN=polyhost-cp on the PC')
    ap.add_argument('--shards', type=int, default=0, help='child processes to split the games across (0: one per 10 game processes)')
    ap.add_argument('--shard', default=None, help=argparse.SUPPRESS)  # i/K: this child's share (set by the parent)
    args = ap.parse_args()
    shards = args.shards or -(-args.procs // 10)
    if shards > 1 and args.shard is None:
        return run_shards(args, shards)
    match = args.match is not None
    if match == (args.out_dir is not None):
        ap.error('give exactly one of --out_dir (training games) and --match')

    nets = {'A': load_net(args.net, args.device)}
    rng = np.random.default_rng(args.first_seed)
    split = args.starts_split or ('val' if match else 'train')
    m_starts = []
    if match:
        bot = int(args.match.split(':')[1]) if args.match.startswith('bot:') else None
        if bot is None:
            nets['B'] = load_net(args.match, args.device)
        elif args.starts:
            ap.error('matches from human positions are between checkpoints only')
        n = args.seeds
        if args.starts:
            m_starts = match_starts(load_starts(args.starts, split), args.seeds)
            n = len(m_starts)
            print(f'{n} held-out human positions ({split}), each played from both seats', flush=True)
        jobs = [(args.first_seed + k, seat) for k in range(n) for seat in (0, 1)]  # seat = A's seat
        opponents = None
    else:
        os.makedirs(args.out_dir, exist_ok=True)
        opponents = []  # (name, share)
        for part in args.pool.split(','):
            name, share = part.rsplit(':', 1)
            opponents.append((name, float(share)))
        shares = np.array([s for _, s in opponents]) / sum(s for _, s in opponents)
        for i, (name, _) in enumerate(opponents):
            if name != 'self' and not name.startswith('bot:'):
                nets[f'P{i}'] = load_net(name, args.device)
        starts = []
        if args.starts and args.start_share > 0:
            starts = load_starts(args.starts, split)
            print(f'{len(starts)} human midgame starts ({split}), share {args.start_share}', flush=True)
        have = set(os.listdir(args.out_dir))
        jobs = [(s, None) for s in range(args.first_seed, args.first_seed + args.games) if f'g{s:08d}.npz' not in have]
    search = None
    if args.search_share > 0 and not match:
        import search_loop as search  # py/search_loop.py
        if args.search_v2 is not None:
            import search_v2 as search  # py/search_v2.py
            v2 = args.search_v2
            args.search_v2 = json.load(open(v2)) if os.path.exists(v2) else json.loads(v2)
            print(f'search in the loop: planner v2, options {json.dumps(args.search_v2)}', flush=True)
        nets['C'] = load_net(args.search_scorer or args.net, args.device)
        if args.search_plans * args.search_samples > search.SLOT_STRIDE:
            ap.error(f'--search_plans x --search_samples must be at most {search.SLOT_STRIDE}')
    if args.shard:
        i, k = map(int, args.shard.split('/'))
        jobs = jobs[i::k]
    jobs.reverse()
    jobs_lock = threading.Lock()
    infer = Inference(nets, args.device, bf16=not match)
    if search:  # the scorer's estimates in fp32: lanes differ by a few hundredths
        infer.fast['C'] = fast_infer.FastPolicy(nets['C'], args.device, bf16=False)
        infer.checked.add('C')
    s_count = dict(turns=0, seconds=0.0, gain=0.0, chosen_gain=0.0, kl=0.0, broken=0, commands=0, forced=0, tac=0, rep=0)

    results, log_lock = [], threading.Lock()
    out = open(args.results, 'a') if args.results else None
    games_log = open(os.path.join(args.out_dir, 'games.tsv'), 'a') if not match else None
    t0 = time.time()
    counters = dict(decisions=0, env=0.0)

    def report():
        n = len(results)
        if match:
            w = sum(r['res'] == 'win' for r in results)
            lo, hi = eval_vs_bot.wilson(w, n)
            print(f'{n} games: A won {w} ({w / n:.1%}, 95% CI {lo:.1%}-{hi:.1%}), '
                  f'{sum(r["res"] == "cap" for r in results)} capped, {time.time() - t0:.0f} s', flush=True)
        else:
            by = {}
            for r in results:
                by.setdefault(r['opp'], []).append(r['res'])
            summary = ', '.join(f"{'self' if o < 0 else opponents[o - 100][0].split('/')[-1]}: "
                                f"{sum(x == 'win' for x in v) if o >= 0 else len(v)}"
                                f"{'/' + str(len(v)) if o >= 0 else ' games'}" for o, v in sorted(by.items()))
            el = time.time() - t0
            if search:  # before the games line, which py/sp_loop.py logs as the last line
                print(search_line(s_count), flush=True)
            print(f'{n} games ({summary}), {counters["decisions"]} decisions, {el:.0f} s, '
                  f'{counters["decisions"] / el:.0f} decisions/s, GPU busy {infer.busy / el:.0%}, '
                  f'{infer.positions / max(infer.batches, 1):.0f} positions/batch', flush=True)

    def search_line(c):
        t = max(c['turns'], 1)
        return (f"searched {c['turns']} turns, {c['seconds'] / t:.2f} s each; best lane - top-move lane {c['gain'] / t:.3f}, "
                f"drawn lane - top-move lane {c['chosen_gain'] / t:.3f}, KL(weights || uniform) {c['kl'] / t:.3f}, "
                f"broken {c['broken'] / t:.1%}, {c['forced']} of {c['commands']} planned commands played"
                + (f", lanes played: tactical {c['tac'] / t:.1%}, repair {c['rep'] / t:.1%}" if args.search_v2 is not None else ''))

    def new_game(env, slot):
        with jobs_lock:
            if not jobs:
                return None
            seed, seat = jobs.pop()
        tribes = tuple(int(t) for t in np.random.default_rng(seed).choice(REGULAR_TRIBES, 2))
        if match:
            seats = {seat + 1: 'A', 2 - seat: 'B' if bot is None else None}
            bots = (None, None) if bot is None else ((None, bot) if seat == 0 else (bot, None))
            start = m_starts[seed - args.first_seed] if m_starts else None
            if start:
                tribes = start['tribes']
            g = dict(seed=seed, seat=seat, tribes=tribes, seats=seats, record=(), human_start=start)
        else:
            o = int(np.random.default_rng([seed, 1]).choice(len(opponents), p=shares))
            seat = int(np.random.default_rng([seed, 2]).integers(2))
            name = opponents[o][0]
            bots = (None, None)
            start = None
            if starts and not name.startswith('bot:') and np.random.default_rng([seed, 3]).random() < args.start_share:
                start = starts[int(np.random.default_rng([seed, 4]).integers(len(starts)))]
                tribes = start['tribes']
            if name == 'self':
                seats, record, opp = {1: 'A', 2: 'A'}, (1, 2), -1
            elif name.startswith('bot:'):  # the game's AI plays its seat inside polyhost, as in matches
                seats, record, opp = {seat + 1: 'A', 2 - seat: None}, (seat + 1,), 100 + o
                d = int(name.split(':')[1])
                bots = (None, d) if seat == 0 else (d, None)
            else:
                seats, record, opp = {seat + 1: 'A', 2 - seat: f'P{o}'}, (seat + 1,), 100 + o
            g = dict(seed=seed, seat=seat, tribes=tribes, seats=seats, record=record, opp=opp, human_start=start)
        spec = polyenv.Spec(tribes=tribes, bots=bots, seed=seed)
        if not match:
            g['rec'] = dagger.Recorder(seed, spec)
            g['rec'].value = []
            g['rec'].opp = []
            if search:
                g['rec'].searched, g['plan'], g['pturn'], g['search'] = [], [], None, []
                g['spec'] = spec
        if g['human_start']:
            with open(g['human_start']['file'], 'rb') as f:
                g['st'] = env.load(slot, spec, f.read())
        else:
            g['st'] = env.new(slot, spec)
        g['start_turn'] = g['st'].turn if g['human_start'] else 0
        g['turn'], g['n'] = g['st'].turn, 0
        if match and args.save_games:
            g['start'] = blob_prefix[0] + env.save(slot)
        return g

    blob_prefix = [b'']
    tag = os.path.basename(os.path.normpath(args.save_games)) if args.save_games else ''
    if args.save_games:
        os.makedirs(args.save_games, exist_ok=True)

    def finish(env, slot, g):
        st = g['st']
        if match:
            a_id = g['seat'] + 1
            res = 'win' if st.ended and st.winner == a_id else 'loss' if st.ended else 'cap'
            r = dict(seed=g['seed'], seat=g['seat'], tribes=g['tribes'], res=res, turns=st.turn)
            if args.save_games:
                name = f"{g['seed']}_s{a_id}"
                for ext, blob in (('start', g['start']), ('final', blob_prefix[0] + env.save(slot))):
                    with open(os.path.join(args.save_games, f'{name}.{ext}'), 'wb') as f:
                        f.write(blob)
                b_name = args.match if bot is not None else f'network ({os.path.basename(args.match)})'
                meta = dict(path=f'{tag}/{name}', tag=tag, time=time.strftime('%Y-%m-%d %H:%M:%S'), seed=g['seed'],
                            tribes=list(g['tribes']), players={str(a_id): f'network ({os.path.basename(args.net)})',
                                                               str(3 - a_id): b_name},
                            agent=a_id, result=res, turns=st.turn)
                if g['human_start']:
                    meta.update(start=os.path.basename(g['human_start']['file']), start_turn=g['start_turn'])
                with log_lock, open(os.path.join(args.save_games, 'games.jsonl'), 'a') as f:
                    f.write(json.dumps(meta) + '\n')
            with log_lock:
                results.append(r)
                if out:
                    out.write(f"{g['seed']}\t{g['seat']}\t{g['tribes'][0]}\t{g['tribes'][1]}\t{res}\t{st.turn}\n")
                    out.flush()
                if len(results) % 20 == 0:
                    report()
        else:
            if g['rec'].played:  # a start position the opponent wins before the learner moves records nothing
                owner, capital = env.owners(slot)
                opp = g['rec'].opp
                extra = dict(opp_tiles=np.stack([x[0] for x in opp]), opp_globals=np.stack([x[1] for x in opp]),
                             opp_techs=np.stack([x[2] for x in opp])) if args.privileged else {}
                if search:  # which decisions the planner chose, and one row per searched turn (search_loop.INFO)
                    extra.update(searched=np.asarray(g['rec'].searched, np.int8),
                                 search=np.array([[r[f] for f in search.INFO] for r in g['search']], np.float32).reshape(-1, len(search.INFO)))
                dagger.save(args.out_dir, g['rec'], st, owner, capital, g['opp'], g['record'], source='selfplay',
                            extra=dict(extra, v_old=np.asarray(g['rec'].value, np.float32), start_turn=np.int32(g['start_turn']),
                                       start=np.str_(os.path.basename(g['human_start']['file']) if g['human_start'] else '')))
            res = 'cap' if not st.ended else 'win' if st.winner in g['record'] else 'loss'
            with log_lock:
                results.append(dict(opp=g['opp'], res=res))
                games_log.write(f"{g['seed']}\t{g['opp']}\t{','.join(map(str, g['record']))}\t"
                                f"{g['tribes'][0]},{g['tribes'][1]}\t{res}\t{st.turn}\t{len(g['rec'].played)}\t{g['start_turn']}\n")
                games_log.flush()
                if len(results) % 50 == 0:
                    report()

    errors = []

    def plan_moves(env, slots, slots_now, live, rng):
        """--search_share: at the start of a recorded player's turn, maybe plans it (py/search_loop.py);
        while a slot's plan lasts, {slot: the legal row of its next command}. A command not legal in
        the real game (its hidden part differed from the samples) ends the plan."""
        force = {}
        base = args.search_slots + search.SLOT_STRIDE * (slots[0] % 2)  # this thread group's scratch slots
        for s in slots_now:
            g = live[s]
            st = g['st']
            if st.current not in g['record']:
                continue
            if g['pturn'] != (st.turn, st.current):
                g['pturn'], g['plan'] = (st.turn, st.current), []
                if search.pick(g['seed'], st.turn, st.current, args.search_share):
                    if args.search_v2 is not None:  # this thread group's slots: search_v2.OFFSET * (group + 1) ...
                        g['plan'], info = search.plan_turn(env, infer, s, g['spec'], args.search_v2, search.OFFSET * (slots[0] % 2 + 1),
                                                           args.search_plans, args.search_samples, args.temperature, rng)
                    else:
                        g['plan'], info = search.plan_turn(env, infer, s, st.current, base, args.search_plans, args.search_samples,
                                                           args.temperature, args.search_tau, g['seed'] * 1000 + st.turn * 4 + st.current, rng)
                    info.update(turn=st.turn, player=st.current)
                    g['search'].append(info)
                    with log_lock:
                        for k, x in (('turns', 1), ('seconds', info['seconds']), ('gain', info['best'] - info['top']),
                                     ('chosen_gain', info['chosen'] - info['top']), ('kl', info['kl']), ('commands', len(g['plan'])),
                                     ('tac', info.get('kind') == 1), ('rep', info.get('kind') == 2)):
                            s_count[k] += x
            if g['plan']:
                r = search.find(st, g['plan'][0])
                if r >= 0:
                    force[s] = r
                    g['plan'].pop(0)
                    with log_lock:
                        s_count['forced'] += 1
                else:
                    g['plan'] = []
                    g['search'][-1]['broken'] = 1
                    with log_lock:
                        s_count['broken'] += 1
        return force

    def run_slots(i, env, slots):
        """Plays games on these slots of one game process until no jobs are left."""
        s_rng = np.random.default_rng([args.first_seed, args.port + i, slots[0]]) if search else None  # draws among lanes
        live = {}
        for s in slots:
            g = new_game(env, s)
            if g:
                live[s] = g
        while live and not errors:
            slots_now = sorted(live)
            te = time.time()
            obs = env.observe([(s, 0) for s in slots_now])
            opp_obs = {}
            if args.privileged and not match:  # the opponent's own view, for the training-only critic
                want = [s for s in slots_now if live[s]['st'].current in live[s]['record']]
                if want:
                    opp_obs = dict(zip(want, env.observe([(s, 3 - live[s]['st'].current) for s in want])))
            counters['env'] += time.time() - te
            force = plan_moves(env, slots, slots_now, live, s_rng) if search else {}
            acts = {}
            by_key = {}
            for s, o in zip(slots_now, obs):
                g = live[s]
                by_key.setdefault(g['seats'][g['st'].current], []).append((s, o))
            for key, items in by_key.items():
                temp = 0.0 if match else args.temperature
                f = [force.get(s, -1) for s, _ in items] if force else None
                rows, logp, value = infer.request(key, [o for _, o in items], [live[s]['st'] for s, _ in items], temp, f)
                for (s, o), a, lp, v in zip(items, rows, logp, value):
                    g = live[s]
                    st = g['st']
                    g['recorded'] = not match and st.current in g['record']
                    if g['recorded']:
                        g['rec'].add(o, st.legal, int(a), st.current)
                        g['rec'].played.append(int(a))
                        g['rec'].logp.append(float(lp))
                        g['rec'].value.append(float(v))
                        if args.privileged:
                            q = opp_obs[s]
                            g['rec'].opp.append((q.tiles.reshape(-1, q.tiles.shape[-1]), q.globals, q.techs))
                        if search:
                            g['rec'].searched.append(int(s in force))
                    if st.turn != g['turn']:
                        g['turn'], g['n'] = st.turn, 0
                    g['n'] += 1
                    if g['n'] > COMMANDS_PER_TURN_CAP:  # a safety net against a stuck policy
                        ends = np.nonzero(st.legal[:, 0] == END_TURN)[0]
                        if len(ends):  # (not legal while a choice is pending, e.g. a city reward)
                            a = int(ends[0])
                    acts[s] = int(a)
            te = time.time()
            stepped = env.step([(s, acts[s]) for s in slots_now])
            counters['env'] += time.time() - te
            counters['decisions'] += len(slots_now)
            for s, (ok, st) in zip(slots_now, stepped):
                if not ok:
                    # the game rejected a listed command: polyhost relisted the unchanged state with every
                    # command checked; forget this decision and choose again
                    counters['rejected'] = counters.get('rejected', 0) + 1
                    print(f'env {i} slot {s}: listed command rejected, relisted ({counters["rejected"]} so far)', flush=True)
                    g = live[s]
                    if g.get('recorded'):
                        r = g['rec']
                        for lst in (r.tiles, r.globs, r.techs, r.legal, r.labels, r.players, r.played, r.logp, r.value) + \
                                ((r.opp,) if args.privileged else ()) + ((r.searched,) if search else ()):
                            lst.pop()
                    if s in force:  # a planned command the game rejected: the rest of the turn is the policy's
                        g['plan'] = []
                        g['search'][-1]['broken'] = 1
                        with log_lock:
                            s_count['broken'] += 1
                    live[s]['st'] = st
                    continue
                live[s]['st'] = st
                if st.ended or st.turn >= TURN_CAP:
                    finish(env, s, live.pop(s))
                    g = new_game(env, s)
                    if g:
                        live[s] = g

    def guarded(fn, *a):
        try:
            fn(*a)
        except Exception as e:
            errors.append(e)
            raise

    def worker(i):
        """One game process, its slots in two groups on two threads: while one group waits for the
        network, the process steps the other (the process serves one request at a time)."""
        with polyenv.launch(args.port + i) as raw:
            blob_prefix[0] = struct.pack('<i', raw.info()['game_version'])  # as the game's server sends states
            env = Locked(raw)
            groups = [list(range(0, args.slots, 2)), list(range(1, args.slots, 2))]
            ts = [threading.Thread(target=guarded, args=(run_slots, i, env, grp), daemon=True) for grp in groups if grp]
            for t in ts:
                t.start()
            for t in ts:
                t.join()

    threads = [threading.Thread(target=guarded, args=(worker, i), daemon=True) for i in range(args.procs)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    if errors:
        raise errors[0]
    report()
    if args.shard:  # for the parent (run_shards)
        print('SHARD ' + json.dumps(dict(results=results, decisions=counters['decisions'], busy=infer.busy,
                                         batches=infer.batches, positions=infer.positions, **({'search': s_count} if search else {}))),
              flush=True)
        return
    if match:
        n = len(results)
        w = sum(r['res'] == 'win' for r in results)
        lo, hi = eval_vs_bot.wilson(w, n)
        by_seat = [sum(r['res'] == 'win' for r in results if r['seat'] == s) for s in (0, 1)]
        turns = [r['turns'] for r in results if r['res'] != 'cap']
        summary = dict(a=args.net, b=args.match, games=n, a_wins=w, a_rate=w / max(n, 1), ci=[lo, hi],
                       capped=sum(r['res'] == 'cap' for r in results), a_wins_by_seat=by_seat,
                       mean_turns=float(np.mean(turns)) if turns else None, first_seed=args.first_seed,
                       seconds=round(time.time() - t0))
        print('SUMMARY ' + json.dumps(summary), flush=True)


def run_shards(args, k):
    """Runs the games as k child processes of this script (each with its share of the jobs, game
    processes and ports), forwards their output, and reports their combined results as one run."""
    t0 = time.time()
    sizes = [args.procs // k + (i < args.procs % k) for i in range(k)]
    procs, port = [], args.port
    for i, n in enumerate(sizes):
        cmd = [sys.executable, os.path.abspath(__file__)] + sys.argv[1:] + [
            '--procs', str(n), '--port', str(port), '--shards', str(k), '--shard', f'{i}/{k}']
        port += n
        procs.append(subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1))
    parts, lock = [], threading.Lock()

    def forward(i, p):
        for line in p.stdout:
            if line.startswith('SHARD '):
                with lock:
                    parts.append(json.loads(line[6:]))
            else:
                print(f'[{i}] {line}', end='', flush=True)

    readers = [threading.Thread(target=forward, args=(i, p)) for i, p in enumerate(procs)]
    for r in readers:
        r.start()
    codes = [p.wait() for p in procs]
    for r in readers:
        r.join()
    if any(codes):
        raise SystemExit(f'shard exit codes {codes}')
    results = [r for part in parts for r in part['results']]
    decisions = sum(part['decisions'] for part in parts)
    el = time.time() - t0
    n = len(results)
    if args.match is None:
        by = {}
        for r in results:
            by.setdefault(r['opp'], []).append(r['res'])
        pool = [part.rsplit(':', 1)[0] for part in args.pool.split(',')]
        summary = ', '.join(f"{'self' if o < 0 else pool[o - 100].split('/')[-1]}: "
                            f"{sum(x == 'win' for x in v) if o >= 0 else len(v)}{'/' + str(len(v)) if o >= 0 else ' games'}"
                            for o, v in sorted(by.items()))
        if any('search' in p for p in parts):  # --search_share: the shards' planner counts, combined
            c = {x: sum(p['search'][x] for p in parts) for x in parts[0]['search']}
            t = max(c['turns'], 1)
            print(f"searched {c['turns']} turns, {c['seconds'] / t:.2f} s each (per thread); best lane - top-move lane "
                  f"{c['gain'] / t:.3f}, drawn lane - top-move lane {c['chosen_gain'] / t:.3f}, KL(weights || uniform) "
                  f"{c['kl'] / t:.3f}, broken {c['broken'] / t:.1%}, {c['forced']} of {c['commands']} planned commands played"
                  + (f", lanes played: tactical {c.get('tac', 0) / t:.1%}, repair {c.get('rep', 0) / t:.1%}" if args.search_v2 is not None else ''),
                  flush=True)
        print(f'{n} games ({summary}), {decisions} decisions, {el:.0f} s, {decisions / el:.0f} decisions/s, '
              f'{k} shards, GPU busy {np.mean([p["busy"] for p in parts]) / el:.0%} per shard, '
              f'{sum(p["positions"] for p in parts) / max(sum(p["batches"] for p in parts), 1):.0f} positions/batch', flush=True)
        return
    w = sum(r['res'] == 'win' for r in results)
    lo, hi = eval_vs_bot.wilson(w, n)
    turns = [r['turns'] for r in results if r['res'] != 'cap']
    print(f'{n} games: A won {w} ({w / max(n, 1):.1%}, 95% CI {lo:.1%}-{hi:.1%}), {decisions / el:.0f} decisions/s, {k} shards',
          flush=True)
    summary = dict(a=args.net, b=args.match, games=n, a_wins=w, a_rate=w / max(n, 1), ci=[lo, hi],
                   capped=sum(r['res'] == 'cap' for r in results),
                   a_wins_by_seat=[sum(r['res'] == 'win' for r in results if r['seat'] == s) for s in (0, 1)],
                   mean_turns=float(np.mean(turns)) if turns else None, first_seed=args.first_seed, seconds=round(el))
    print('SUMMARY ' + json.dumps(summary), flush=True)


if __name__ == '__main__':
    main()
