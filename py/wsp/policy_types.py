"""WSP-1 Part B audit: which kinds of command the policy over- or under-rates compared with strong
humans at the same positions (held-out human games).

For a random share of the recorded decisions it scores every legal row and keeps, per command group
(End Turn, Research, Train, Build, Upgrade, Move, Attack, Capture, other), the log-sum-exp of the
group's rows, and the recorded move's group. It then fits one shift per group (Move fixed at 0) that
maximizes the likelihood of the recorded groups: p(group) = softmax(lse + shift). A positive shift
means the players choose that kind of command more often than the policy predicts there. The choice
within a group is untouched by such shifts.

    python py/wsp/policy_types.py gen083.pt --data DIR:N[:val] --share 0.15 [--by_stars]
"""
import argparse
import os
import sys

import numpy as np
import torch

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import eval_vs_bot  # noqa: E402
import polyenv  # noqa: E402
import train_bc  # noqa: E402

G = {n: i for i, n in enumerate(polyenv.GLOBAL_FEATURES)}
GROUPS = [('EndTurn', (15,)), ('Research', (8,)), ('Train', (5,)), ('Build', (1,)), ('Upgrade', (16,)), ('Move', (6,)),
          ('Attack', (2,)), ('Capture', (7,))]
REF = 5  # Move


def group_of(types):
    g = np.full(types.shape, len(GROUPS), np.int64)  # other
    for k, (_, ts) in enumerate(GROUPS):
        g[np.isin(types, ts)] = k
    return g


def fit_shifts(lse, lab, iters=300, lr=0.5):
    """lse [N, K] (-inf where absent), lab [N] -> shifts [K] (shift[REF] = 0), by gradient ascent."""
    K = lse.shape[1]
    s = np.zeros(K)
    present = np.isfinite(lse)
    L = np.where(present, lse, -1e9)
    for _ in range(iters):
        z = L + s
        z -= z.max(1, keepdims=True)
        p = np.exp(z) * present
        p /= p.sum(1, keepdims=True)
        onehot = np.zeros_like(p)
        onehot[np.arange(len(lab)), lab] = 1
        g = (onehot - p).mean(0)
        g[REF] = 0
        # per-group step scaled by how often the group is present
        s += lr * g / np.maximum(present.mean(0), 1e-3)
    z = L + s
    z -= z.max(1, keepdims=True)
    p = np.exp(z) * present
    p /= p.sum(1, keepdims=True)
    ll = np.log(p[np.arange(len(lab)), lab] + 1e-12).mean()
    z0 = L - L.max(1, keepdims=True)
    p0 = np.exp(z0) * present
    p0 /= p0.sum(1, keepdims=True)
    ll0 = np.log(p0[np.arange(len(lab)), lab] + 1e-12).mean()
    return s, ll - ll0


@torch.no_grad()
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('checkpoint')
    ap.add_argument('--data', required=True)
    ap.add_argument('--share', type=float, default=0.15)
    ap.add_argument('--threads', type=int, default=4)
    ap.add_argument('--batch', type=int, default=64)
    ap.add_argument('--out', default=None)
    args = ap.parse_args()
    torch.set_num_threads(args.threads)
    net, _ = eval_vs_bot.load_policy(args.checkpoint, 'cpu')
    parts = args.data.split(':')
    d, lim, val = parts[0], int(parts[1]) if len(parts) > 1 and parts[1] else 0, 'val' in parts[2:]
    files = train_bc.list_games(d, val=True) if val else sorted(
        os.path.join(d, f) for f in os.listdir(d) if f.endswith('.npz') and not f.endswith('.tmp.npz'))
    rng = np.random.default_rng(0)
    K = len(GROUPS) + 1
    lses, labs, stars, turns = [], [], [], []
    used = 0
    for f in files:
        if lim and used >= lim:
            break
        g = train_bc.load_game(f)
        if g is None:
            continue
        used += 1
        D = len(g['label'])
        pick = np.nonzero((rng.random(D) < args.share) & (g['label'] >= 0))[0]
        for i in range(0, len(pick), args.batch):
            ps = [(0, int(x)) for x in pick[i:i + args.batch]]
            b = train_bc.collate([g], ps)
            t = {k: torch.from_numpy(v) for k, v in b.items()}
            logits, _, _ = net(t['tiles'], t['globals'], t['techs'], t['legal'], t['mask'])
            logits = logits.float().numpy()
            for j in range(len(ps)):
                m = b['mask'][j]
                gr = group_of(b['legal'][j, m, 0])
                lg = logits[j, m]
                row = np.full(K, -np.inf)
                for k in range(K):
                    if (gr == k).any():
                        row[k] = np.logaddexp.reduce(lg[gr == k])
                lses.append(row)
                labs.append(int(gr[int(b['label'][j])]))
                stars.append(int(b['globals'][j, G['stars']]))
                turns.append(int(b['globals'][j, G['turn']]))
    lse, lab, stars, turns = np.array(lses), np.array(labs), np.array(stars), np.array(turns)
    if args.out:
        np.savez(args.out, lse=lse, lab=lab, stars=stars, turns=turns)
    names = [n for n, _ in GROUPS] + ['other']
    print(f'{args.checkpoint} on {args.data}: {len(lab)} decisions')
    for name, m in (('all', np.ones(len(lab), bool)), ('stars >= 6', stars >= 6), ('turn <= 20', turns <= 20)):
        s, gain = fit_shifts(lse[m], lab[m])
        z = np.where(np.isfinite(lse[m]), lse[m], -1e9)
        z -= z.max(1, keepdims=True)
        p = np.exp(z) * np.isfinite(lse[m])
        p /= p.sum(1, keepdims=True)
        print(f'  {name} (n {m.sum()}, log-likelihood +{gain:.4f} per decision with the shifts):')
        for k, n in enumerate(names):
            print(f'    {n:9s} recorded {np.mean(lab[m] == k):.3f}  policy {p[:, k].mean():.3f}  shift {s[k]:+.2f}')


if __name__ == '__main__':
    main()
