"""Review recorded CrazyPlus games (match.py --record DIR): the board at the start of each of the
bot's turns (its own view, and the opponent's view of its own side), and every override with its
reason and the scores of the candidates it compared (stars, relative to the AI's own command).

    .venv/bin/python -m crazyplus.review DIR --summary [--only loss]
    .venv/bin/python -m crazyplus.review DIR/23000003_s1.pkl --turns 10-14 [--log] [--opp]

Map legend as py/rulebot/review.py: lowercase units are mine, uppercase the enemy's, digits = hp/10.
"""
import argparse
import collections
import os
import pickle
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from rulebot import data as D  # noqa: E402
from rulebot.review import G, game_line, parse_turns, render  # noqa: E402

CMD = {1: 'Build', 2: 'Attack', 3: 'Recover', 4: 'Heal', 5: 'Train', 6: 'Move', 7: 'Capture', 8: 'Research',
       11: 'Reward', 13: 'Promote', 14: 'Ruins', 15: 'EndTurn', 16: 'Upgrade', 21: 'Stay', 36: 'Disembark'}


def row_text(r):
    if r is None:
        return 'Stay(AI)'
    if r[0] == 'stay':
        return f'Stay {D.xy(r[1])}'
    t, a, b, arg = r
    s = CMD.get(t, f'type{t}')
    if a is not None and a >= 0:
        s += f' {D.xy(a)}'
    if b is not None and b >= 0:
        s += f'->{D.xy(b)}'
    if t in (1, 5, 8, 11, 16):
        s += f' [{arg}]'
    return s


def show(path, turns=None, log=False, opp=False):
    rec = pickle.load(open(path, 'rb'))
    m = rec['meta']
    name = lambda t: D.TRIBE_NAME.get(t, t)
    print(f"seed {m['seed']} seat {m['seat'] + 1}: {name(m['my_tribe'])} vs {name(m['opp_tribe'])} -> {m['result']} "
          f"at turn {m['turns']}  stats {m.get('stats')}")
    T = rec['turns']
    want = set(parse_turns(turns, 999))
    for k, tr in enumerate(T):
        if tr['turn'] not in want:
            continue
        g = tr['globals']
        techs = [D.TECH_NAME.get(int(i), i) for i in np.nonzero(tr['techs'])[0] if i]
        print(f"\n--- turn {tr['turn']}: stars {g[G['stars']]} income {g[G['income']]} cities {g[G['cities']]} "
              f"techs {techs} | opponent {tr.get('opp')}")
        print(render(tr['tiles']))
        if opp and 'opp_tiles' in tr:
            print('  opponent\'s own view:')
            print(render(tr['opp_tiles']))
        if log:
            end = T[k + 1]['log_at'] if k + 1 < len(T) else len(rec['log'])
            for line in rec['log'][tr['log_at']:end]:
                sc = ', '.join(f'{w} {v}' for w, v in (line.get('scored') or []))
                print(f"    {line['kind']:18s} AI {row_text(line['ai']):24s} -> {row_text(line['chosen']):24s} "
                      f"{line['why']}   [{sc}]")


def summary(d, only=None):
    files = sorted(f for f in os.listdir(d) if f.endswith('.pkl'))
    kinds = collections.Counter()
    for f in files:
        rec = pickle.load(open(os.path.join(d, f), 'rb'))
        kinds.update(l['kind'] for l in rec['log'])
        if only and rec['meta']['result'] != only:
            continue
        print(f[:-4], game_line(rec))
    print('overrides by kind:', dict(kinds.most_common()))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('path')
    ap.add_argument('--turns', default=None)
    ap.add_argument('--log', action='store_true')
    ap.add_argument('--opp', action='store_true', help="also render the opponent's own view")
    ap.add_argument('--summary', action='store_true')
    ap.add_argument('--only', default=None, help='with a directory: only games with this result')
    args = ap.parse_args()
    if os.path.isdir(args.path):
        summary(args.path, args.only)
    else:
        show(args.path, args.turns, args.log, args.opp)


if __name__ == '__main__':
    main()
