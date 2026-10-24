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
- **Matches** (`--match B`): `--net` against B, both greedy (`--temperature` 0), each seed played
  twice with the seats swapped. B is a checkpoint or `bot:<difficulty>` (the game's own AI on the
  other seat, as py/eval_vs_bot.py; the same seeds and tribe draws give the same games). Writes one
  line per game to `--results` and prints the score with its Wilson interval.

Every game process has its own worker thread that steps its slots; one inference thread batches
the workers' requests per network on the GPU. Tribes are drawn from the seed as in eval_vs_bot.py.

    POLYHOST=qemu python selfplay.py gen004.pt --out_dir games/g005 --games 2000 \
        --pool self:0.4,gen003.pt:0.25,gen002.pt:0.25,bot:3:0.1 --temperature 0.5 --first_seed 50500000
    POLYHOST=qemu python selfplay.py gen005.pt --match gen004.pt --seeds 100 --first_seed 13000000 --results h2h.tsv
    POLYHOST=qemu python selfplay.py gen005.pt --match bot:3 --seeds 100 --first_seed 10000000 --results crazy.tsv
"""
import argparse
import contextlib
import json
import os
import queue
import struct
import threading
import time

import numpy as np
import torch

import dagger
import eval_vs_bot
import gen_data
import model as M
import polyenv

REGULAR_TRIBES = sorted(polyenv.TRIBES.values())
TURN_CAP = 150
COMMANDS_PER_TURN_CAP = 400
END_TURN = 15


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
        self.q = queue.Queue()
        self.busy = 0.0
        self.batches = 0
        self.positions = 0
        threading.Thread(target=self.loop, daemon=True).start()

    def request(self, key, obs, statuses, temperature):
        done = threading.Event()
        box = {}
        self.q.put((key, obs, statuses, temperature, done, box))
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
                    rows, logp, value = self.run(key, obs, sts, temperature)
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
    def run(self, key, obs, statuses, temperature):
        inputs = eval_vs_bot.batch_inputs(obs, statuses)
        t = {k: torch.from_numpy(v).to(self.device, non_blocking=True) for k, v in inputs.items()}
        with torch.autocast('cuda', dtype=torch.bfloat16, enabled=self.bf16 and self.device == 'cuda'):
            logits, v, _ = self.nets[key](t['tiles'], t['globals'], t['techs'], t['legal'], t['mask'])
        logits = logits.float().masked_fill(~t['mask'], float('-inf'))
        value = torch.sigmoid(v.float()).cpu().numpy()
        if temperature <= 0:
            return logits.argmax(1).cpu().numpy(), np.zeros(len(obs), np.float32), value
        lp = torch.log_softmax(logits / temperature, 1)
        rows = torch.multinomial(lp.exp(), 1).squeeze(1)
        return rows.cpu().numpy(), lp.gather(1, rows[:, None]).squeeze(1).cpu().numpy(), value


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('net', help='the learner policy (training games) or side A (matches)')
    ap.add_argument('--out_dir', default=None, help='training games: write them here')
    ap.add_argument('--pool', default='self', help='training games: opponents with shares, "self:0.5,a.pt:0.25,bot:3:0.25"')
    ap.add_argument('--games', type=int, default=1000)
    ap.add_argument('--starts', default=None, help='training games: a directory of human midgame states (py/human_starts.py)')
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
    args = ap.parse_args()
    match = args.match is not None
    if match == (args.out_dir is not None):
        ap.error('give exactly one of --out_dir (training games) and --match')

    nets = {'A': load_net(args.net, args.device)}
    rng = np.random.default_rng(args.first_seed)
    if match:
        bot = int(args.match.split(':')[1]) if args.match.startswith('bot:') else None
        if bot is None:
            nets['B'] = load_net(args.match, args.device)
        jobs = [(args.first_seed + k, seat) for k in range(args.seeds) for seat in (0, 1)]  # seat = A's seat
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
            rows = [l.rstrip('\n').split('\t') for l in open(os.path.join(args.starts, 'index.tsv'))][1:]
            starts = [dict(file=os.path.join(args.starts, r[0]), tribes=tuple(int(x) for x in r[4].split(','))) for r in rows]
            print(f'{len(starts)} human midgame starts, share {args.start_share}', flush=True)
        have = set(os.listdir(args.out_dir))
        jobs = [(s, None) for s in range(args.first_seed, args.first_seed + args.games) if f'g{s:08d}.npz' not in have]
    jobs.reverse()
    jobs_lock = threading.Lock()
    infer = Inference(nets, args.device, bf16=not match)

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
            print(f'{n} games ({summary}), {counters["decisions"]} decisions, {el:.0f} s, '
                  f'{counters["decisions"] / el:.0f} decisions/s, GPU busy {infer.busy / el:.0%}, '
                  f'{infer.positions / max(infer.batches, 1):.0f} positions/batch', flush=True)

    def new_game(env, slot):
        with jobs_lock:
            if not jobs:
                return None
            seed, seat = jobs.pop()
        tribes = tuple(int(t) for t in np.random.default_rng(seed).choice(REGULAR_TRIBES, 2))
        if match:
            seats = {seat + 1: 'A', 2 - seat: 'B' if bot is None else None}
            bots = (None, None) if bot is None else ((None, bot) if seat == 0 else (bot, None))
            g = dict(seed=seed, seat=seat, tribes=tribes, seats=seats, record=())
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
        if not match and g['human_start']:
            with open(g['human_start']['file'], 'rb') as f:
                g['st'] = env.load(slot, spec, f.read())
        else:
            g['st'] = env.new(slot, spec)
        g['start_turn'] = g['st'].turn if not match and g['human_start'] else 0
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
            owner, capital = env.owners(slot)
            dagger.save(args.out_dir, g['rec'], st, owner, capital, g['opp'], g['record'], source='selfplay',
                        extra=dict(v_old=np.asarray(g['rec'].value, np.float32), start_turn=np.int32(g['start_turn']),
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

    def run_slots(i, env, slots):
        """Plays games on these slots of one game process until no jobs are left."""
        live = {}
        for s in slots:
            g = new_game(env, s)
            if g:
                live[s] = g
        while live and not errors:
            slots_now = sorted(live)
            te = time.time()
            obs = env.observe([(s, 0) for s in slots_now])
            counters['env'] += time.time() - te
            acts = {}
            by_key = {}
            for s, o in zip(slots_now, obs):
                g = live[s]
                by_key.setdefault(g['seats'][g['st'].current], []).append((s, o))
            for key, items in by_key.items():
                temp = 0.0 if match else args.temperature
                rows, logp, value = infer.request(key, [o for _, o in items], [live[s]['st'] for s, _ in items], temp)
                for (s, o), a, lp, v in zip(items, rows, logp, value):
                    g = live[s]
                    st = g['st']
                    g['recorded'] = not match and st.current in g['record']
                    if g['recorded']:
                        g['rec'].add(o, st.legal, int(a), st.current)
                        g['rec'].played.append(int(a))
                        g['rec'].logp.append(float(lp))
                        g['rec'].value.append(float(v))
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
                        for lst in (r.tiles, r.globs, r.techs, r.legal, r.labels, r.players, r.played, r.logp, r.value):
                            lst.pop()
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


if __name__ == '__main__':
    main()
