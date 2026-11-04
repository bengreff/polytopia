"""WSP-1: re-score the planner's recorded lanes (turn_search.py --dump) with other critics, or with the
original critic's logit plus a correction per fact, and count how often the chosen lane changes.

A lane's score is as in the planner: the mean over its samples of 1/0 if the lane ended the game
(won/lost), else the critic's win probability at A's next turn. The chosen lane is the argmax.

    python py/wsp/rescore.py DUMP_DIR --base critic_g083.pt [--critics name=edited.pt ...]
        [--adjust "stars:-0.02" "ntechs:+0.05" ...] [--out rescore.npz]

--adjust adds coef x fact to the base critic's logit (facts of py/wsp/positions.facts, plus
own_<Unit>, enemy_<Unit>, tech_<Name>), one variant per argument; "name=fact:coef,fact:coef" combines.
"""
import argparse
import glob
import os
import sys

import numpy as np
import torch

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from positions import facts, load_net, values  # noqa: E402

UNIT_NAMES = 'None Scout Warrior Rider Knight Defender Ship Battleship Catapult Archer MindBender Swordsman Giant'.split()
TECH_NAMES = ('Basic Riding FreeSpirit Chivalry Roads Trade Organization Shields Farming Construction Fishing Whaling Aquatism '
              'Sailing Navigation Hunting Forestry Mathematics Archery Spiritualism Climbing Meditation Philosophy Mining '
              'Smithery FreeDiving Spearing').split()


def lane_facts(tiles, glob_, techs):
    f = facts(tiles, glob_, techs)
    out = {k: v.astype(float) for k, v in f.items() if v.ndim == 1}
    for i, n in enumerate(UNIT_NAMES):
        out['own_' + n] = f['own_units'][:, i].astype(float)
        out['enemy_' + n] = f['enemy_units'][:, i].astype(float)
    out['own_total'] = f['own_units'].sum(1).astype(float)
    for i, n in enumerate(TECH_NAMES):
        out['tech_' + n] = techs[:, i].astype(float)
    return out


def load_dumps(d):
    turns = []
    for f in sorted(glob.glob(os.path.join(d, '*.npz'))):
        z = np.load(f)
        if 'tiles' not in z.files:
            continue
        has = z['has_obs']
        ti = 0
        for t in range(len(has)):
            if not has[t]:
                continue
            turns.append(dict(file=os.path.basename(f), turn=int(z['turn'][t]), best=int(z['best'][t]),
                              scores=z['scores'][t], est=z['est'][t], ended=z['ended'][t], won=z['won'][t],
                              tiles=z['tiles'][ti], globals=z['globals'][ti], techs=z['techs'][ti]))
            ti += 1
    return turns


def lane_scores(turn, logit, K):
    """Scores [K] from per-slot logits [K*D] (slot index d*K + k)."""
    p = 1 / (1 + np.exp(-logit))
    v = np.where(turn['ended'], turn['won'].astype(float), p)
    return v.reshape(-1, K).mean(0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dump')
    ap.add_argument('--base', required=True)
    ap.add_argument('--critics', nargs='*', default=[])
    ap.add_argument('--adjust', nargs='*', default=[])
    ap.add_argument('--threads', type=int, default=4)
    ap.add_argument('--out', default=None)
    args = ap.parse_args()
    torch.set_num_threads(args.threads)
    turns = load_dumps(args.dump)
    K = len(turns[0]['scores'])
    print(f'{len(turns)} planned turns with lanes from {args.dump} ({K} lanes, {len(turns[0]["ended"]) // K} samples)')
    nets = {'base': load_net(args.base)}
    for c in args.critics:
        n, p = c.split('=', 1)
        nets[n] = load_net(p)
    logits = {n: [] for n in nets}
    fx = []
    for t in turns:
        live = ~t['ended']
        for n, net in nets.items():
            lg = np.zeros(len(live))
            if live.any():
                lg[live] = values(net, t['tiles'][live], t['globals'][live], t['techs'][live])
            logits[n].append(lg)
        fx.append(lane_facts(t['tiles'], t['globals'], t['techs']))
    # sanity: the base critic reproduces the recorded scores and choices
    rec_best = np.array([t['best'] for t in turns])
    base_scores = [lane_scores(t, lg, K) for t, lg in zip(turns, logits['base'])]
    base_best = np.array([int(np.argmax(s)) for s in base_scores])
    dev = np.mean([np.abs(s - t['scores']).max() for s, t in zip(base_scores, turns)])
    print(f'base critic offline vs recorded: same choice on {np.mean(base_best == rec_best):.1%} of turns, '
          f'mean max |score diff| {dev:.5f}')
    spread = np.array([s.max() - s.min() for s in base_scores])
    gap = np.array([np.sort(s)[-1] - np.sort(s)[-2] for s in base_scores])
    print(f'lane score spread: median {np.median(spread):.4f}, mean {spread.mean():.4f}; best - second best: median '
          f'{np.median(gap):.4f}, mean {gap.mean():.4f}')

    def report(name, scores):
        best = np.array([int(np.argmax(s)) for s in scores])
        ch = best != base_best
        # what the new choice does differently from the old one (lane facts, sample 0)
        diffs = {}
        for key in ('stars', 'ntechs', 'own_Catapult', 'own_total', 'cities', 'own_land'):
            dv = [f[key][b] - f[key][o] for f, b, o in zip(fx, best, base_best)]
            diffs[key] = float(np.mean(np.array(dv)[ch])) if ch.any() else 0.0
        # base-critic regret of the new choice (how much worse the base critic thinks it is)
        regret = np.array([bs[o] - bs[b] for bs, b, o in zip(base_scores, best, base_best)])
        print(f'{name}: choice changes on {ch.mean():.1%} of turns ({ch.sum()}/{len(ch)}); on those, the base critic '
              f'scores the new choice {regret[ch].mean() if ch.any() else 0:.4f} lower; new - old lane: ' +
              ', '.join(f'{k} {v:+.2f}' for k, v in diffs.items()), flush=True)
        return best

    out = dict(base_best=base_best, rec_best=rec_best)
    for n in nets:
        if n == 'base':
            continue
        out[n] = report(f'critic {n}', [lane_scores(t, lg, K) for t, lg in zip(turns, logits[n])])
    for a in args.adjust:
        name, _, spec = a.rpartition('=')
        terms = [(s.split(':')[0], float(s.split(':')[1])) for s in spec.split(',')]
        sc = []
        for t, lg, f in zip(turns, logits['base'], fx):
            adj = lg + sum(c * (lg if k == 'logit' else f[k]) for k, c in terms)  # 'logit:c' scales the logit by 1 + c
            sc.append(lane_scores(t, adj, K))
        out['adj_' + (name or spec)] = report(f'adjust {name or spec}', sc)
    if args.out:
        np.savez(args.out, **out)


if __name__ == '__main__':
    main()
