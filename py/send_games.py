"""Puts recorded games at the top of the web app's watch list ("Sent by Claude"): appends them to
runs/games/sent.jsonl with a note. Games are recorded by eval_vs_bot.py --save_games (tools/eval.sh
always does) and by the browser UI for games a person plays.

    python3 py/send_games.py --latest 10                  # the 10 most recent games
    python3 py/send_games.py --tag r4_20k_d3 --result loss --note "losses vs Crazy"
    python3 py/send_games.py --path r4_20k_d3/10000012_s1 --note "a long one"
    python3 py/send_games.py --clear                      # empty the sent list
"""
import argparse
import glob
import json
import os

GAMES = os.environ.get('POLY_GAMES', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'runs', 'games'))


def all_games():
    out = []
    for f in glob.glob(os.path.join(GAMES, '*', 'games.jsonl')):
        out += [json.loads(l) for l in open(f) if l.strip()]
    return sorted(out, key=lambda g: g.get('time', ''), reverse=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--latest', type=int, default=0, help='the N most recent games (after the filters)')
    ap.add_argument('--tag', default=None, help='only games of this recording (directory under runs/games)')
    ap.add_argument('--result', default=None, help='win, loss, cap or unfinished')
    ap.add_argument('--path', action='append', default=[], help='a game by its path, e.g. r4_20k_d3/10000012_s1')
    ap.add_argument('--note', default='', help='shown next to the game in the list')
    ap.add_argument('--clear', action='store_true')
    args = ap.parse_args()
    sent = os.path.join(GAMES, 'sent.jsonl')
    if args.clear:
        open(sent, 'w').close()
    games = all_games()
    pick = [g for g in games if g['path'] in args.path]
    rest = [g for g in games if (not args.tag or g.get('tag') == args.tag) and (not args.result or g.get('result') == args.result)]
    if args.latest:
        pick += rest[:args.latest]
    os.makedirs(GAMES, exist_ok=True)
    with open(sent, 'a') as f:
        for g in pick:
            f.write(json.dumps(dict(g, note=args.note)) + '\n')
    for g in pick:
        print(f"sent {g['path']}: {g.get('result')} in {g.get('turns')} turns {args.note}")


if __name__ == '__main__':
    main()
