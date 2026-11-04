"""WSP-1 counterfactual probes of a critic (research log WSP-1): edit one fact in real turn-start
observations and measure how the critic's estimate moves. CPU, a few threads.

Edits (each on every kept position where it applies):
- stars + k (k in --star_steps), and stars set to 0;
- one researched tech removed / one unresearched tech added (each tech id, averaged);
- one own unit of kind A turned into kind B (health scaled to B's max), or removed.

Prints the mean change in win probability and in logit with standard errors, and writes them as JSON.

    python py/wsp/probes.py pos_sp_obs.npz --critic critic_g083.pt [--edited edited.pt] --out probes.json
"""
import argparse
import json
import os
import sys

import numpy as np
import torch

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import polyenv  # noqa: E402
from positions import load_net, values  # noqa: E402

T = {n: i for i, n in enumerate(polyenv.TILE_FEATURES)}
G = {n: i for i, n in enumerate(polyenv.GLOBAL_FEATURES)}
UNIT = {'Warrior': 2, 'Rider': 3, 'Knight': 4, 'Defender': 5, 'Catapult': 8, 'Archer': 9, 'Swordsman': 11, 'Giant': 12}
MAX_HP = {2: 10, 3: 10, 4: 10, 5: 15, 8: 10, 9: 10, 11: 15, 12: 40}  # the game's base health (x10 in observations)
COST = {2: 2, 3: 3, 4: 8, 5: 3, 8: 8, 9: 3, 11: 5, 12: 10}
TECHS = dict(enumerate(
    'Basic Riding FreeSpirit Chivalry Roads Trade Organization Shields Farming Construction Fishing Whaling Aquatism '
    'Sailing Navigation Hunting Forestry Mathematics Archery Spiritualism Climbing Meditation Philosophy Mining '
    'Smithery FreeDiving Spearing'.split()))
TIER = {1: 1, 6: 1, 10: 1, 15: 1, 20: 1, 2: 2, 4: 2, 7: 2, 8: 2, 12: 2, 13: 2, 16: 2, 18: 2, 21: 2, 23: 2,
        3: 3, 5: 3, 9: 3, 11: 3, 14: 3, 17: 3, 19: 3, 22: 3, 24: 3}


def stat(x):
    x = np.asarray(x, float)
    return dict(n=int(len(x)), mean=float(x.mean()) if len(x) else None,
                se=float(x.std(ddof=1) / np.sqrt(len(x))) if len(x) > 1 else None)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('obs')
    ap.add_argument('--critic', required=True)
    ap.add_argument('--max', type=int, default=3000)
    ap.add_argument('--star_steps', default='-3,1,3,5,10')
    ap.add_argument('--threads', type=int, default=4)
    ap.add_argument('--out', default=None)
    args = ap.parse_args()
    torch.set_num_threads(args.threads)
    z = np.load(args.obs)
    tiles, glob, techs = z['tiles'][:args.max], z['globals'][:args.max], z['techs'][:args.max]
    net = load_net(args.critic)
    base = values(net, tiles, glob, techs)
    pb = 1 / (1 + np.exp(-base))
    res = dict(n=len(base), base_mean=float(pb.mean()))

    def delta(name, idx, t2, g2, k2):
        if not len(idx):
            return
        lg = values(net, t2, g2, k2)
        dl = lg - base[idx]
        dp = 1 / (1 + np.exp(-lg)) - pb[idx]
        res[name] = dict(dp=stat(dp), dlogit=stat(dl))
        r = res[name]
        print(f"{name}: n {r['dp']['n']}, d win prob {r['dp']['mean']:+.4f} ({r['dp']['se'] or 0:.4f}), "
              f"d logit {r['dlogit']['mean']:+.4f} ({r['dlogit']['se'] or 0:.4f})", flush=True)

    S = G['stars']
    for k in [int(x) for x in args.star_steps.split(',')]:
        g2 = glob.copy()
        g2[:, S] = np.maximum(0, g2[:, S] + k)
        idx = np.nonzero(glob[:, S] + k >= 0)[0] if k < 0 else np.arange(len(glob))
        delta(f'stars {k:+d}', idx, tiles[idx], g2[idx], techs[idx])
    g2 = glob.copy(); g2[:, S] = 0
    delta('stars = 0', np.arange(len(glob)), tiles, g2, techs)

    # techs: add / remove one, per tech
    rng = np.random.default_rng(0)
    for tid, name in TECHS.items():
        if tid == 0 or tid > 24:
            continue
        idx = np.nonzero(techs[:, tid] == 0)[0]
        if len(idx) > 400:
            idx = rng.choice(idx, 400, replace=False)
        k2 = techs[idx].copy(); k2[:, tid] = 1
        delta(f'tech +{name} (tier {TIER[tid]})', idx, tiles[idx], glob[idx], k2)
        idx = np.nonzero(techs[:, tid] == 1)[0]
        if len(idx) > 400:
            idx = rng.choice(idx, 400, replace=False)
        k2 = techs[idx].copy(); k2[:, tid] = 0
        delta(f'tech -{name} (tier {TIER[tid]})', idx, tiles[idx], glob[idx], k2)

    # units: one own unit of kind A -> kind B, or removed
    U, O, H, MH = T['unit'], T['unit_owner'], T['unit_health'], T['unit_max_health']
    flat = tiles.reshape(len(tiles), -1, tiles.shape[-1])
    for an, a in UNIT.items():
        hits = [(i, int(np.nonzero((flat[i, :, U] == a + 1) & (flat[i, :, O] == 1))[0][0]))
                for i in range(len(flat)) if ((flat[i, :, U] == a + 1) & (flat[i, :, O] == 1)).any()]
        if len(hits) > 400:
            hits = [hits[j] for j in rng.choice(len(hits), 400, replace=False)]
        if len(hits) < 20:
            continue
        idx = np.array([h[0] for h in hits])
        for bn, b in list(UNIT.items()) + [('removed', None)]:
            if b == a:
                continue
            t2 = flat[idx].copy()
            for j, (_, tile) in enumerate(hits):
                if b is None:  # an empty tile's unit fields are all 0 in observations (unit_home too)
                    t2[j, tile, U:T['unit_home'] + 1] = 0
                else:
                    frac = t2[j, tile, H] / max(1, t2[j, tile, MH])
                    t2[j, tile, U] = b + 1
                    t2[j, tile, MH] = MAX_HP[b] * 10
                    t2[j, tile, H] = int(round(frac * MAX_HP[b] * 10))
            delta(f'unit {an} -> {bn}', idx, t2.reshape(tiles[idx].shape), glob[idx], techs[idx])
    if args.out:
        json.dump(res, open(args.out, 'w'), indent=1)


if __name__ == '__main__':
    main()
