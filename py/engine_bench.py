"""Engine-only throughput (radical-ideas.md B3): how many decisions per second the game processes
can serve with no network in the loop. Each worker process runs one polyhost env with --slots
games, and every round observes all of them and plays a random legal row in each (End Turn with
probability --end_turn, so games move on). A game that ends or reaches turn 60 is replaced by a
new one. Self-play's real rate (py/selfplay.py's decisions/s) divided by this is how much of the
time the network costs.

    POLYHOST=linux python engine_bench.py --procs 28 --slots 6 --seconds 120 --port 9450
"""
import argparse
import multiprocessing as mp
import time

import numpy as np

import polyenv


def worker(args, i, out):
    rng = np.random.default_rng(1000 + i)
    with polyenv.launch(args.port + i) as env:
        spec = lambda s: polyenv.Spec(tribes=tuple(int(t) for t in rng.choice(sorted(polyenv.TRIBES.values()), 2)), seed=s)
        sts = {s: env.new(s, spec(i * 1000 + s)) for s in range(args.slots)}
        n, t_obs, t0 = 0, 0.0, time.time()
        while time.time() - t0 < args.seconds:
            t1 = time.time()
            env.observe([(s, 0) for s in sts])
            t_obs += time.time() - t1
            acts = []
            for s, st in sts.items():
                k = len(st.legal) - 1 if rng.random() < args.end_turn else int(rng.integers(len(st.legal)))
                acts.append((s, k))
            for (s, _), (ok, st) in zip(acts, env.step(acts)):
                sts[s] = st
                if st.ended or st.turn >= 60:
                    sts[s] = env.new(s, spec(int(rng.integers(1 << 30))))
            n += len(acts)
        out.put((n, time.time() - t0, t_obs))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--procs', type=int, default=28)
    ap.add_argument('--slots', type=int, default=6)
    ap.add_argument('--seconds', type=float, default=120)
    ap.add_argument('--end_turn', type=float, default=0.08)
    ap.add_argument('--port', type=int, default=9450)
    args = ap.parse_args()
    q = mp.Queue()
    ps = [mp.Process(target=worker, args=(args, i, q)) for i in range(args.procs)]
    for p in ps:
        p.start()
    res = [q.get() for _ in ps]
    for p in ps:
        p.join()
    n = sum(r[0] for r in res)
    el = max(r[1] for r in res)
    obs = sum(r[2] for r in res) / max(sum(r[1] for r in res), 1e-9)
    print(f'{args.procs} processes x {args.slots} slots: {n} decisions in {el:.0f} s = {n / el:.0f} decisions/s '
          f'({n / el / args.procs:.0f} per process); observation share of the time {obs:.0%}')


if __name__ == '__main__':
    main()
