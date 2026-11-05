"""Planner against planner (the v2 gate: planner v2 vs v1), or a planner against a network or CrazyPlus.

Each seed is played twice with the seats swapped; tribes are drawn from the seed as in
py/turn_search.py. Side A and side B are each one of:
- v1: plan_agent.Planner as Ben plays it (16 plans, 2 fog-safe samples, the greedy reply, the critic);
- v2: the same planner with tactics (plan_agent.Planner(tactical=...), py/tactics/planner2.py),
  --a_opts / --b_opts overriding planner2.DEFAULTS;
- net:<checkpoint>: the network playing greedily from its own view (no planning);
- crazyplus: the CrazyPlus bot (py/crazyplus, full view, as in the S10 evaluations).
Both planners use the same --net (policy and reply model) and --scorer; a planner's turn is planned
on a saved copy of the game (exactly as agent_server.py does for Ben), and the commands it returns
are played in the game.

    POLYHOST=linux POLYHOST_BIN=~/polylinux/polyhost-cp python -m tactics.match --a v2 --b v1 \
        --net s9v/gen083.pt --scorer s9v/critic_g083.pt --seeds 200 --first_seed 40000000 --procs 4 --port 10200 --out r.jsonl
"""
import argparse
import copy
import json
import multiprocessing as mp
import os
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

TURN_CAP = int(os.environ.get('TAC_TURN_CAP', 150))  # (a smaller cap only for quick tests)
VERBOSE = bool(os.environ.get('TAC_VERBOSE'))
GAME = 900  # the game's slot (CrazyPlus uses GAME + 1 as its scratch slot)
END_TURN = 15


def tribes_for(seed):
    import polyenv
    from selfplay import REGULAR_TRIBES
    return tuple(int(t) for t in np.random.default_rng(seed).choice(REGULAR_TRIBES, 2))


def find(st, row):
    hit = np.nonzero((st.legal == np.asarray(row, st.legal.dtype)).all(1))[0] if len(st.legal) else []
    return int(hit[0]) if len(hit) else -1


class Side:
    def __init__(self, kind, opts, base, env, device, rng_seed):
        """base: a v1/v2 Planner whose networks are shared (None: load them)."""
        import eval_vs_bot
        import plan_agent
        self.kind, self.env = kind, env
        self.stats = dict(turns=0, seconds=0.0, max_seconds=0.0, kinds={}, tac_played=0, repairs_played=0, broken=0)
        self.times = []
        if kind in ('v1', 'v2'):
            self.p = copy.copy(base)
            self.p.tactical = (opts or {}) if kind == 'v2' else None
            self.p.rng = np.random.default_rng(rng_seed)
        elif kind.startswith('net:'):
            self.net, _ = eval_vs_bot.load_policy(kind[4:], device)
            self.device = device
        elif kind != 'crazyplus':
            raise SystemExit(f'unknown side {kind}')

    def start(self, spec, me):
        if self.kind == 'crazyplus':
            from crazyplus.bot import CrazyPlus
            self.bot = CrazyPlus(self.env, GAME, spec, me, None)

    def turn(self, spec, st):
        """Plays this side's whole turn in the game slot; returns the game's status after it."""
        import eval_vs_bot
        import polyenv
        env = self.env
        me = st.current
        t0 = time.time()
        if self.kind in ('v1', 'v2'):
            played, info = self.p.plan_turn(spec, env.save(GAME))
            for r in played:
                k = find(st, r)
                if k < 0:
                    raise polyenv.EnvError(f'planned command {r} not legal in the game')
                st = env.step([(GAME, k)])[0][1]
                if st.ended or st.current != me:
                    break
            k_ = info.get('kind', 'net')
            self.stats['kinds'][k_] = self.stats['kinds'].get(k_, 0) + 1
            self.stats['broken'] += bool(info.get('broken'))
            if VERBOSE:
                print(f"  {self.kind} p{me} turn {info.get('turn')}: {info.get('seconds')} s, lane {k_}, "
                      f"threat {info.get('threat_loss')}, repairs {len(info.get('repairs', []))}, {info.get('times', '')}", flush=True)
        elif self.kind == 'crazyplus':
            from crazyplus import sim
            n = 0
            while not st.ended and st.current == me and n < 400:
                ok, st = sim.play(env, GAME, self.bot.act(st))
                n += 1
        else:
            n = 0
            while not st.ended and st.current == me:
                obs = env.observe([(GAME, me)])[0]
                a = int(eval_vs_bot.choose(self.net, self.device, [obs], [st], 0.0, None)[0])
                n += 1
                e = np.nonzero(st.legal[:, 0] == END_TURN)[0]
                if n > 400 and len(e):
                    a = int(e[0])
                st = env.step([(GAME, a)])[0][1]
        if not st.ended and st.current == me:  # a planner that ran out of commands: end the turn
            e = np.nonzero(st.legal[:, 0] == END_TURN)[0]
            if len(e):
                st = env.step([(GAME, int(e[0]))])[0][1]
        dt = time.time() - t0
        self.stats['turns'] += 1
        self.stats['seconds'] += dt
        self.stats['max_seconds'] = max(self.stats['max_seconds'], dt)
        self.times.append(dt)
        return st


def worker(args, jobs, port, out_q, device=None):
    if sys.platform.startswith('linux'):  # die with the main process (a stopped run leaves no workers)
        import ctypes
        import signal
        ctypes.CDLL('libc.so.6', use_errno=True).prctl(1, signal.SIGTERM)
    import torch

    import plan_agent
    import polyenv
    dev = device or args.device or ('cuda' if torch.cuda.is_available() else 'mps' if torch.backends.mps.is_available() else 'cpu')
    if dev == 'cpu':
        torch.set_num_threads(args.cpu_threads)
    a_opts = json.loads(args.a_opts) if args.a_opts else {}
    b_opts = json.loads(args.b_opts) if args.b_opts else {}
    todo = list(jobs)
    while todo:
        with polyenv.launch(port) as env:  # a game process that dies is started again for the next game
            base = None
            if any(k in ('v1', 'v2') for k in (args.a, args.b)):
                base = plan_agent.Planner(env, args.net, args.scorer, dev, args.plans, args.samples, args.temperature)
                if not args.graphs:  # no CUDA graphs: less GPU memory per process (the same numbers, a little slower)
                    for f in (base.net, base.model, base.scorer):
                        f.graphs = False
            while todo:
                seed, seat = todo.pop(0)
                tribes = tribes_for(seed)
                spec = polyenv.Spec(tribes=tribes, bots=(None, None), seed=seed)
                a_id = seat + 1
                A = Side(args.a, a_opts, base, env, dev, seed * 2 + 0)
                B = Side(args.b, b_opts, base, env, dev, seed * 2 + 1)
                st = env.new(GAME, spec)
                A.start(spec, a_id)
                B.start(spec, 3 - a_id)
                t0 = time.time()
                try:
                    while not st.ended and st.turn < TURN_CAP:
                        st = (A if st.current == a_id else B).turn(spec, st)
                except Exception as ex:  # (an engine error or a bug): logged, then a fresh engine for the next game
                    import traceback
                    out_q.put(dict(seed=seed, seat=seat, res='error', error=(str(ex) + ' ' + traceback.format_exc()[-600:])[:900]))
                    break
                res = 'win' if st.ended and st.winner == a_id else 'loss' if st.ended else 'cap'
                pct = lambda t: [round(float(np.percentile(t, q)), 2) for q in (50, 90, 100)] if t else []
                out_q.put(dict(seed=seed, seat=seat, tribes=tribes, res=res, turns=st.turn, seconds=round(time.time() - t0),
                               a=args.a, b=args.b, a_stats=A.stats, b_stats=B.stats, a_turn_s=pct(A.times), b_turn_s=pct(B.times)))


def wilson(w, n, z=1.96):
    if n == 0:
        return 0.0, 0.0
    p = w / n
    d = 1 + z * z / n
    c = p + z * z / (2 * n)
    r = z * ((p * (1 - p) / n + z * z / (4 * n * n)) ** 0.5)
    return (c - r) / d, (c + r) / d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--a', default='v2')
    ap.add_argument('--b', default='v1')
    ap.add_argument('--a_opts', default=None, help='v2: JSON overriding planner2.DEFAULTS')
    ap.add_argument('--b_opts', default=None)
    ap.add_argument('--net', required=True)
    ap.add_argument('--scorer', required=True)
    ap.add_argument('--plans', type=int, default=16)
    ap.add_argument('--samples', type=int, default=2)
    ap.add_argument('--temperature', type=float, default=0.5)
    ap.add_argument('--seeds', type=int, default=10)
    ap.add_argument('--first_seed', type=int, default=40_000_000)
    ap.add_argument('--procs', type=int, default=1)
    ap.add_argument('--port', type=int, default=10200)
    ap.add_argument('--out', default=None)
    ap.add_argument('--device', default=None)
    ap.add_argument('--cpu_procs', type=int, default=0, help='extra workers that run the networks on the CPU')
    ap.add_argument('--cpu_threads', type=int, default=3)
    ap.add_argument('--graphs', action='store_true', help='CUDA graphs for inference (more GPU memory per process)')
    args = ap.parse_args()
    jobs = [(args.first_seed + k, seat) for k in range(args.seeds) for seat in (0, 1)]
    done = set()
    if args.out and os.path.exists(args.out):  # resume: skip the games already in the results file
        for line in open(args.out):
            r = json.loads(line)
            if r.get('res') != 'error':
                done.add((r['seed'], r['seat']))
    jobs = [j for j in jobs if j not in done]
    ctx = mp.get_context('spawn')
    q = ctx.Queue()
    n = args.procs + args.cpu_procs  # --cpu_procs: more workers with CPU inference (the engine, not the GPU, limits a game)
    ps = [ctx.Process(target=worker, args=(args, jobs[i::n], args.port + i, q, 'cpu' if i >= args.procs else None)) for i in range(n)]
    for p in ps:
        p.start()
    results = []
    if args.out and os.path.exists(args.out):
        results = [json.loads(line) for line in open(args.out)]
        results = [r for r in results if r.get('res') != 'error']
    out = open(args.out, 'a') if args.out else None
    t0 = time.time()
    n_new = 0
    while n_new < len(jobs):
        if not any(p.is_alive() for p in ps) and q.empty():
            break
        try:
            r = q.get(timeout=30)
        except Exception:
            continue
        n_new += 1
        if out:
            out.write(json.dumps(r) + '\n')
            out.flush()
        if r.get('res') == 'error':
            print('ERROR', r, flush=True)
            continue
        results.append(r)
        w = sum(x['res'] == 'win' for x in results)
        print(f"{len(results)} games: A ({args.a}) won {w} ({w / len(results):.1%}); last {r['res']} in {r['turns']} turns, "
              f"{r['seconds']} s, A turn s p50/p90/max {r['a_turn_s']}, {time.time() - t0:.0f} s", flush=True)
    for p in ps:
        p.join()
    n = len(results)
    w = sum(r['res'] == 'win' for r in results)
    lo, hi = wilson(w, n)
    print('SUMMARY ' + json.dumps(dict(a=args.a, b=args.b, a_opts=args.a_opts, b_opts=args.b_opts, net=args.net, scorer=args.scorer,
                                       first_seed=args.first_seed, games=n, a_wins=w, a_rate=w / max(n, 1), ci=[lo, hi],
                                       capped=sum(r['res'] == 'cap' for r in results),
                                       a_wins_by_seat=[sum(r['res'] == 'win' for r in results if r['seat'] == s) for s in (0, 1)],
                                       seconds=round(time.time() - t0))), flush=True)


if __name__ == '__main__':
    main()
