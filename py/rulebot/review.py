"""Review recorded RuleBot games (match.py --record DIR): the bot's own view at the start of each
of its turns as text, with the decisions it made that turn and why.

    .venv/bin/python -m rulebot.review DIR/20000000_s1.pkl [--turns 5-12] [--log] [--summary]
    .venv/bin/python -m rulebot.review DIR --summary          # one line per game

Map legend: terrain  , field  f forest  M mountain  ~ water  . ocean  (blank) unexplored
cities: @ my capital, C my city, X enemy capital, E enemy city, v village, r ruin
units: lowercase letter = mine, uppercase = enemy (w warrior, r rider, k knight, d defender,
s swordsman, a archer, c catapult, m mindbender, g giant, b boat, x explorer), digits = hp/10
"""
import argparse
import os
import pickle
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import polyenv  # noqa: E402
from rulebot import data as D  # noqa: E402

F = {n: i for i, n in enumerate(polyenv.TILE_FEATURES)}
G = {n: i for i, n in enumerate(polyenv.GLOBAL_FEATURES)}
TER = {1: '~', 2: '.', 3: ',', 4: 'M', 5: 'f', 6: 'I', 7: 'w', 8: 'g'}
ULET = {D.WARRIOR: 'w', D.RIDER: 'r', D.KNIGHT: 'k', D.DEFENDER: 'd', D.SWORDSMAN: 's', D.ARCHER: 'a', D.CATAPULT: 'c',
        D.MINDBENDER: 'm', D.GIANT: 'g', D.EXPLORER_UNIT: 'x', D.RAFT: 'b', D.SCOUTSHIP: 'b', D.RAMMER: 'b',
        D.BOMBER: 'b', D.CLOAK: 'o', D.DAGGER: 'q', D.POLYTAUR: 'p'}


def render(tiles):
    t = tiles.reshape(D.N, -1)
    rows = []
    for y in range(D.H):
        cells = []
        for x in range(D.W):
            i = y * D.W + x
            f = t[i]
            if not f[F['explored']]:
                cells.append('   ')
                continue
            c0 = TER.get(int(f[F['terrain']]), '?')
            if f[F['improvement']] == D.CITY:
                own = int(f[F['owner']])
                cap = int(f[F['capital_of']]) != 0
                c0 = ('@' if cap else 'C') if own == 1 else ('X' if cap else 'E') if own == 2 else 'v'
            elif f[F['improvement']] == D.RUIN:
                c0 = 'r'
            elif f[F['improvement']] == D.PORT:
                c0 = 'P'
            elif f[F['improvement']] in (D.FARM, D.MINE, D.LUMBERHUT, D.WINDMILL, D.FORGE, D.SAWMILL, D.MARKET):
                c0 = '+'
            u = int(f[F['unit']]) - 1
            if u >= 0:
                let = ULET.get(u, '?')
                if f[F['unit_owner']] != 1:
                    let = let.upper()
                hp = min(9, max(0, int(round(f[F['unit_health']] / 10.0)))) if f[F['unit_health']] < 100 else 0
                cells.append(c0 + let + (str(hp) if hp else ' '))
            else:
                own = int(f[F['owner']])
                cells.append(c0 + ('·' if own == 1 else ':' if own == 2 else ' ') + ' ')
        rows.append(f'{y:2d} ' + ''.join(cells))
    rows.insert(0, '   ' + ''.join(f'{x:<3d}' for x in range(D.W)))
    return '\n'.join(rows)


def parse_turns(s, n):
    if not s:
        return range(n)
    a, _, b = s.partition('-')
    return range(int(a), int(b or a) + 1)


def show(path, turns=None, log=False):
    rec = pickle.load(open(path, 'rb'))
    m = rec['meta']
    name = lambda t: D.TRIBE_NAME.get(t, t)
    print(f"seed {m['seed']} seat {m['seat'] + 1}: {name(m['my_tribe'])} vs {name(m['opp_tribe'])} -> {m['result']} "
          f"at turn {m['turns']}")
    T = rec['turns']
    want = set(parse_turns(turns, 999))
    for k, tr in enumerate(T):
        if tr['turn'] not in want:
            continue
        g = tr['globals']
        techs = [D.TECH_NAME.get(int(i), i) for i in np.nonzero(tr['techs'])[0] if i]
        print(f"\n--- turn {tr['turn']}: stars {g[G['stars']]} income {g[G['income']]} cities {g[G['cities']]} "
              f"techs {techs}")
        print(render(tr['tiles']))
        if log:
            end = T[k + 1]['log_at'] if k + 1 < len(T) else len(rec['log'])
            for line in rec['log'][tr['log_at']:end]:
                print('   ', line)


def game_line(rec):
    m = rec['meta']
    T = rec['turns']
    g = lambda k, n: int(T[min(k, len(T) - 1)]['globals'][G[n]]) if T else 0
    cities = [int(t['globals'][G['cities']]) for t in T]
    units = [int((t['tiles'].reshape(D.N, -1)[:, F['unit_owner']] == 1).sum()) for t in T]
    stars = [int(t['globals'][G['stars']]) for t in T]
    inc = [int(t['globals'][G['income']]) for t in T]
    at = lambda xs, k: xs[min(k, len(xs) - 1)] if xs else 0
    opp = [t.get('opp') for t in T]
    oc = lambda k, n: (opp[min(k, len(opp) - 1)] or {}).get(n, '?') if opp else '?'
    first_contact = next((t['turn'] for t in T if (t['tiles'].reshape(D.N, -1)[:, F['unit_owner']] == 2).any()), None)
    return (f"{D.TRIBE_NAME.get(m['my_tribe'])[:5]:5s} v {D.TRIBE_NAME.get(m['opp_tribe'])[:5]:5s} {m['result']:4s} t{m['turns']:<3d} "
            f"cities 5/10/20/max {at(cities, 5)}/{at(cities, 10)}/{at(cities, 20)}/{max(cities or [0])} "
            f"income 5/10/20 {at(inc, 5)}/{at(inc, 10)}/{at(inc, 20)} units 10/20 {at(units, 10)}/{at(units, 20)} "
            f"stars@start mean {np.mean(stars[3:21] or [0]):.1f} contact t{first_contact} "
            f"kills {g(999, 'kills')} lost {g(999, 'casualties')} | opp cities 5/10/20 {oc(5, 'cities')}/{oc(10, 'cities')}/"
            f"{oc(20, 'cities')} income {oc(5, 'income')}/{oc(10, 'income')}/{oc(20, 'income')} "
            f"units {oc(10, 'units')}/{oc(20, 'units')} techs@10/20 {oc(10, 'techs')}/{oc(20, 'techs')} "
            f"my techs@10/20 {int(T[min(10, len(T) - 1)]['techs'].sum())}/{int(T[min(20, len(T) - 1)]['techs'].sum())}")


def summary(d, only=None):
    files = sorted(f for f in os.listdir(d) if f.endswith('.pkl'))
    for f in files:
        rec = pickle.load(open(os.path.join(d, f), 'rb'))
        if only and rec['meta']['result'] != only:
            continue
        print(f[:-4], game_line(rec))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('path')
    ap.add_argument('--turns', default=None)
    ap.add_argument('--log', action='store_true')
    ap.add_argument('--summary', action='store_true')
    ap.add_argument('--only', default=None, help='with a directory: only games with this result')
    args = ap.parse_args()
    if os.path.isdir(args.path):
        summary(args.path, args.only)
    else:
        show(args.path, args.turns, args.log)


if __name__ == '__main__':
    main()
