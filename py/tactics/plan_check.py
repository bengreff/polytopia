"""Planner v2 on fixed states, deterministically, as py/selfplay.py --search_v2 runs it: for checking that a
speed change leaves the plans unchanged (docs/design/throughput.md).

Each state (a real game saved at the start of a planned turn; file name s<k>_<seed>_<tribe1>_<tribe2>.state)
is planned once through selfplay's batched Inference (the learner 'A' in bf16, the critic 'C' in fp32), with
its own rng seed and no time budget (planner2's `budget` only skips repairs on slow turns, so it would tie
the plans to the machine's speed). One thread, so every inference batch is exactly one request and the run
is repeatable. Writes one JSON line per state: the commands played, the planner's info and every
threat-checked lane's score; two runs (old and new code) should give identical lines apart from times.

    POLYHOST=linux POLYHOST_BIN=~/polylinux/polyhost-cp python -m tactics.plan_check --states DIR \
        --net gen107.pt --scorer critic_g107.pt --port 11150 --out a.jsonl
    python -m tactics.plan_check --compare a.jsonl b.jsonl
"""
import argparse
import collections
import json
import os
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

SKIP = ('seconds', 'times', 'ops', 'dedup')  # what may differ between two runs (time, engine requests made)


def compare(a_path, b_path):
    a = {r['name']: r for r in map(json.loads, open(a_path))}
    b = {r['name']: r for r in map(json.loads, open(b_path))}
    names = sorted(set(a) & set(b))
    same_play = sum(a[n]['played'] == b[n]['played'] for n in names)
    same_all = sum({k: v for k, v in a[n].items() if k not in SKIP} == {k: v for k, v in b[n].items() if k not in SKIP}
                   for n in names)
    print(f'{len(names)} states in both: same commands played {same_play}, identical in every field but times {same_all}')
    for n in names:
        if a[n]['played'] != b[n]['played']:
            print('  played differs:', n, 'best lane', a[n]['info']['best'], b[n]['info']['best'],
                  'scores', a[n]['info']['best_score'], b[n]['info']['best_score'])
    ta, tb = sum(a[n]['seconds'] for n in names), sum(b[n]['seconds'] for n in names)
    print(f'planner seconds: {ta:.1f} vs {tb:.1f} ({tb / max(ta, 1e-9):.2f}x)')
    for side, r in (('a', a), ('b', b)):
        ops = collections.Counter()
        for n in names:
            ops.update(r[n].get('ops', {}))
        print(f'{side} engine requests per state:', {k: round(v / len(names), 1) for k, v in sorted(ops.items())})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--states')
    ap.add_argument('--net')
    ap.add_argument('--scorer')
    ap.add_argument('--opts', default='{}', help='planner2 options (JSON); budget defaults to 1e9 here')
    ap.add_argument('--plans', type=int, default=16)
    ap.add_argument('--samples', type=int, default=2)
    ap.add_argument('--temperature', type=float, default=0.5)
    ap.add_argument('--max', type=int, default=10 ** 9)
    ap.add_argument('--port', type=int, default=11150)
    ap.add_argument('--device', default='cuda')
    ap.add_argument('--out')
    ap.add_argument('--compare', nargs=2, default=None)
    args = ap.parse_args()
    if args.compare:
        return compare(*args.compare)
    import torch

    import fast_infer
    import polyenv
    import search_v2
    import selfplay
    from tactics import planner2
    dev = args.device
    if dev == 'cuda' and not torch.cuda.is_available():
        raise SystemExit('CUDA is not available (pass --device cpu to run on the CPU: slow, and other numbers)')
    nets = {'A': selfplay.load_net(args.net, dev), 'C': selfplay.load_net(args.scorer, dev)}
    infer = selfplay.Inference(nets, dev, bf16=True)  # as selfplay's training games
    infer.fast['C'] = fast_infer.FastPolicy(nets['C'], dev, bf16=False)
    infer.checked.add('C')
    opts = dict(dict(budget=1e9), **json.loads(args.opts))
    names = sorted(n for n in os.listdir(args.states) if n.endswith('.state'))[:args.max]
    ops = collections.Counter()
    orig = polyenv.Env.request

    def counted(self, op, payload=b''):
        ops[op] += 1
        if op == 2:  # slots stepped (u32 count, then 8 bytes per slot)
            ops['step_slots'] += (len(payload) - 4) // 8
        elif op == 3:  # positions observed (u32 count, then 5 bytes per request)
            ops['observed'] += (len(payload) - 4) // 5
        return orig(self, op, payload)

    polyenv.Env.request = counted
    out = open(args.out, 'w')
    with polyenv.launch(args.port) as raw:
        env = selfplay.Locked(raw)
        for i, n in enumerate(names):
            _, seed, t1, t2 = n[:-len('.state')].split('_')
            spec = polyenv.Spec(tribes=(int(t1), int(t2)), bots=(None, None), seed=int(seed))
            blob = open(os.path.join(args.states, n), 'rb').read()
            env.load(0, spec, blob)
            rng = np.random.default_rng([int(seed), i])
            agent = search_v2.Agent(env, infer, args.plans, args.samples, args.temperature, rng)
            ops.clear()
            t0 = time.time()
            v2 = planner2.V2(agent, spec, env.save(0), opts, search_v2.OFFSET)
            played, info = v2.run()
            dt = time.time() - t0
            nets_plans = [tuple(map(tuple, v2.plans[k])) for k in range(args.plans) if k in v2.plans]
            rec = dict(name=n, played=[list(map(int, c)) for c in played], seconds=round(dt, 3),
                       info={k: v for k, v in info.items() if k not in SKIP},
                       scores={str(k): v2.combined(k) for k in sorted(v2.checked)},
                       greedy={f'{d},{k}': v for (d, k), v in sorted(v2.g.items())},
                       unique_net_lanes=len(set(nets_plans)), ops={polyenv_op(o): c for o, c in sorted(ops.items(), key=lambda x: str(x[0]))},
                       dedup=dict(dup=getattr(v2, 'n_dup', None), split=getattr(v2, 'n_split', None)))
            out.write(json.dumps(rec) + '\n')
            out.flush()
            print(i, n, f'{dt:.2f} s', info['kind'], 'lanes', info['lanes'], 'unique net lanes', rec['unique_net_lanes'], flush=True)


def polyenv_op(o):
    return {1: 'new', 2: 'step', 3: 'observe', 5: 'save', 6: 'load', 14: 'determinize'}.get(o, str(o))


if __name__ == '__main__':
    main()
