"""Turn planning with the engine (research log S6): does choosing among whole candidate turns beat the
raw network?

The planning side (A) starts each of its turns by copying the game into N scratch slots (the engine's
own save and load). In each slot it plays out a complete turn with its policy: plan 1 takes the top
move every time, the others sample at --temperature. A plan ends when the policy chooses End Turn
(not yet played) or the game ends. Each plan is scored by A's own win estimate (the value head) at
that moment: the position after the plan, from A's view, before handing over the turn. The best
plan's position is loaded into the real game and its End Turn played.

Side B is a network playing greedily without planning (by default the same checkpoint), or the
game's AI (bot:<difficulty>). Each seed is played twice with the seats swapped; tribes are drawn
from the seed as in py/selfplay.py. Games run in --procs worker processes, each with its own engine
process and networks.

    POLYHOST=qemu python turn_search.py gen047.pt [--b gen047.pt | --b bot:3] [--plans 8] [--seeds 50]
"""
import argparse
import json
import multiprocessing as mp
import time

import numpy as np

TURN_CAP = 150
COMMANDS_PER_TURN_CAP = 400


def worker(args, jobs, port, out_q):
    import torch

    import eval_vs_bot
    import fast_infer
    import polyenv
    from selfplay import REGULAR_TRIBES, load_net
    end_turn = {v: k for k, v in polyenv.COMMAND_TYPES.items()}['EndTurn']
    dev = 'cuda' if torch.cuda.is_available() else 'cpu'
    net_a = fast_infer.FastPolicy(load_net(args.net, dev), dev, bf16=False)
    bot = int(args.b.split(':')[1]) if args.b.startswith('bot:') else None
    net_b = net_a if args.b == args.net else (None if bot is not None else fast_infer.FastPolicy(load_net(args.b, dev), dev, bf16=False))
    rng = np.random.default_rng(port)

    def infer(net, slots, sts, obs):
        logits, v = net(eval_vs_bot.batch_inputs(obs, [sts[s] for s in slots]))
        return logits.float().cpu().numpy(), torch.sigmoid(v.float()).cpu().numpy()

    def search_turn(env, spec, a_id, stats):
        blob = env.save(0)
        sts = {i: env.load(i, spec, blob) for i in range(1, args.plans + 1)}
        active, value, n = set(sts), {}, {i: 0 for i in sts}
        while active:
            ids = sorted(active)
            obs = env.observe([(i, 0) for i in ids])
            logits, v = infer(net_a, ids, sts, obs)
            acts = []
            for k, i in enumerate(ids):
                row = logits[k][:len(sts[i].legal)]
                if i == 1:
                    a = int(np.argmax(row))
                else:
                    z = (row - row.max()) / args.temperature
                    p = np.exp(z) / np.exp(z).sum()
                    a = int(rng.choice(len(p), p=p))
                n[i] += 1
                if sts[i].legal[a][0] == end_turn or n[i] > COMMANDS_PER_TURN_CAP:
                    value[i] = float(v[k])
                    active.discard(i)
                else:
                    acts.append((i, a))
            for (i, _), (ok, st) in zip(acts, env.step(acts) if acts else []):
                sts[i] = st
                if st.ended:  # the plan ended the game
                    value[i] = 1.0 if st.winner == a_id else 0.0
                    active.discard(i)
        best = max(value, key=lambda i: value[i])
        stats['planned'] += 1
        stats['greedy_best'] += best == 1
        stats['gain'] += value[best] - value[1]
        stats['spread'] += max(value.values()) - min(value.values())
        st = env.load(0, spec, env.save(best))
        if not st.ended:
            ends = np.nonzero(st.legal[:, 0] == end_turn)[0]
            st = env.step([(0, int(ends[0]))])[0][1] if len(ends) else st
        return st

    with polyenv.launch(port) as env:
        for seed, seat in jobs:
            tribes = tuple(int(t) for t in np.random.default_rng(seed).choice(REGULAR_TRIBES, 2))
            a_id = seat + 1
            bots = (None, None) if bot is None else ((None, bot) if seat == 0 else (bot, None))
            spec = polyenv.Spec(tribes=tribes, bots=bots, seed=seed)
            st = env.new(0, spec)
            stats = dict(planned=0, greedy_best=0, gain=0.0, spread=0.0)
            t0, b_turn, n_b = time.time(), -1, 0
            while not st.ended and st.turn < TURN_CAP:
                if st.current == a_id:
                    st = search_turn(env, spec, a_id, stats)
                else:  # B: one greedy move at a time (a bot seat plays inside the engine)
                    if st.turn != b_turn:
                        b_turn, n_b = st.turn, 0
                    obs = env.observe([(0, 0)])
                    logits, _ = infer(net_b, [0], {0: st}, obs)
                    a = int(np.argmax(logits[0][:len(st.legal)]))
                    n_b += 1
                    ends = np.nonzero(st.legal[:, 0] == end_turn)[0]
                    if n_b > COMMANDS_PER_TURN_CAP and len(ends):  # a safety net, as in py/selfplay.py
                        a = int(ends[0])
                    st = env.step([(0, a)])[0][1]
            res = 'win' if st.ended and st.winner == a_id else 'loss' if st.ended else 'cap'
            out_q.put(dict(seed=seed, seat=seat, res=res, turns=st.turn, seconds=round(time.time() - t0), **stats))


def main():
    import eval_vs_bot
    ap = argparse.ArgumentParser()
    ap.add_argument('net')
    ap.add_argument('--b', default=None, help='side B: a checkpoint (default: the same) or bot:<difficulty>')
    ap.add_argument('--plans', type=int, default=8)
    ap.add_argument('--temperature', type=float, default=1.0)
    ap.add_argument('--seeds', type=int, default=50)
    ap.add_argument('--first_seed', type=int, default=16_000_000)
    ap.add_argument('--procs', type=int, default=6)
    ap.add_argument('--port', type=int, default=9700)
    ap.add_argument('--out', default=None, help='results, one JSON line per game')
    args = ap.parse_args()
    args.b = args.b or args.net
    jobs = [(args.first_seed + k, seat) for k in range(args.seeds) for seat in (0, 1)]
    ctx = mp.get_context('spawn')
    q = ctx.Queue()
    ps = [ctx.Process(target=worker, args=(args, jobs[i::args.procs], args.port + i, q)) for i in range(args.procs)]
    for p in ps:
        p.start()
    results, t0 = [], time.time()
    out = open(args.out, 'a') if args.out else None
    while len(results) < len(jobs):
        if not any(p.is_alive() for p in ps) and q.empty():
            break
        try:
            r = q.get(timeout=30)
        except Exception:
            continue
        results.append(r)
        if out:
            out.write(json.dumps(r) + '\n')
            out.flush()
        w = sum(x['res'] == 'win' for x in results)
        print(f"{len(results)}/{len(jobs)} games: planner won {w} ({w / len(results):.1%}); "
              f"best plan = top-move plan on {sum(x['greedy_best'] for x in results) / max(1, sum(x['planned'] for x in results)):.0%} of turns, "
              f"mean estimate gain {sum(x['gain'] for x in results) / max(1, sum(x['planned'] for x in results)):.3f}, "
              f"{time.time() - t0:.0f} s", flush=True)
    for p in ps:
        p.join()
    n = len(results)
    w = sum(r['res'] == 'win' for r in results)
    lo, hi = eval_vs_bot.wilson(w, n)
    print('SUMMARY ' + json.dumps(dict(a=args.net, b=args.b, plans=args.plans, temperature=args.temperature, games=n, a_wins=w,
                                       a_rate=w / max(n, 1), ci=[lo, hi], capped=sum(r['res'] == 'cap' for r in results),
                                       seconds=round(time.time() - t0))), flush=True)


if __name__ == '__main__':
    main()
