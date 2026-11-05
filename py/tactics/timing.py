"""Planner v2's time per turn on this machine (pre-registered test (d)): v2 plans every turn start of one
seat in a recorded game (states saved by the suite tools, runs/tactics/ben/tNN_pP.state).

    POLYHOST=mac .venv/bin/python py/tactics/timing.py --states runs/tactics/ben --player 2 --port 8961 --out runs/tactics/timing_mac.jsonl
"""
import argparse
import json
import os
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))


def main():
    import torch

    import plan_agent
    import polyenv
    ap = argparse.ArgumentParser()
    ap.add_argument('--states', required=True)
    ap.add_argument('--player', type=int, default=2)
    ap.add_argument('--tribes', default='7,13')
    ap.add_argument('--net', default=os.path.join(polyenv.REPO, 'runs/sp/s9v/gen083.pt'))
    ap.add_argument('--scorer', default=os.path.join(polyenv.REPO, 'runs/sp/s9v/critic_g083.pt'))
    ap.add_argument('--opts', default=None)
    ap.add_argument('--v1', action='store_true', help='time v1 instead')
    ap.add_argument('--port', type=int, default=8961)
    ap.add_argument('--env_bin', default=os.path.join(polyenv.REPO, 'build', 'polyhost-mac-det'))
    ap.add_argument('--out', required=True)
    args = ap.parse_args()
    if polyenv.NATIVE:
        os.environ['POLYHOST_BIN'] = args.env_bin
    dev = 'cuda' if torch.cuda.is_available() else 'mps' if torch.backends.mps.is_available() else 'cpu'
    spec = polyenv.Spec(tribes=tuple(int(x) for x in args.tribes.split(',')), bots=(None, None), seed=0)
    names = sorted(n for n in os.listdir(args.states) if n.endswith(f'_p{args.player}.state'))
    out = open(args.out, 'a')
    secs = []
    with polyenv.launch(args.port) as env:
        tactical = None if args.v1 else (json.loads(args.opts) if args.opts else {})
        P = plan_agent.Planner(env, args.net, args.scorer, dev, 16, 2, 0.5, seed=7, tactical=tactical)
        for n in names:
            blob = open(os.path.join(args.states, n), 'rb').read()
            t0 = time.time()
            played, info = P.plan_turn(spec, blob)
            dt = time.time() - t0
            secs.append(dt)
            info = {k: v for k, v in info.items() if k != 'tactical'}
            out.write(json.dumps(dict(name=n, seconds=round(dt, 2), info=info)) + '\n')
            out.flush()
            print(n, round(dt, 2), info.get('kind'), info.get('threat_loss'), flush=True)
    print('SUMMARY', json.dumps(dict(n=len(secs), median=round(float(np.median(secs)), 2), p90=round(float(np.percentile(secs, 90)), 2),
                                     max=round(float(max(secs)), 2), mean=round(float(np.mean(secs)), 2))), flush=True)


if __name__ == '__main__':
    main()
