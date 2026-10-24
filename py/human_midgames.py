"""Finds close, complex midgame positions in the human championship games (data/human1), for
self-play starts (research log S3c) and for watching.

A position is the start of a player's turn: the first decision that player records in that turn.
From the two players' own observations (each exact about itself) it measures, per turn:
- closeness: score ratio, city difference, army value ratio (army value = the units' star cost);
- complexity: units on the board, units in contact (next to a visible enemy unit), legal moves and
  attacks available to the player to move;
- how open the game still was: score-lead changes after this turn, and whether the leader at this
  turn went on to lose.

Midgame positions: turn >= 8 and at least 3 turns before the end, both players with >= 3 cities
and >= 4 units, some contact; close: scores within 20%, cities within 2, the smaller army at least
half the larger. Ranked by percentile (among those positions) of
    complexity  = mean percentile of units on the board, units in contact, legal moves, attacks
    closeness   = mean percentile of score ratio and army ratio
    interest    = complexity + closeness + later lead changes (up to 3) / 12
with at most --per_game positions per game, at least 4 turns apart. Who won the human game doesn't
enter the training positions' rank (it would bias the starts toward upsets).

The showcase (printed, and --showcase games for the web app's watch list) ranks whole games for
watching: their best position's interest, plus back-and-forth (score-lead changes over the game, up
to 6, / 6) and 0.5 if the player ahead at that position lost. Only games whose replay ends in
the server's final state (final check 'bytes' or 'content', or differing only in VillageNameSeed)
are used, so the positions are exact.

    .venv/bin/python py/human_midgames.py [--data data/human1] [--top 2000] [--out runs/human_midgames.tsv]
"""
import argparse
import json
import multiprocessing as mp
import os

import numpy as np

import polyenv

T = {n: i for i, n in enumerate(polyenv.TILE_FEATURES)}
G = {n: i for i, n in enumerate(polyenv.GLOBAL_FEATURES)}
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GD = json.load(open(os.path.join(REPO, 'game', 'dump', 'gamedata', 'GameLogicData28.txt')))
COST = {v['idx']: v.get('cost', 0) or 0 for v in GD['unitData'].values() if isinstance(v, dict) and 'idx' in v}
CITY = GD['improvementData']['city']['idx']
ATTACK = {v: k for k, v in polyenv.COMMAND_TYPES.items()}['Attack']
TRIBE_NAMES = {v: k for k, v in polyenv.TRIBES.items()}


def side(tiles, legal_rows):
    """One player's view at the start of its turn -> its own units, army value, cities seen, contact."""
    unit, owner = tiles[:, T['unit']], tiles[:, T['unit_owner']]
    mine, theirs = (unit > 0) & (owner == 1), (unit > 0) & (owner == 2)
    grid_m, grid_t = mine.reshape(14, 14), theirs.reshape(14, 14)
    edge = np.zeros_like(grid_t)  # tiles next to a visible enemy unit
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            if not (dy or dx):
                continue
            sh = np.zeros_like(grid_t)
            ys, xs = slice(max(dy, 0), 14 + min(dy, 0)), slice(max(dx, 0), 14 + min(dx, 0))
            yt, xt = slice(max(-dy, 0), 14 + min(-dy, 0)), slice(max(-dx, 0), 14 + min(-dx, 0))
            sh[ys, xs] = grid_t[yt, xt]
            edge |= sh
    contact = int((grid_m & edge).sum())
    return dict(units=int(mine.sum()), army=int(sum(COST.get(int(u), 0) for u in unit[mine])),
                enemy_seen=int(theirs.sum()), contact=contact, legal=int(len(legal_rows)),
                attacks=int((legal_rows[:, 0] == ATTACK).sum()))


def game_positions(path):
    z = np.load(path)
    check = str(z['final_check'])
    fields = [str(f) for f in z['final_diff_fields']]
    if not (check in ('bytes', 'content') or (check.startswith('differs') and fields == ['VillageNameSeed'])):
        return None
    glob, player, off, legal = z['globals'], z['player'], z['legal_offsets'], z['legal']
    turn = glob[:, G['turn']]
    first = {}
    for d in range(len(player)):
        key = (int(turn[d]), int(player[d]))
        if key not in first:
            first[key] = d
    per = {}  # (turn, player) -> stats from that player's own view
    tiles = z['tiles']
    for (t, p), d in first.items():
        s = side(tiles[d], legal[off[d]:off[d + 1]])
        s.update(score=int(glob[d, G['score']]), cities=int(glob[d, G['cities']]), stars=int(glob[d, G['stars']]),
                 income=int(glob[d, G['income']]), decision=d)
        per[(t, p)] = s
    turns, winner = int(z['turns']), int(z['winner'])
    order = sorted(per)
    lead = []  # (turn, player, sign of player 1's score lead), from each side's latest numbers
    last = {}
    for t, p in order:
        last[p] = per[(t, p)]
        if 1 in last and 2 in last:
            diff = last[1]['score'] - last[2]['score']
            lead.append(((t, p), 0 if diff == 0 else (1 if diff > 0 else -1)))
    changes_after = {}
    signs = [s for _, s in lead if s]
    for i, (k, _) in enumerate(lead):
        later = [s for _, s in lead[i:] if s]
        changes_after[k] = sum(1 for a, b in zip(later, later[1:]) if a != b)
    out = []
    for i, (t, p) in enumerate(order):
        if t < 8 or t > turns - 3 or i == 0:
            continue
        me = per[(t, p)]
        prev = [k for k in order[:i] if k[1] != p]
        if not prev:
            continue
        opp = per[prev[-1]]
        if min(me['cities'], opp['cities']) < 3 or min(me['units'], opp['units']) < 4:
            continue
        contact = me['contact'] + opp['contact']
        if contact < 2:
            continue
        s_close = min(me['score'], opp['score']) / max(me['score'], opp['score'], 1)
        a_close = min(me['army'], opp['army']) / max(me['army'], opp['army'], 1)
        if s_close < 0.8 or abs(me['cities'] - opp['cities']) > 2 or a_close < 0.5:
            continue
        leader = p if me['score'] >= opp['score'] else 3 - p
        upset = winner in (1, 2) and leader != winner
        lc = changes_after.get((t, p), 0)
        out.append(dict(game=str(z['game_id']), turn=t, player=p, decision=me['decision'], interest=0.0,
                        total_units=me['units'] + opp['units'],
                        score_close=round(s_close, 3), army_close=round(a_close, 3), cities=(me['cities'], opp['cities']),
                        units=(me['units'], opp['units']), army=(me['army'], opp['army']), contact=contact, legal=me['legal'],
                        attacks=me['attacks'], lead_changes_after=lc, upset=bool(upset), game_turns=turns, winner=winner,
                        resigned=int(z['resigned']), tribes=[int(x) for x in z['tribes']],
                        total_lead_changes=sum(1 for a, b in zip(signs, signs[1:]) if a != b)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default=os.path.join(REPO, 'data', 'human1'))
    ap.add_argument('--top', type=int, default=2000)
    ap.add_argument('--per_game', type=int, default=2)
    ap.add_argument('--out', default=os.path.join(REPO, 'runs', 'human_midgames.tsv'))
    ap.add_argument('--procs', type=int, default=6)
    ap.add_argument('--showcase', type=int, default=12)
    args = ap.parse_args()
    files = sorted(os.path.join(args.data, f) for f in os.listdir(args.data) if f.endswith('.npz'))
    with mp.Pool(args.procs) as pool:
        results = pool.map(game_positions, files, chunksize=16)
    usable = [r for r in results if r is not None]
    positions = [p for r in usable for p in r]
    pct = lambda key: (lambda v: {i: r / max(len(v) - 1, 1) for r, i in enumerate(np.argsort(v, kind='stable'))})(
        np.array([p[key] for p in positions], float))
    ranks = {k: pct(k) for k in ('total_units', 'contact', 'legal', 'attacks', 'score_close', 'army_close')}
    for i, p in enumerate(positions):
        complexity = np.mean([ranks[k][i] for k in ('total_units', 'contact', 'legal', 'attacks')])
        closeness = np.mean([ranks[k][i] for k in ('score_close', 'army_close')])
        p['interest'] = round(float(complexity + closeness + min(p['lead_changes_after'], 3) / 12), 3)
    print(f'{len(files)} games, {len(usable)} with exact replays, {sum(1 for r in usable if r)} with a midgame position, '
          f'{len(positions)} positions', flush=True)
    positions.sort(key=lambda p: -p['interest'])
    chosen, per_game = [], {}
    for p in positions:
        taken = per_game.setdefault(p['game'], [])
        if len(taken) >= args.per_game or any(abs(p['turn'] - t) < 4 for t in taken):
            continue
        taken.append(p['turn'])
        chosen.append(p)
        if len(chosen) >= args.top:
            break
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    cols = ['game', 'turn', 'player', 'decision', 'interest', 'score_close', 'army_close', 'cities', 'units', 'army', 'contact',
            'legal', 'attacks', 'lead_changes_after', 'upset', 'game_turns', 'winner', 'resigned', 'tribes', 'total_lead_changes']
    with open(args.out, 'w') as f:
        f.write('\t'.join(cols) + '\n')
        for p in chosen:
            f.write('\t'.join(json.dumps(p[c]) if isinstance(p[c], (list, tuple)) else str(p[c]) for c in cols) + '\n')
    print(f'{len(chosen)} positions from {len(per_game)} games -> {args.out}')
    t = np.array([p['turn'] for p in chosen])
    print(f'turns: median {np.median(t):.0f}, 10-90% {np.percentile(t, 10):.0f}-{np.percentile(t, 90):.0f}; '
          f'units {np.median([sum(p["units"]) for p in chosen]):.0f} median; contact {np.median([p["contact"] for p in chosen]):.0f}; '
          f'upsets {np.mean([p["upset"] for p in chosen]):.0%}; winner = player to move {np.mean([p["winner"] == p["player"] for p in chosen]):.0%}')
    best = {}
    for p in positions:
        show = p['interest'] + min(p['total_lead_changes'], 6) / 6 + 0.5 * p['upset']
        if show > best.get(p['game'], (-1, None))[0]:
            best[p['game']] = (show, p)
    showcase = [p for _, p in sorted(best.values(), key=lambda x: -x[0])[:args.showcase]]
    with open(os.path.splitext(args.out)[0] + '_showcase.tsv', 'w') as f:
        f.write('\t'.join(cols) + '\n')
        for p in showcase:
            f.write('\t'.join(json.dumps(p[c]) if isinstance(p[c], (list, tuple)) else str(p[c]) for c in cols) + '\n')
    print('\nshowcase (games to watch):')
    for p in showcase:
        tn = '/'.join(TRIBE_NAMES.get(x, str(x)) for x in p['tribes'])
        print(f"  {p['game']} turn {p['turn']} (P{p['player']} to move), {tn}: score {p['score_close']:.2f}, army {p['army']}, "
              f"cities {p['cities']}, units {p['units']}, contact {p['contact']}, {p['legal']} legal, {p['attacks']} attacks, "
              f"{p['lead_changes_after']} lead changes after ({p['total_lead_changes']} in the game), {'upset, ' if p['upset'] else ''}"
              f"P{p['winner']} won at turn {p['game_turns']}{' (resigned)' if p['resigned'] else ''}")


if __name__ == '__main__':
    main()
