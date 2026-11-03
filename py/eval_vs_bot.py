"""Plays a trained policy against the game's built-in AI (the M1 gate: raw policy vs Normal).

Championship games (Might, Lakes, Small). Each seed is played twice, on the same map and tribes,
with the agent and the bot trading seats. The agent's seat is a person's seat (handicap 1); the
bot is the game's AI at the given difficulty. The policy plays greedily (or samples with
--temperature), with no search.

    .venv/bin/python py/eval_vs_bot.py <checkpoint.pt> --seeds 200 --difficulty 1 --procs 3 --slots 8

Strategy conditioning (research log HP1, a network trained with train_bc.py --z): --z_from <human games
dir> gives each game a z drawn from the held-out games of that directory, from a player of the
agent's own tribe whose facts pass --z_filter (a Python expression over py/zfacts.py's fact names plus
T, the game's length in turns, e.g. "techs_t20 >= 9 and T >= 20"). Without --z_from the network gets
no z (unconditioned). --facts_out writes one JSON line per game with the agent's own facts (and the
z's), for the controllability test.
"""
import argparse
import concurrent.futures as cf
import contextlib
import json
import math
import os
import struct
import time

import numpy as np
import torch

import model as M
import polyenv
import remote
import zfacts

REGULAR_TRIBES = sorted(polyenv.TRIBES.values())
TURN_CAP = 150
COMMANDS_PER_TURN_CAP = 400


def wilson(wins, n, z=1.96):
    if n == 0:
        return 0.0, 0.0
    p = wins / n
    d = 1 + z * z / n
    c = p + z * z / (2 * n)
    r = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n))
    return (c - r) / d, (c + r) / d


def load_policy(path, device):
    ck = torch.load(path, map_location='cpu')
    net = M.Net(**ck.get('config', {}))
    net.load_state_dict(ck['model'])
    return net.to(device).eval(), ck.get('step')


def batch_inputs(obs, statuses):
    """The network inputs for live decisions, as numpy arrays (test_parity.py checks these equal
    train_bc.collate's for the same decisions)."""
    N = max(len(s.legal) for s in statuses)
    legal = np.zeros((len(obs), N, 4), np.int16)
    mask = np.zeros((len(obs), N), bool)
    for i, s in enumerate(statuses):
        legal[i, :len(s.legal)] = s.legal
        mask[i, :len(s.legal)] = True
    return dict(tiles=np.stack([o.tiles.reshape(-1, o.tiles.shape[-1]) for o in obs]),
                globals=np.stack([o.globals for o in obs]), techs=np.stack([o.techs for o in obs]),
                legal=legal, mask=mask)


@torch.no_grad()
def policy_logits(net, device, inputs):
    if isinstance(net, remote.RemoteNet):  # --remote: the PC's GPU
        return net.infer(inputs)[0]
    t = {k: torch.from_numpy(v).to(device) for k, v in inputs.items()}
    if 'z' in t:
        logits, _, _ = net(t['tiles'], t['globals'], t['techs'], t['legal'], t['mask'], t['z'])
    else:
        logits, _, _ = net(t['tiles'], t['globals'], t['techs'], t['legal'], t['mask'])
    return logits.float().cpu().numpy()


def z_pool(data_dir, expr, split='val'):
    """{tribe: [(game id, player, z vector, facts)]} from the held-out games of data_dir (split 'val'),
    the training games ('train') or all, whose player passes expr (zfacts fact names plus T, the game's
    turns)."""
    import train_bc
    pool = {}
    files = [f for v in ((True,) if split == 'val' else (False,) if split == 'train' else (True, False))
             for f in train_bc.list_games(data_dir, val=v)]
    for f in files:
        z = np.load(f)
        if not train_bc.accepted(z):
            continue
        for p, fx in zfacts.game_facts(z).items():
            names = dict(fx[2], T=int(z['turns']))
            if expr and not eval(expr, {}, names):
                continue
            tribe = int(z['tribes'][p - 1])
            pool.setdefault(tribe, []).append((os.path.basename(f)[:-4], p, zfacts.vector(*fx), names))
    return pool


def choose(net, device, obs, statuses, temperature, rng, z=None):
    inputs = batch_inputs(obs, statuses)
    if z is not None:
        inputs['z'] = z
    logits = policy_logits(net, device, inputs)
    if temperature <= 0:
        return logits.argmax(1)
    out = []
    for row, m in zip(logits, inputs['mask']):
        p = np.exp((row[m] - row[m].max()) / temperature)
        out.append(int(rng.choice(len(p), p=p / p.sum())))
    return np.array(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('checkpoint')
    ap.add_argument('--seeds', type=int, default=200, help='games = 2 x seeds (sides swapped)')
    ap.add_argument('--first_seed', type=int, default=10_000_000, help='kept apart from training seeds')
    ap.add_argument('--difficulty', type=int, default=1, help='0 Easy, 1 Normal, 2 Hard, 3 Crazy')
    ap.add_argument('--procs', type=int, default=3)
    ap.add_argument('--slots', type=int, default=8)
    ap.add_argument('--port', type=int, default=8300)
    ap.add_argument('--temperature', type=float, default=0.0)
    ap.add_argument('--device', default='mps' if torch.backends.mps.is_available() else 'cpu')
    ap.add_argument('--out', default=None, help='results .tsv')
    ap.add_argument('--cells', type=int, default=0,
                    help='M4 gate layout instead of random tribes: this many games for every (agent tribe, bot tribe, '
                         'agent seat) cell, 144 x 2 cells, seeds from 11,000,000')
    ap.add_argument('--cell_list', default=None,
                    help='with --cells: only these cells, a file of lines "agent_tribe bot_tribe agent_seat(1|2)"; '
                         'seeds from 12,000,000')
    ap.add_argument('--save_games', default=None,
                    help='keep every game for the web app\'s watch mode: <dir>/<seed>_s<seat>.start/.final (GameState '
                         'blobs with the command stack) and one line per game in <dir>/games.jsonl')
    ap.add_argument('--remote', action='store_true', help='run the policy on the PC\'s GPU (py/remote.py)')
    ap.add_argument('--teacher', action='store_true',
                    help='reference: the agent seat plays the game\'s own AI (the M1 teacher); checkpoint is ignored')
    ap.add_argument('--z_from', default=None, help='a z per game from the held-out games of this dir (HP1)')
    ap.add_argument('--z_filter', default='', help='with --z_from: only players whose facts pass this expression')
    ap.add_argument('--z_split', choices=['val', 'train', 'all'], default='val', help='with --z_from: which games')
    ap.add_argument('--facts_out', default=None, help='one JSON line per game: the agent\'s facts (and its z\'s)')
    args = ap.parse_args()
    zpool = z_pool(args.z_from, args.z_filter, args.z_split) if args.z_from else None
    if zpool is not None:
        print(f'z pool: {sum(map(len, zpool.values()))} players from {args.z_from} pass {args.z_filter!r}; '
              f'per tribe {dict(sorted((t, len(v)) for t, v in zpool.items()))}', flush=True)
    facts_out = open(args.facts_out, 'a') if args.facts_out else None
    if args.teacher:
        net, step = None, 'teacher'
    elif args.remote:
        net = remote.RemoteNet(args.checkpoint)
        step = net.step
    else:
        net, step = load_policy(args.checkpoint, args.device)
    rng = np.random.default_rng(0)
    jobs = []  # (seed, agent seat, tribes or None for the seed's own draw)
    if args.cells and args.cell_list:
        seed = 12_000_000
        for line in open(args.cell_list):
            if line.strip():
                ta, tb, s1 = (int(x) for x in line.split())
                for _ in range(args.cells):
                    jobs.append((seed, s1 - 1, (ta, tb) if s1 == 1 else (tb, ta)))
                    seed += 1
    elif args.cells:
        seed = 11_000_000
        for ta in REGULAR_TRIBES:
            for tb in REGULAR_TRIBES:
                for seat in (0, 1):
                    for _ in range(args.cells):
                        jobs.append((seed, seat, (ta, tb) if seat == 0 else (tb, ta)))
                        seed += 1
    else:
        for k in range(args.seeds):
            jobs += [(args.first_seed + k, 0, None), (args.first_seed + k, 1, None)]
    jobs.reverse()
    results = []
    out = open(args.out, 'a') if args.out else None
    with contextlib.ExitStack() as stack:
        envs = [stack.enter_context(polyenv.launch(args.port + i)) for i in range(args.procs)]
        blob_prefix = struct.pack('<i', envs[0].info()['game_version'])  # as the game's server sends states
        tag = os.path.basename(os.path.normpath(args.save_games)) if args.save_games else ''
        if args.save_games:
            os.makedirs(args.save_games, exist_ok=True)

        def save_game(d, g, final, res, turns):
            name = f"{g['seed']}_s{g['seat'] + 1}"
            for ext, blob in (('start', g['start']), ('final', blob_prefix + final)):
                with open(os.path.join(d, f'{name}.{ext}'), 'wb') as f:
                    f.write(blob)
            agent = g['seat'] + 1
            meta = dict(path=f'{tag}/{name}', tag=tag, time=time.strftime('%Y-%m-%d %H:%M:%S'), seed=g['seed'],
                        tribes=list(g['tribes']), players={str(agent): f'network ({os.path.basename(args.checkpoint)})'
                                                           if not args.teacher else "the game's AI (teacher)",
                                                           str(3 - agent): f'bot, difficulty {args.difficulty}'},
                        agent=agent, result=res, turns=turns)
            with open(os.path.join(d, 'games.jsonl'), 'a') as f:
                f.write(json.dumps(meta) + '\n')
        pool = cf.ThreadPoolExecutor(len(envs))
        live = [dict() for _ in envs]  # per env: slot -> dict(job, status, turn commands)

        def start(e, slot):
            seed, seat, tribes = jobs.pop()
            if tribes is None:
                tribes = tuple(int(t) for t in np.random.default_rng(seed).choice(REGULAR_TRIBES, 2))
            bots = (None, args.difficulty) if seat == 0 else (args.difficulty, None)
            st = envs[e].new(slot, polyenv.Spec(tribes=tribes, bots=bots, seed=seed))
            live[e][slot] = dict(seed=seed, seat=seat, tribes=tribes, st=st, turn=st.turn, n=0,
                                 start=blob_prefix + envs[e].save(slot) if args.save_games else None,
                                 rec=[], z=None, z_src=None)
            if zpool is not None:
                cands = zpool.get(int(tribes[seat]), [])
                if cands:
                    gid, p, zv, names = cands[np.random.default_rng(seed * 2 + seat).integers(len(cands))]
                    live[e][slot].update(z=zv, z_src=dict(game=gid, player=p, facts=names))

        for e in range(len(envs)):
            for s in range(args.slots):
                if jobs:
                    start(e, s)
        t0 = time.time()
        # Two groups of slots (even, odd) alternate: while the network chooses for one group on this
        # thread, the game processes step the other group and observe it again on `orch`. Each game
        # process only ever serves one request at a time. Games and results are the same as without
        # the overlap (12 of 12 games identical in the check).
        orch = cf.ThreadPoolExecutor(1)

        def finish(group):
            for e in range(len(envs)):
                for s, g in list(live[e].items()):
                    st = g['st']
                    if s % 2 != group or not (st.ended or st.turn >= TURN_CAP):
                        continue
                    agent_id = g['seat'] + 1
                    res = 'win' if st.ended and st.winner == agent_id else 'loss' if st.ended else 'cap'
                    results.append((g['seed'], g['seat'], g['tribes'], res, st.turn))
                    if facts_out:
                        rec = g['rec']
                        techs, units, fx = zfacts.facts([r[0] for r in rec], [r[1] for r in rec], [r[2] for r in rec],
                                                        [r[3] for r in rec]) if rec else ([], [], {})
                        facts_out.write(json.dumps(dict(seed=g['seed'], seat=g['seat'], tribes=list(map(int, g['tribes'])),
                                                        result=res, turns=st.turn, facts=fx, techs=techs, units=units,
                                                        z=g['z_src'])) + '\n')
                        facts_out.flush()
                    if args.save_games:
                        save_game(args.save_games, g, envs[e].save(s), res, st.turn)
                    if out:
                        out.write(f"{g['seed']}\t{g['seat']}\t{g['tribes'][0]}\t{g['tribes'][1]}\t{res}\t{st.turn}\n")
                        out.flush()
                    del live[e][s]
                    if jobs:
                        start(e, s)
                    n = len(results)
                    if n % 20 == 0:
                        w = sum(r[3] == 'win' for r in results)
                        lo, hi = wilson(w, n)
                        print(f'{n} games: agent won {w} ({w / n:.1%}, 95% CI {lo:.1%}-{hi:.1%}), '
                              f'{sum(r[3] == "cap" for r in results)} capped, {time.time() - t0:.0f} s', flush=True)

        def prep(group):
            """Finishes the group's games, then observes its live slots (and asks the AI, with --teacher)."""
            finish(group)
            order = [sorted(s for s in l if s % 2 == group) for l in live]
            obs = list(pool.map(lambda e: envs[e].observe([(s, 0) for s in order[e]]) if order[e] else [], range(len(envs))))
            bots = list(pool.map(lambda e: envs[e].bot(order[e]) if order[e] else [], range(len(envs)))) if args.teacher else None
            flat = [(e, s) for e in range(len(envs)) for s in order[e]]
            return flat, [o for e in range(len(envs)) for o in obs[e]], \
                [b for e in range(len(envs)) for b in bots[e]] if bots else None

        def act(data):
            flat, flat_obs, bots = data
            if not flat:
                return None
            statuses = [live[e][s]['st'] for e, s in flat]
            zs = None
            if zpool is not None:
                zs = np.stack([live[e][s]['z'] if live[e][s]['z'] is not None else np.zeros(zfacts.Z_DIM, np.float32)
                               for e, s in flat])
            if args.teacher:
                acts = [b if b >= 0 else polyenv.STEP_BOT_OWN for b in bots]
            else:
                acts = choose(net, args.device, flat_obs, statuses, args.temperature, rng, zs)
            if facts_out:
                for (e, s), o, st_, a in zip(flat, flat_obs, statuses, acts):
                    if a >= 0:
                        naval = bool(zfacts.naval_flags(o.tiles.reshape(1, -1, o.tiles.shape[-1]))[0])
                        live[e][s]['rec'].append((o.get('turn'), st_.legal[int(a)].tolist(), o.get('cities'), naval))
            per_env = [[] for _ in envs]
            for (e, s), a in zip(flat, acts):
                g = live[e][s]
                if g['st'].turn != g['turn']:
                    g['turn'], g['n'] = g['st'].turn, 0
                g['n'] += 1
                ends = np.nonzero(g['st'].legal[:, 0] == 15)[0]
                if g['n'] > COMMANDS_PER_TURN_CAP and a != polyenv.STEP_BOT_OWN and len(ends):  # a safety net against a stuck policy
                    a = int(ends[0])
                per_env[e].append((s, int(a)))
            return per_env

        def step_then_prep(group, per_env):
            if per_env is not None:
                stepped = list(pool.map(lambda e: envs[e].step(per_env[e]) if per_env[e] else [], range(len(envs))))
                for e in range(len(envs)):
                    for (s, _), (ok, st) in zip(per_env[e], stepped[e]):
                        if not ok:
                            raise polyenv.EnvError(f'env {e} slot {s}: legal command rejected')
                        live[e][s]['st'] = st
            return prep(group)

        here, group = prep(0), 0
        nxt = orch.submit(prep, 1)
        while True:
            per_env = act(here)
            other = nxt.result()
            if per_env is None and not other[0]:
                break
            nxt = orch.submit(step_then_prep, group, per_env)
            here, group = other, 1 - group
    n = len(results)
    w = sum(r[3] == 'win' for r in results)
    lo, hi = wilson(w, n)
    print(f'\ncheckpoint step {step}, vs difficulty {args.difficulty}: {n} games, agent won {w} ({w / max(n, 1):.1%}, '
          f'95% Wilson CI {lo:.1%}-{hi:.1%}), {sum(r[3] == "cap" for r in results)} capped at turn {TURN_CAP}')
    for seat in (0, 1):
        rs = [r for r in results if r[1] == seat]
        print(f'  agent in seat {seat + 1}: won {sum(r[3] == "win" for r in rs)}/{len(rs)}')
    turns = [r[4] for r in results if r[3] != 'cap']
    if turns:
        print(f'  mean game length {np.mean(turns):.1f} turns')


if __name__ == '__main__':
    main()
