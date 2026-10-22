"""What the self-play policy actually does, per generation: a strategy profile from the recorded
moves of py/selfplay.py games (or any dagger-layout games with `played`).

Per player-game (each recorded seat): the techs it researched in order, the units it trained, the
city rewards it chose, what it built, its captures (villages vs enemy cities, by the tile's owner
in its own observation), attacks, and when the game ended. Reported as shares and averages, split by
winners and losers where that is telling.

    python sp_strategy.py ~/runs/sp/s1/games/g001 ~/runs/sp/s1/games/g002 ... [--max_games 500]
"""
import argparse
import collections
import multiprocessing as mp
import os

import numpy as np

import polyenv

TECHS = dict(enumerate(
    'Basic Riding FreeSpirit Chivalry Roads Trade Organization Shields Farming Construction Fishing Whaling Aquatism '
    'Sailing Navigation Hunting Forestry Mathematics Archery Spiritualism Climbing Meditation Philosophy Mining '
    'Smithery FreeDiving Spearing'.split()))
UNITS = dict(enumerate('None Scout Warrior Rider Knight Defender Ship Battleship Catapult Archer MindBender Swordsman '
                       'Giant'.split()))
BUILDS = {1: 'City', 3: 'Road', 4: 'CustomsHouse', 5: 'Farm', 6: 'Windmill', 7: 'Fishing', 8: 'Port', 9: 'Hunting',
          10: 'ClearForest', 11: 'BurnForest', 12: 'LumberHut', 13: 'Sawmill', 14: 'GrowForest', 15: 'HarvestFruit',
          16: 'WhaleHunting', 17: 'Temple', 18: 'ForestTemple', 19: 'WaterTemple', 20: 'MountainTemple', 21: 'Mine',
          22: 'Forge', 47: 'LightHouse', 48: 'Bridge', 50: 'Market'}
REWARDS = {1: 'CityWall', 2: 'Park', 3: 'Workshop', 4: 'Explorer', 5: 'BorderGrowth', 6: 'SuperUnit', 7: 'Resources',
           8: 'PopulationGrowth'}
TRIBE_NAMES = {v: k for k, v in polyenv.TRIBES.items()}
T_OWNER = polyenv.TILE_FEATURES.index('owner')
G_TURN = polyenv.GLOBAL_FEATURES.index('turn')
CMD = {v: k for k, v in polyenv.COMMAND_TYPES.items()}


def profile(path):
    """One game -> a list of per-player records."""
    z = np.load(path)
    if 'played' not in z.files:
        return []
    legal, off, played, player = z['legal'], z['legal_offsets'], z['played'], z['player']
    turn = z['globals'][:, G_TURN]
    tiles = None
    winner, ended = int(z['winner']), bool(z['ended'])
    out = []
    for p in sorted(set(int(x) for x in player)):
        rec = dict(won=ended and winner == p, ended=ended, turns=int(z['turns']), tribe=int(z['tribes'][p - 1]),
                   opponent=int(z['opponent']) if 'opponent' in z.files else 0,
                   techs=[], units=collections.Counter(), rewards=collections.Counter(), builds=collections.Counter(),
                   villages=0, cities=0, first_city_turn=None, attacks=0, decisions=0)
        for d in np.nonzero(player == p)[0]:
            t, a, b, arg = (int(x) for x in legal[off[d] + played[d]])
            rec['decisions'] += 1
            if t == CMD['Research']:
                rec['techs'].append(arg)
            elif t == CMD['Train']:
                rec['units'][arg] += 1
            elif t == CMD['CityReward']:
                rec['rewards'][arg] += 1
            elif t == CMD['Build']:
                rec['builds'][arg] += 1
            elif t == CMD['Attack']:
                rec['attacks'] += 1
            elif t == CMD['Capture']:
                if tiles is None:
                    tiles = z['tiles']
                owner = int(tiles[d, a, T_OWNER])
                if owner == 0:
                    rec['villages'] += 1
                else:
                    rec['cities'] += 1
                    if rec['first_city_turn'] is None:
                        rec['first_city_turn'] = int(turn[d])
        out.append(rec)
    return out


def share(counter, names, top=6):
    tot = sum(counter.values()) or 1
    return ', '.join(f'{names.get(k, k)} {v / tot:.0%}' for k, v in counter.most_common(top))


def report(name, recs):
    print(f'\n## {name}: {len(recs)} player-games')
    ended = [r for r in recs if r['ended']]
    wins = [r for r in recs if r['won']]
    if not recs:
        return
    print(f'- games end at turn {np.mean([r["turns"] for r in ended]):.1f} on average '
          f'(median {np.median([r["turns"] for r in ended]):.0f}); {len(recs) - len(ended)} capped')
    for label, rs in (('winners', wins), ('losers', [r for r in ended if not r['won']])):
        if not rs:
            continue
        fc = [r['first_city_turn'] for r in rs if r['first_city_turn'] is not None]
        print(f'- {label}: {np.mean([r["villages"] for r in rs]):.1f} villages, '
              f'{np.mean([r["cities"] for r in rs]):.2f} enemy cities captured (first at turn '
              f'{np.mean(fc) if fc else float("nan"):.1f}), {np.mean([r["attacks"] for r in rs]):.1f} attacks, '
              f'{np.mean([sum(r["units"].values()) for r in rs]):.1f} units trained, '
              f'{np.mean([len(r["techs"]) for r in rs]):.1f} techs')
    first = collections.Counter(r['techs'][0] for r in recs if r['techs'])
    second = collections.Counter(r['techs'][1] for r in recs if len(r['techs']) > 1)
    print(f'- first tech: {share(first, TECHS, 5)}')
    print(f'- second tech: {share(second, TECHS, 5)}')
    allt = collections.Counter(t for r in recs for t in r['techs'])
    print(f'- techs overall: {share(allt, TECHS, 8)}')
    print(f'- units: {share(sum((r["units"] for r in recs), collections.Counter()), UNITS, 6)}')
    print(f'- city rewards: {share(sum((r["rewards"] for r in recs), collections.Counter()), REWARDS, 6)}')
    print(f'- builds: {share(sum((r["builds"] for r in recs), collections.Counter()), BUILDS, 6)}')
    by_tribe = collections.defaultdict(list)
    for r in ended:
        by_tribe[r['tribe']].append(r['won'])
    tr = sorted(((np.mean(v), TRIBE_NAMES.get(k, k), len(v)) for k, v in by_tribe.items() if len(v) >= 20), reverse=True)
    if tr:
        print('- win rate by own tribe (self-play and pool games): ' + ', '.join(f'{n} {w:.0%}' for w, n, _ in tr))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dirs', nargs='+')
    ap.add_argument('--max_games', type=int, default=600)
    ap.add_argument('--procs', type=int, default=6)
    args = ap.parse_args()
    with mp.Pool(args.procs) as pool:
        for d in args.dirs:
            files = sorted(os.path.join(d, f) for f in os.listdir(d) if f.endswith('.npz') and not f.endswith('.tmp.npz'))
            files = files[:args.max_games]
            recs = [r for rs in pool.map(profile, files, chunksize=8) for r in rs]
            report(os.path.basename(os.path.normpath(d)) + f' ({len(files)} games)', recs)


if __name__ == '__main__':
    main()
