"""Win rates by matchup cell from eval_vs_bot.py --out files: per agent tribe, per bot tribe, and
the worst (agent tribe, bot tribe, seat) cells, for the M4 gate (no cell with >= 20 games below 70%).

    python3 tools/cells.py runs/evals/<tag>_d3.tsv [--min 20]
"""
import collections
import sys

sys.path.insert(0, __import__('os').path.join(__import__('os').path.dirname(__file__), '..', 'py'))
import polyenv  # noqa: E402

NAMES = {v: k for k, v in polyenv.TRIBES.items()}


def main():
    path = sys.argv[1]
    lo = int(sys.argv[sys.argv.index('--min') + 1]) if '--min' in sys.argv else 20
    cell = collections.defaultdict(lambda: [0, 0])
    mine = collections.defaultdict(lambda: [0, 0])
    theirs = collections.defaultdict(lambda: [0, 0])
    for line in open(path):
        f = line.rstrip('\n').split('\t')
        seat, t0, t1, res = int(f[1]), int(f[2]), int(f[3]), f[4]
        a, b = (t0, t1) if seat == 0 else (t1, t0)
        for d, k in ((cell, (a, b, seat)), (mine, a), (theirs, b)):
            d[k][0] += res == 'win'
            d[k][1] += 1
    n = sum(v[1] for v in mine.values())
    w = sum(v[0] for v in mine.values())
    print(f'{n} games, agent won {w} ({w / n:.1%})')
    print('by agent tribe:  ' + ', '.join(f'{NAMES.get(k, k)} {v[0]}/{v[1]}' for k, v in sorted(mine.items())))
    print('by bot tribe:    ' + ', '.join(f'{NAMES.get(k, k)} {v[0]}/{v[1]}' for k, v in sorted(theirs.items())))
    worst = sorted(cell.items(), key=lambda kv: kv[1][0] / kv[1][1])[:12]
    print('worst cells (agent tribe vs bot tribe, agent seat): ' + ', '.join(
        f'{NAMES.get(a, a)} v {NAMES.get(b, b)} s{s + 1} {v[0]}/{v[1]}' for (a, b, s), v in worst))
    gated = [(k, v) for k, v in cell.items() if v[1] >= lo]
    below = [(k, v) for k, v in gated if v[0] / v[1] < 0.7]
    print(f'cells with >= {lo} games: {len(gated)} of {len(cell)}; below 70%: {len(below)}')


if __name__ == '__main__':
    main()
