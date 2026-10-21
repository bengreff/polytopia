"""Plays a trained policy against the game's built-in AI (the M1 gate: raw policy vs Normal).

Championship games (Might, Lakes, Small). Each seed is played twice, on the same map and tribes,
with the agent and the bot trading seats. The agent's seat is a person's seat (handicap 1); the
bot is the game's AI at the given difficulty. The policy plays greedily (or samples with
--temperature), with no search.

    .venv/bin/python py/eval_vs_bot.py <checkpoint.pt> --seeds 200 --difficulty 1 --procs 3 --slots 8
"""
import argparse
import concurrent.futures as cf
import contextlib
import math
import time

import numpy as np
import torch

import model as M
import polyenv

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
    t = {k: torch.from_numpy(v).to(device) for k, v in inputs.items()}
    logits, _, _ = net(t['tiles'], t['globals'], t['techs'], t['legal'], t['mask'])
    return logits.float().cpu().numpy()


def choose(net, device, obs, statuses, temperature, rng):
    inputs = batch_inputs(obs, statuses)
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
    args = ap.parse_args()
    net, step = load_policy(args.checkpoint, args.device)
    rng = np.random.default_rng(0)
    jobs = []  # (seed, agent seat)
    for k in range(args.seeds):
        jobs += [(args.first_seed + k, 0), (args.first_seed + k, 1)]
    jobs.reverse()
    results = []
    out = open(args.out, 'a') if args.out else None
    with contextlib.ExitStack() as stack:
        envs = [stack.enter_context(polyenv.launch(args.port + i)) for i in range(args.procs)]
        pool = cf.ThreadPoolExecutor(len(envs))
        live = [dict() for _ in envs]  # per env: slot -> dict(job, status, turn commands)

        def start(e, slot):
            seed, seat = jobs.pop()
            tribes = tuple(int(t) for t in np.random.default_rng(seed).choice(REGULAR_TRIBES, 2))
            bots = (None, args.difficulty) if seat == 0 else (args.difficulty, None)
            st = envs[e].new(slot, polyenv.Spec(tribes=tribes, bots=bots, seed=seed))
            live[e][slot] = dict(seed=seed, seat=seat, tribes=tribes, st=st, turn=st.turn, n=0)

        for e in range(len(envs)):
            for s in range(args.slots):
                if jobs:
                    start(e, s)
        t0 = time.time()
        while any(live):
            # Finish games that are over, start new ones.
            for e in range(len(envs)):
                for s, g in list(live[e].items()):
                    st = g['st']
                    if st.ended or st.turn >= TURN_CAP:
                        agent_id = g['seat'] + 1
                        res = 'win' if st.ended and st.winner == agent_id else 'loss' if st.ended else 'cap'
                        results.append((g['seed'], g['seat'], g['tribes'], res, st.turn))
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
            order = [sorted(l) for l in live]
            if not any(order):
                break
            obs = list(pool.map(lambda e: envs[e].observe([(s, 0) for s in order[e]]) if order[e] else [], range(len(envs))))
            flat = [(e, s) for e in range(len(envs)) for s in order[e]]
            flat_obs = [o for e in range(len(envs)) for o in obs[e]]
            statuses = [live[e][s]['st'] for e, s in flat]
            acts = choose(net, args.device, flat_obs, statuses, args.temperature, rng)
            per_env = [[] for _ in envs]
            for (e, s), a in zip(flat, acts):
                g = live[e][s]
                if g['st'].turn != g['turn']:
                    g['turn'], g['n'] = g['st'].turn, 0
                g['n'] += 1
                if g['n'] > COMMANDS_PER_TURN_CAP:  # never seen; a safety net against a stuck policy
                    a = int(np.nonzero(g['st'].legal[:, 0] == 15)[0][0])
                per_env[e].append((s, int(a)))
            stepped = list(pool.map(lambda e: envs[e].step(per_env[e]) if per_env[e] else [], range(len(envs))))
            for e in range(len(envs)):
                for (s, _), (ok, st) in zip(per_env[e], stepped[e]):
                    if not ok:
                        raise polyenv.EnvError(f'env {e} slot {s}: legal command rejected')
                    live[e][s]['st'] = st
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
