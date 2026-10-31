"""Strategy indicators per player-game (docs/strategy: the book's measurable indicators), for comparing
self-play generations with the human championship games:

- tempo: techs researched by turns 10 and 20, cities at turns 5, 10, 15 and 20, stars banked
  (stars left when the player ends a turn, turns 3-20);
- water: share of player-games with a port by turn 20, ports built, boats upgraded (Upgrade);
- army: units trained by kind (front line: warrior, swordsman, defender, rider, knight, giant;
  ranged: archer, catapult), catapult share, attacks per game.

Reads py/selfplay.py games (`played`) and converted human games (`label`; rows outside the legal
list are skipped). Turn-based indicators count only player-games that reached that turn.

    python indicators.py human=/home/greff/polydata/human_full g001=/home/greff/runs/sp/s3/games/g001 ... [--max_games 400]
                         [--tsv indicators.tsv]   (also appends one line per set there)
"""
import argparse
import collections
import multiprocessing as mp
import os
import random

import numpy as np

import polyenv

CMD = {v: k for k, v in polyenv.COMMAND_TYPES.items()}
G_TURN = polyenv.GLOBAL_FEATURES.index('turn')
G_STARS = polyenv.GLOBAL_FEATURES.index('stars')
G_CITIES = polyenv.GLOBAL_FEATURES.index('cities')
FRONT = {2: 'warrior', 11: 'swordsman', 5: 'defender', 3: 'rider', 4: 'knight', 12: 'giant'}
RANGED = {9: 'archer', 8: 'catapult'}
PORT = 8


def player_games(path):
    z = np.load(path)
    if 'start_turn' in z.files and int(z['start_turn']) > 0:  # began from a human midgame position
        return []
    rows = z['played'] if 'played' in z.files else z['label']
    legal, off, player, glob = z['legal'], z['legal_offsets'], z['player'], z['globals']
    turn = glob[:, G_TURN]
    out = []
    for p in sorted(set(int(x) for x in player)):
        r = dict(won=bool(z['ended']) and int(z['winner']) == p, turns=int(z['turns']), techs=[], ports=[], upgrades=0,
                 units=collections.Counter(), attacks=0, cities={}, banked=[])
        ds = np.nonzero(player == p)[0]
        for d in ds:
            t_now = int(turn[d])
            r['cities'].setdefault(t_now, int(glob[d, G_CITIES]))
            if rows[d] < 0:
                continue
            t, a, b, arg = (int(x) for x in legal[off[d] + rows[d]])
            if t == CMD['Research']:
                r['techs'].append(t_now)
            elif t == CMD['Build'] and arg == PORT:
                r['ports'].append(t_now)
            elif t == CMD['Upgrade']:
                r['upgrades'] += 1
            elif t == CMD['Train']:
                r['units'][arg] += 1
            elif t == CMD['Attack']:
                r['attacks'] += 1
            elif t == CMD['EndTurn'] and 3 <= t_now <= 20:
                r['banked'].append(int(glob[d, G_STARS]))
        out.append(r)
    return out


def summarize(name, recs):
    n = len(recs)
    reached = lambda T: [r for r in recs if r['turns'] >= T]
    mean = lambda xs: float(np.mean(xs)) if len(xs) else float('nan')
    units = sum((r['units'] for r in recs), collections.Counter())
    total = sum(units.values()) or 1
    front = sum(units[k] for k in FRONT) / total
    ranged = sum(units[k] for k in RANGED) / total
    city_at = lambda T: mean([r['cities'].get(T, max((c for t, c in r['cities'].items() if t <= T), default=1))
                              for r in reached(T)])
    return dict(
        name=name, player_games=n, win_turns=mean([r['turns'] for r in recs if r['won']]),
        techs10=mean([sum(t <= 10 for t in r['techs']) for r in reached(10)]),
        techs20=mean([sum(t <= 20 for t in r['techs']) for r in reached(20)]),
        cities5=city_at(5), cities10=city_at(10), cities15=city_at(15), cities20=city_at(20),
        banked=mean([x for r in recs for x in r['banked']]),
        port_by20=mean([any(t <= 20 for t in r['ports']) for r in reached(20)]),
        ports=mean([len(r['ports']) for r in recs]), upgrades=mean([r['upgrades'] for r in recs]),
        units=mean([sum(r['units'].values()) for r in recs]), front=front, ranged=ranged,
        catapult=units[8] / total, attacks=mean([r['attacks'] for r in recs]),
        mix=', '.join(f'{(FRONT | RANGED).get(k, k)} {v / total:.0%}' for k, v in units.most_common(6)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('sets', nargs='+', help='name=directory of .npz games')
    ap.add_argument('--max_games', type=int, default=400)
    ap.add_argument('--procs', type=int, default=8)
    ap.add_argument('--tsv', default=None, help='append one line per set to this file (with a header if it is new)')
    args = ap.parse_args()
    rows = []
    with mp.Pool(args.procs) as pool:
        for s in args.sets:
            name, d = s.split('=', 1)
            files = sorted(os.path.join(d, f) for f in os.listdir(d) if f.endswith('.npz') and not f.endswith('.tmp.npz'))
            random.Random(0).shuffle(files)
            recs = [r for rs in pool.map(player_games, files[:args.max_games]) for r in rs]
            rows.append(summarize(name, recs))
    cols = [('name', '{}'), ('player_games', '{}'), ('techs10', '{:.1f}'), ('techs20', '{:.1f}'), ('cities5', '{:.1f}'),
            ('cities10', '{:.1f}'), ('cities15', '{:.1f}'), ('cities20', '{:.1f}'), ('banked', '{:.1f}'),
            ('port_by20', '{:.0%}'), ('ports', '{:.2f}'), ('upgrades', '{:.1f}'), ('units', '{:.1f}'), ('front', '{:.0%}'),
            ('ranged', '{:.0%}'), ('catapult', '{:.0%}'), ('attacks', '{:.1f}'), ('win_turns', '{:.1f}')]
    print('| ' + ' | '.join(c for c, _ in cols) + ' |')
    print('|' + '---|' * len(cols))
    for r in rows:
        print('| ' + ' | '.join(f.format(r[c]) for c, f in cols) + ' |')
    for r in rows:
        print(f"{r['name']}: {r['mix']}")
    if args.tsv:
        new = not os.path.exists(args.tsv)
        with open(args.tsv, 'a') as f:
            if new:
                f.write('\t'.join(c for c, _ in cols) + '\n')
            for r in rows:
                f.write('\t'.join(str(r[c]) if c in ('name', 'player_games') else f'{r[c]:.4f}' for c, _ in cols) + '\n')


if __name__ == '__main__':
    main()
