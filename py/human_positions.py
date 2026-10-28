"""Chooses a large, varied set of human game positions for self-play starts (research log S8): up to
--per_game positions from every human championship game whose replay is exact and whose version is
at least --min_version, spread over the game: the player-turn starts from turn 3 to the second-last
turn are split into early, middle and late thirds, and positions are drawn from each third in turn,
at least --gap turns apart. Positions where the game is already decided are left out: a player without
cities, or one score below --min_ratio of the other.

Unlike py/human_midgames.py (2,000 close, complex midgames), this aims at variety: tens of thousands
of positions from every phase, so no position is seen often enough to be memorized. The positions
are then saved as states by py/human_starts.py.

    .venv/bin/python py/human_positions.py [--per_game 16] [--out runs/human_positions.tsv]
"""
import argparse
import multiprocessing as mp
import os
import random

import numpy as np

import polyenv

G = {n: i for i, n in enumerate(polyenv.GLOBAL_FEATURES)}
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def game_positions(job):
    path, per_game, min_ratio, min_version, seed, args_gap = job
    z = np.load(path)
    check, fields = str(z['final_check']), [str(f) for f in z['final_diff_fields']]
    if not (check in ('bytes', 'content') or (check.startswith('differs') and fields == ['VillageNameSeed'])):
        return []
    if int(z['game_version']) < min_version:
        return []
    glob, player = z['globals'], z['player']
    turn = glob[:, G['turn']]
    first, last = {}, {}
    for d in range(len(player)):
        k = (int(turn[d]), int(player[d]))
        first.setdefault(k, d)
    for (t, p), d in first.items():
        last[p] = (t, d)
    turns = int(z['turns'])
    cands = []
    for (t, p), d in sorted(first.items()):
        if t < 3 or t > turns - 1:
            continue
        prev = [first[k] for k in first if k[1] != p and k <= (t, p)]
        if not prev:
            continue
        o = max(prev)
        me_cities, opp_cities = int(glob[d, G['cities']]), int(glob[o, G['cities']])
        s, so = int(glob[d, G['score']]), int(glob[o, G['score']])
        if me_cities < 1 or opp_cities < 1 or min(s, so) < min_ratio * max(s, so):
            continue
        cands.append((t, p, d))
    if not cands:
        return []
    rng = random.Random(seed)
    lo, hi = cands[0][0], cands[-1][0]
    thirds = [[c for c in cands if lo + (hi - lo + 1) * k / 3 <= c[0] < lo + (hi - lo + 1) * (k + 1) / 3] for k in range(3)]
    for th in thirds:
        rng.shuffle(th)
    chosen, k = [], 0
    while len(chosen) < per_game and any(thirds):
        th = thirds[k % 3]
        k += 1
        while th:
            c = th.pop()
            if all(abs(c[0] - x[0]) >= args_gap for x in chosen):
                chosen.append(c)
                break
    gid = str(z['game_id'])
    tribes = ','.join(str(int(x)) for x in z['tribes'])
    return [(gid, t, p, d, tribes, int(z['game_version']), turns) for t, p, d in sorted(chosen)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default=os.path.join(REPO, 'data', 'human1'))
    ap.add_argument('--per_game', type=int, default=16)
    ap.add_argument('--min_ratio', type=float, default=0.4)
    ap.add_argument('--min_version', type=int, default=0, help='older versions are re-checked under the current rules by py/human_starts.py')
    ap.add_argument('--out', default=os.path.join(REPO, 'runs', 'human_positions.tsv'))
    ap.add_argument('--procs', type=int, default=4)
    ap.add_argument('--gap', type=int, default=1, help='turns between a game\'s positions')
    args = ap.parse_args()
    files = sorted(os.path.join(args.data, f) for f in os.listdir(args.data) if f.endswith('.npz'))
    jobs = [(f, args.per_game, args.min_ratio, args.min_version, i, args.gap) for i, f in enumerate(files)]
    with mp.Pool(args.procs) as pool:
        rows = [r for rs in pool.map(game_positions, jobs, chunksize=16) for r in rs]
    with open(args.out, 'w') as f:
        f.write('game\tturn\tplayer\tdecision\ttribes\tversion\tgame_turns\n')
        for r in rows:
            f.write('\t'.join(map(str, r)) + '\n')
    t = np.array([r[1] for r in rows])
    rel = np.array([r[1] / r[6] for r in rows])
    print(f'{len(rows)} positions from {len(set(r[0] for r in rows))} games -> {args.out}')
    print(f'turns: 10% {np.percentile(t, 10):.0f}, median {np.median(t):.0f}, 90% {np.percentile(t, 90):.0f}; '
          f'share of the game: early {np.mean(rel < 1 / 3):.0%}, middle {np.mean((rel >= 1 / 3) & (rel < 2 / 3)):.0%}, '
          f'late {np.mean(rel >= 2 / 3):.0%}; mirror matches {np.mean([r[4].split(",")[0] == r[4].split(",")[1] for r in rows]):.0%}')


if __name__ == '__main__':
    main()
