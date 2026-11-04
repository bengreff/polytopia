"""WSP-1 Part B audit: how the policy rates End Turn against what strong humans do, at the same
positions (held-out human games), and in its own games.

For a random share of the recorded decisions it scores every legal row and keeps: the End Turn row's
logit, the log-sum-exp of the other rows, whether the recorded move was End Turn, whether a purchase
(Research, Train, Build, Upgrade) was legal, and the player's stars. Then it fits delta, the shift
of the End Turn logit that maximizes the recorded moves' likelihood (overall and by stars), i.e.
how much the policy overrates ending the turn compared with the players who made those moves.

    python py/wsp/policy_et.py gen083.pt --data DIR:N[:val] --share 0.15 --out et_human.npz
"""
import argparse
import os
import sys
import time

import numpy as np
import torch

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import eval_vs_bot  # noqa: E402
import polyenv  # noqa: E402
import train_bc  # noqa: E402

G = {n: i for i, n in enumerate(polyenv.GLOBAL_FEATURES)}
END_TURN, PURCHASES = 15, (1, 5, 8, 16)


def fit_delta(et, other, is_et, grid=np.linspace(-6, 6, 241)):
    """delta maximizing sum log p(recorded) when the End Turn logit is shifted by delta (2-way: End Turn
    vs the rest, which is exact for the recorded move's probability up to the within-rest share)."""
    best, ll0 = None, None
    for d in grid:
        z = et + d - other
        lp_et = -np.logaddexp(0, -z)
        lp_rest = -np.logaddexp(0, z)
        ll = np.where(is_et, lp_et, lp_rest).sum()
        if abs(d) < 1e-9:
            ll0 = ll
        if best is None or ll > best[1]:
            best = (d, ll)
    return best[0], (best[1] - ll0) / len(et)


@torch.no_grad()
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('checkpoint')
    ap.add_argument('--data', required=True, help='DIR[:N][:val]')
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
    rows = dict(et=[], other=[], is_et=[], purchase=[], stars=[], turn=[], game=[])
    used, t0 = 0, time.time()
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
            for j, (_, dd) in enumerate(ps):
                m = b['mask'][j]
                ty = b['legal'][j, m, 0]
                e = np.nonzero(ty == END_TURN)[0]
                if not len(e):
                    continue
                e = int(e[0])
                lg = logits[j, m]
                others = np.delete(lg, e)
                rows['et'].append(lg[e]); rows['other'].append(np.logaddexp.reduce(others) if len(others) else -1e9)
                rows['is_et'].append(int(b['label'][j]) == e)
                rows['purchase'].append(bool(np.isin(ty, PURCHASES).any()))
                rows['stars'].append(int(b['globals'][j, G['stars']])); rows['turn'].append(int(b['globals'][j, G['turn']]))
                rows['game'].append(used)
        if used % 25 == 0:
            print(f'{used} games, {len(rows["et"])} decisions, {time.time() - t0:.0f} s', flush=True)
    r = {k: np.array(v) for k, v in rows.items()}
    if args.out:
        np.savez(args.out, **r)
    pet = 1 / (1 + np.exp(-(r['et'] - r['other'])))
    print(f'{args.checkpoint} on {args.data}: {len(r["et"])} decisions; recorded End Turn {r["is_et"].mean():.3f}, '
          f'policy P(End Turn) {pet.mean():.3f}')
    for name, m in (('all', np.ones(len(pet), bool)), ('a purchase is legal', r['purchase']),
                    ('stars 0-2', r['stars'] <= 2), ('stars 3-5', (r['stars'] >= 3) & (r['stars'] <= 5)),
                    ('stars 6-9', (r['stars'] >= 6) & (r['stars'] <= 9)), ('stars 10+', r['stars'] >= 10),
                    ('stars 10+, turn <= 20', (r['stars'] >= 10) & (r['turn'] <= 20))):
        if m.sum() < 30:
            continue
        dl, gain = fit_delta(r['et'][m], r['other'][m], r['is_et'][m])
        print(f'  {name}: n {m.sum()}, recorded End Turn {r["is_et"][m].mean():.3f}, policy {pet[m].mean():.3f}, '
              f'best End Turn shift {dl:+.2f} logits (log-likelihood +{gain:.4f} per decision)')


if __name__ == '__main__':
    main()
