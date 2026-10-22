"""Per-command-type agreement of a trained policy with the AI on held-out recorded decisions.

    .venv/bin/python py/diag_confusion.py <checkpoint> <data_dir> [games]
"""
import collections
import glob
import os
import sys

import numpy as np
import torch

import eval_vs_bot as E
import polyenv
import train_bc as T

ck, data = sys.argv[1], sys.argv[2]
n_games = int(sys.argv[3]) if len(sys.argv) > 3 else 20
dev = 'cuda' if torch.cuda.is_available() else 'mps' if torch.backends.mps.is_available() else 'cpu'
net, step = E.load_policy(ck, dev)
files = [f for f in sorted(glob.glob(os.path.join(data, 'g*.npz'))) if int(os.path.basename(f)[1:9]) % 50 == 0][:n_games]
by_type = collections.Counter()
right = collections.Counter()
confused = collections.Counter()
end_early = 0
for f in files:
    g = T.load_game(f)
    D = len(g['label'])
    for i in range(0, D, 256):
        picks = [(0, d) for d in range(i, min(D, i + 256))]
        b = T.collate([g], picks)
        logits = E.policy_logits(net, dev, {k: b[k] for k in ('tiles', 'globals', 'techs', 'legal', 'mask')})
        pred = logits.argmax(1)
        for j, (_, d) in enumerate(picks):
            lt = int(b['legal'][j, b['label'][j], 0])
            pt = int(b['legal'][j, pred[j], 0])
            name = polyenv.COMMAND_TYPES.get(lt, str(lt))
            by_type[name] += 1
            if pred[j] == b['label'][j]:
                right[name] += 1
            else:
                confused[(name, polyenv.COMMAND_TYPES.get(pt, str(pt)))] += 1
                end_early += pt == 15
total = sum(by_type.values())
print(f'step {step}: {total} held-out decisions from {len(files)} games, top-1 {sum(right.values()) / total:.1%}')
print(f'{"AI chose":22s} {"share":>7s} {"agree":>7s}')
for name, n in by_type.most_common():
    print(f'{name:22s} {n / total:7.1%} {right[name] / n:7.1%}')
print('\nmost common disagreements (AI chose -> model chose):')
for (a, b), n in confused.most_common(12):
    print(f'  {a:20s} -> {b:20s} {n / total:6.2%}')
print(f'\nmodel ended the turn while the AI still acted: {end_early / total:.1%} of decisions')
