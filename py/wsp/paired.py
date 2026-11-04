"""WSP-1: paired comparison of two selfplay.py --match result files on the same seeds and seats
(per game: 1 if the network won). Prints both win shares and the paired difference with a 95%
interval (normal approximation on the per-game differences).

    python py/wsp/paired.py edited_vs0.tsv base_vs0.tsv
"""
import sys

import numpy as np


def load(p):
    out = {}
    for line in open(p):
        f = line.rstrip('\n').split('\t')
        if len(f) >= 5:
            out[(int(f[0]), int(f[1]))] = 1.0 if f[4] == 'win' else 0.0
    return out


a, b = load(sys.argv[1]), load(sys.argv[2])
keys = sorted(set(a) & set(b))
x = np.array([a[k] - b[k] for k in keys])
m, se = x.mean(), x.std(ddof=1) / np.sqrt(len(x))
print(f'{len(keys)} paired games: {sys.argv[1]} {np.mean([a[k] for k in keys]):.3f}, {sys.argv[2]} '
      f'{np.mean([b[k] for k in keys]):.3f}; difference {m:+.3f} (95% CI {m - 1.96 * se:+.3f} to {m + 1.96 * se:+.3f}); '
      f'discordant {int((x != 0).sum())}')
