"""Strategy-tagged start positions from the human championship games (docs/design/strategy-starts.md).

Reads the converted human games (data/human1, py/convert_replays.py's layout) and finds strategy
episodes from the record alone: the commands each player chose (units trained, techs, builds, boat
upgrades, city rewards, diplomacy), and what the chosen attacks and captures did (read from the
player's own observation before and after the command). Nothing here plays or judges a position.

Stage 1 (extract) turns every game into a compact event table and per-turn summaries (cached in
--cache, so the taxonomy can be tuned without reading the games again). Stage 2 (classify) finds
the episodes of each kind (DETECTORS; the rules are in each detector's docstring), keeps the
successful ones, and writes:
  <out>.tsv           positions for py/human_starts.py (game, turn, player, decision, tribes, version,
                      game_turns, strategy, lead, episode_t0, payoff_turn, split, won, note): the
                      executing player's turn `lead` (1-3) turns before the episode starts; at most
                      --cap per kind from the training games (train_bc.is_val false), plus at most
                      --val_cap per kind from the held-out games (split 'val', which py/selfplay.py
                      --starts_split train never uses for training);
  <out>_episodes.tsv  every episode found (successful or not), for counting and watching;
  <out>_summary.tsv   per kind: attempts, successes, success rate, share by the game's winner,
                      successful episodes with a start position (train, val), positions written.
Kinds with fewer than --min_episodes successful training episodes are dropped.

    .venv/bin/python py/strategy_positions.py [--data data/human1] [--out runs/strategy/strategy_positions]
        [--only pc_raw_ids.txt] [--cap 600] [--val_cap 60] [--procs 4]
"""
import argparse
import collections
import json
import multiprocessing as mp
import os
import pickle
import random
import zlib

import numpy as np

import polyenv

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
T = {n: i for i, n in enumerate(polyenv.TILE_FEATURES)}
G = {n: i for i, n in enumerate(polyenv.GLOBAL_FEATURES)}
CMD = {v: k for k, v in polyenv.COMMAND_TYPES.items()}
W = 14  # Small map; all championship games are 14x14
WATER_TERRAIN = (1, 2)  # TerrainType water, ocean
CITY_IMP = 1            # ImprovementType city (villages are cities without an owner)
HUMAN_VAL_EVERY = 10    # train_bc.is_val for human games (crc32 % 10 == 0 is held out)


def load_gamedata(path):
    gd = json.load(open(path))
    idx = lambda key: {n: v['idx'] for n, v in gd[key].items() if isinstance(v, dict) and 'idx' in v}
    units, techs, imps = idx('unitData'), idx('techData'), idx('improvementData')
    cost = {v['idx']: v.get('cost', 0) or 0 for v in gd['unitData'].values() if isinstance(v, dict) and 'idx' in v}
    return units, techs, imps, cost


GD_PATH = next((p for p in (
    os.path.join(REPO, 'game', 'dump', 'gamedata', 'GameLogicData28.txt'),
    # a git worktree has no game files: the main checkout's
    os.path.join(REPO.split('/.claude/worktrees/')[0], 'game', 'dump', 'gamedata', 'GameLogicData28.txt'),
    os.path.expanduser('~/polylinux/gamedata/GameLogicData28.txt'),  # the PC
) if os.path.exists(p)), None)
U, TECH, IMP, COST = load_gamedata(GD_PATH)
NAVAL = {U[n] for n in ('transportship', 'scoutship', 'rammership', 'bombership', 'juggernaut', 'pirate', 'cloak_boat')}
WARSHIPS = {U[n] for n in ('scoutship', 'rammership', 'bombership', 'juggernaut', 'pirate')}
REWARD = {'CityWall': 1, 'Park': 2, 'Workshop': 3, 'Explorer': 4, 'BorderGrowth': 5, 'SuperUnit': 6, 'Resources': 7,
          'PopulationGrowth': 8}  # the game's CityReward enum (GameLogicAssembly/CityReward.cs)

# Event table columns (int16). u_* are UnitType (-1 none); own_* (unit owner) and towner_* (the tile's
# territory owner, which for a city tile is the city's owner: 0 for a village) are relative: 1 me, 2 enemy.
EV = ['d', 'turn', 'player', 'type', 'a', 'b', 'arg', 'u_a', 'hp_a', 'terr_a', 'imp_a', 'towner_a', 'cap_a', 'lvl_a',
      'pas_a', 'u_b', 'own_b', 'hp_b', 'terr_b', 'imp_b', 'cap_b', 'towner_b', 'kills', 'after_u_b',
      'after_own_b', 'threat']
E = {n: i for i, n in enumerate(EV)}
# Per player-turn summary columns, from the player's first decision of the turn.
TS = ['turn', 'player', 'd', 'stars', 'income', 'score', 'cities', 'kills', 'casualties', 'techs', 'relation',
      'my_embassy', 'their_embassy', 'units', 'army', 'enemy_seen', 'enemy_in_mine', 'mine_in_theirs', 'naval', 'warships',
      'enemy_naval', 'threat']
S = {n: i for i, n in enumerate(TS)}
UNIT_SLOTS = 64


def cheb(i, j):
    i, j = np.asarray(i), np.asarray(j)
    return np.maximum(np.abs(i // W - j // W), np.abs(i % W - j % W))


def exact_replay(z):
    check, fields = str(z['final_check']), [str(f) for f in z['final_diff_fields']]
    return check in ('bytes', 'content') or (check.startswith('differs') and fields == ['VillageNameSeed'])


def extract(path):
    """One game -> dict(meta, turns [n, len(TS)] int32, units [n, UNIT_SLOTS] int16 (own units by type at
    each player-turn start, boats counted with their passengers), events [m, len(EV)] int16)."""
    z = np.load(path)
    meta = dict(game=str(z['game_id']), version=int(z['game_version']), tribes=[int(x) for x in z['tribes']],
                winner=int(z['winner']), resigned=int(z['resigned']), turns=int(z['turns']), exact=exact_replay(z))
    label, player = z['label'].astype(np.int64), z['player'].astype(np.int64)
    glob, off, legal = z['globals'], z['legal_offsets'], z['legal']
    tiles = z['tiles']
    if tiles.shape[1] != W * W:
        return dict(meta=meta, turns=None, units=None, events=None)
    techs = z['techs']
    D = len(label)
    ok = label >= 0
    rows = np.full((D, 4), -1, np.int64)
    rows[ok] = legal[off[:-1][ok] + label[ok]]
    unit = tiles[..., T['unit']].astype(np.int64) - 1
    uown, terr, imp = tiles[..., T['unit_owner']], tiles[..., T['terrain']], tiles[..., T['improvement']]
    cap, towner = tiles[..., T['capital_of']], tiles[..., T['owner']]
    hp, lvl, pas = tiles[..., T['unit_health']], tiles[..., T['imp_level']], tiles[..., T['unit_passenger']].astype(np.int64) - 1
    turn = glob[:, G['turn']]
    allt = np.arange(W * W)

    def threat(d, tile):
        """Chebyshev distance from `tile` to the nearest enemy unit the player sees (99 if none)."""
        e = np.nonzero((unit[d] >= 0) & (uown[d] == 2))[0]
        return int(cheb(e, tile).min()) if len(e) else 99

    # per player-turn summaries
    first = {}
    for d in range(D):
        first.setdefault((int(turn[d]), int(player[d])), d)
    trows, urows = [], []
    for (t, p), d in sorted(first.items()):
        mine = (unit[d] >= 0) & (uown[d] == 1)
        theirs = (unit[d] >= 0) & (uown[d] == 2)
        types = unit[d][mine]
        cnt = np.bincount(np.clip(types, 0, UNIT_SLOTS - 1), minlength=UNIT_SLOTS)
        pt = pas[d][mine]
        pt = pt[pt >= 0]
        cnt += np.bincount(np.clip(pt, 0, UNIT_SLOTS - 1), minlength=UNIT_SLOTS)
        cities = np.nonzero((imp[d] == CITY_IMP) & (towner[d] == 1))[0]
        e = np.nonzero(theirs)[0]
        th = int(min(cheb(e[:, None], cities[None, :]).min(), 99)) if len(e) and len(cities) else 99
        g = glob[d]
        trows.append([t, p, d, g[G['stars']], g[G['income']], g[G['score']], g[G['cities']], g[G['kills']],
                      g[G['casualties']], int(techs[d].sum()), g[G['relation']], g[G['my_embassy']], g[G['their_embassy']],
                      int(mine.sum()), int(sum(COST.get(int(u), 0) for u in types)), int(theirs.sum()),
                      int((theirs & (towner[d] == 1)).sum()), int((mine & (towner[d] == 2)).sum()),
                      int(np.isin(types, list(NAVAL)).sum()), int(np.isin(types, list(WARSHIPS)).sum()),
                      int(np.isin(unit[d][theirs], list(NAVAL)).sum()), th])
        urows.append(cnt)
    # events
    ev = []
    for d in np.nonzero(ok)[0]:
        ty, a, b, arg = (int(x) for x in rows[d])
        p = int(player[d])
        if ty == CMD['EndTurn']:
            continue
        nxt = d + 1 if d + 1 < D and player[d + 1] == p and turn[d + 1] == turn[d] else -1
        a_ok, b_ok = 0 <= a < W * W, 0 <= b < W * W
        if ty == CMD['Move']:
            if not (a_ok and b_ok):
                continue
            ua = int(unit[d, a])
            water_change = (terr[d, a] in WATER_TERRAIN) != (terr[d, b] in WATER_TERRAIN)
            if not (ua in (U['knight'], U['cloak'], U['dagger'], U['giant']) or water_change or imp[d, b] == CITY_IMP):
                continue
        r = [0] * len(EV)
        r[E['d']], r[E['turn']], r[E['player']], r[E['type']], r[E['a']], r[E['b']], r[E['arg']] = d, int(turn[d]), p, ty, a, b, arg
        if a_ok:
            r[E['u_a']], r[E['hp_a']], r[E['terr_a']] = int(unit[d, a]), int(hp[d, a]), int(terr[d, a])
            r[E['imp_a']], r[E['towner_a']], r[E['cap_a']] = int(imp[d, a]), int(towner[d, a]), int(cap[d, a])
            r[E['lvl_a']], r[E['pas_a']] = int(lvl[d, a]), int(pas[d, a])
        else:
            r[E['u_a']] = r[E['pas_a']] = -1
        if b_ok:
            r[E['u_b']], r[E['own_b']], r[E['hp_b']], r[E['terr_b']] = int(unit[d, b]), int(uown[d, b]), int(hp[d, b]), int(terr[d, b])
            r[E['imp_b']], r[E['cap_b']], r[E['towner_b']] = int(imp[d, b]), int(cap[d, b]), int(towner[d, b])
        else:
            r[E['u_b']] = -1
        if nxt >= 0:
            r[E['kills']] = int(glob[nxt, G['kills']] - glob[d, G['kills']])
            if b_ok:
                r[E['after_u_b']], r[E['after_own_b']] = int(unit[nxt, b]), int(uown[nxt, b])
            else:
                r[E['after_u_b']] = -1
        else:
            r[E['kills']], r[E['after_u_b']] = -1, -1
        r[E['threat']] = threat(d, a) if a_ok else 99
        ev.append(r)
    return dict(meta=meta, turns=np.asarray(trows, np.int32), units=np.asarray(urows, np.int16),
                events=np.asarray(ev, np.int16).reshape(-1, len(EV)))


def clusters(turns, gap):
    """Sorted runs of turns in which consecutive turns are at most `gap` apart."""
    out = []
    for t in sorted(set(int(x) for x in turns)):
        if out and t - out[-1][-1] <= gap:
            out[-1].append(t)
        else:
            out.append([t])
    return out


class View:
    """One player's record in one game: its events, the opponent's, and its per-turn summaries."""

    def __init__(self, g, p):
        ev = g['events']
        self.g, self.p, self.meta = g, p, g['meta']
        self.ev, self.oev = ev[ev[:, E['player']] == p], ev[ev[:, E['player']] == 3 - p]
        tr = g['turns']
        self.rows = {int(r[S['turn']]): r for r in tr if r[S['player']] == p}
        self.orows = {int(r[S['turn']]): r for r in tr if r[S['player']] == 3 - p}
        self.urows = {int(r[S['turn']]): u for r, u in zip(tr, g['units']) if r[S['player']] == p}
        self.turns = sorted(self.rows)
        self.won = g['meta']['winner'] == p
        ty = self.ev[:, E['type']]
        self.attacks = self.ev[ty == CMD['Attack']]
        self.trains = self.ev[ty == CMD['Train']]
        self.research = self.ev[ty == CMD['Research']]
        self.captures = self.ev[ty == CMD['Capture']]
        self.rewards = self.ev[ty == CMD['CityReward']]
        self.moves = self.ev[ty == CMD['Move']]
        self.city_caps = self.captures[self.captures[:, E['towner_a']] == 2]
        self.village_caps = self.captures[self.captures[:, E['towner_a']] == 0]
        oc = self.oev[self.oev[:, E['type']] == CMD['Capture']]
        self.city_losses = oc[oc[:, E['towner_a']] == 2]  # the opponent took one of this player's cities
        oa = self.oev[self.oev[:, E['type']] == CMD['Attack']]
        self.opp_attacks = oa

    def row_at(self, t):
        """The summary of this player's turn t, or of its next recorded turn (None past the end)."""
        for u in self.turns:
            if u >= t:
                return self.rows[u]
        return None

    def d_at(self, t):
        r = self.row_at(t)
        return int(r[S['d']]) if r is not None else 1 << 30

    def delta(self, col, t0, t1):
        """Change of a cumulative summary (kills, casualties) from the start of turn t0 to the start of
        turn t1 + 1 (or the last recorded turn)."""
        a = self.row_at(t0)
        b = self.row_at(t1 + 1)
        if b is None:
            b = self.rows[self.turns[-1]]
        return int(b[S[col]] - a[S[col]]) if a is not None else 0

    def lost_cities(self, t0, t1):
        d0, d1 = self.d_at(t0), self.d_at(t1 + 1)
        dd = self.city_losses[:, E['d']].astype(np.int64)
        return int(((dd >= d0) & (dd < d1)).sum())

    def net(self, t0, t1):
        return self.delta('kills', t0, t1) - self.delta('casualties', t0, t1)

    def kills_by(self, units, t0, t1, target=None):
        a = self.attacks
        m = np.isin(a[:, E['u_a']], list(units)) & (a[:, E['turn']] >= t0) & (a[:, E['turn']] <= t1) & (a[:, E['kills']] > 0)
        if target is not None:
            m &= target(a)
        return int(a[m][:, E['kills']].sum())

    def activity_turns(self, units, techs=(), rewards=(), extra=()):
        """Turns in which this player trained one of `units`, researched one of `techs`, chose one of the
        city `rewards` (e.g. SuperUnit, the giant), or any of the `extra` turns."""
        t = self.trains[np.isin(self.trains[:, E['arg']], list(units))][:, E['turn']].tolist()
        t += self.research[np.isin(self.research[:, E['arg']], list(techs))][:, E['turn']].tolist()
        t += self.rewards[np.isin(self.rewards[:, E['arg']], list(rewards))][:, E['turn']].tolist()
        return sorted(set(int(x) for x in t) | set(extra))

    def buildup(self, tp, lo, units, techs=(), back=8, gap=2, rewards=(), extra=()):
        """Start of the run of activity (activity_turns) that leads into turn lo without a gap of more than
        `gap` turns, at most `back` turns before the payoff turn tp."""
        act = set(self.activity_turns(units, techs, rewards, extra))
        t0 = lo
        while any((t0 - k) in act for k in range(1, gap + 1)) and t0 > tp - back:
            t0 = max(t0 - k for k in range(1, gap + 1) if (t0 - k) in act)
        return max(t0, tp - back)


def episode(v, kind, t0, tp, success, note=''):
    return dict(kind=kind, game=v.meta['game'], player=v.p, t0=int(t0), tp=int(tp), success=bool(success), won=v.won,
                note=note)


# ---------------------------------------------------------------- detectors, one per kind
# Each returns the player's episodes of that kind: t0 = the turn the execution starts (the first move of
# the build-up that runs into the payoff), tp = the payoff turn, success per the kind's rule.

def unit_push(v, kind, units, techs, min_kills, max_turn=99, window=4, need_capture=False, target=None, capture_units=None,
              rewards=(), extra=()):
    """Attacks with `units`: an attempt is a run of turns with such attacks (gaps <= 2). It pays off at
    the first turn tp where those units killed >= min_kills over the last `window` turns, or captured
    an enemy city (capture_units, default `units`), by turn max_turn. Success: paid off and the player
    lost no more units than it killed from the start of the episode to the turn after tp."""
    out = []
    a = v.attacks[np.isin(v.attacks[:, E['u_a']], list(units))]
    if target is not None:
        a = a[target(a)]
    cu = capture_units if capture_units is not None else units
    caps = v.city_caps[np.isin(v.city_caps[:, E['u_a']], list(cu))]
    cap_turns = set(caps[:, E['turn']].tolist())
    for cl in clusters(a[:, E['turn']].tolist() + list(cap_turns), 2):
        if cl[0] > max_turn:
            continue
        lo, found = cl[0], False
        for tp in range(cl[0], min(cl[-1], max_turn) + 1):
            if tp < lo:
                continue
            k = v.kills_by(units, max(lo, tp - window + 1), tp, target)
            capped = tp in cap_turns
            if (capped if need_capture else (k >= min_kills or capped)):
                first = min(t for t in cl if max(lo, tp - window + 1) <= t <= tp)
                t0 = v.buildup(tp, first, units, techs, rewards=rewards, extra=extra)
                ok = v.net(t0, tp) >= 0
                out.append(episode(v, kind, t0, tp, ok, f'kills {k}{" capture" if capped else ""}'))
                found, lo = True, tp + 4
        if not found:
            out.append(episode(v, kind, cl[0], cl[-1], False, 'no payoff'))
    return out


def knight_chain(v):
    """A knight that kills >= 2 units in one turn (persist), following the knight by its tile through its
    moves and the kills it moves in after. Attempt: a run of turns (gaps <= 2) in which a knight moved
    or attacked. Success: the chain happened and the player lost no more units than it killed over the
    episode."""
    out = []
    kn = U['knight']
    ev = v.ev[((v.ev[:, E['type']] == CMD['Move']) | (v.ev[:, E['type']] == CMD['Attack'])) & (v.ev[:, E['u_a']] == kn)]
    best = {}
    for t in sorted(set(ev[:, E['turn']].tolist())):
        pos, kills, nid = {}, collections.Counter(), 0
        for r in ev[ev[:, E['turn']] == t]:
            a, b = int(r[E['a']]), int(r[E['b']])
            if a not in pos:
                pos[a], nid = nid, nid + 1
            if r[E['type']] == CMD['Move']:
                pos[b] = pos.pop(a)
            else:
                if r[E['kills']] > 0:
                    kills[pos[a]] += int(r[E['kills']])
                    if r[E['after_u_b']] == kn and r[E['after_own_b']] == 1:
                        pos[b] = pos.pop(a)
        best[t] = max(kills.values(), default=0)
    last = -99
    for cl in clusters(list(best), 2):
        chain = [t for t in cl if best[t] >= 2]
        if not chain:
            out.append(episode(v, 'knight_chain', cl[0], cl[-1], False, 'no chain'))
            continue
        for tp in chain:
            if tp < last + 4:
                continue
            t0 = v.buildup(tp, max(cl[0], tp - 3), {kn}, {TECH['chivalry']})
            out.append(episode(v, 'knight_chain', t0, tp, v.net(t0, tp) >= 0, f'chain {best[tp]}'))
            last = tp
    return out


def capital_rush(v, max_turn=12):
    """The enemy capital captured by turn max_turn. Attempt: the player attacked a unit standing in
    an enemy city by max_turn. Success: the capital captured and not lost again within 3 turns."""
    out = []
    caps = v.city_caps[(v.city_caps[:, E['cap_a']] == 2) & (v.city_caps[:, E['turn']] <= max_turn)]
    if len(caps):
        r = caps[0]
        tp, tile = int(r[E['turn']]), int(r[E['a']])
        early = v.attacks[(v.attacks[:, E['turn']] <= tp) & (v.attacks[:, E['turn']] >= tp - 6)]
        t0 = int(early[0, E['turn']]) if len(early) else tp
        lost = v.city_losses[(v.city_losses[:, E['a']] == tile) & (v.city_losses[:, E['turn']] <= tp + 3)]
        out.append(episode(v, 'capital_rush', t0, tp, len(lost) == 0, f'capital at {tile}'))
    else:
        a = v.attacks[(v.attacks[:, E['turn']] <= max_turn) & (v.attacks[:, E['imp_b']] == CITY_IMP) & (v.attacks[:, E['towner_b']] == 2)]
        if len(a):
            out.append(episode(v, 'capital_rush', int(a[0, E['turn']]), max_turn, False, 'no capital'))
    return out


def catapult_siege(v):
    """Catapults bombard units in enemy territory (>= 2 catapult attacks within 3 turns), then the player
    captures an enemy city within 3 tiles of a bombarded tile within 3 turns of the last attack.
    Success: that capture."""
    out = []
    cat = v.attacks[(v.attacks[:, E['u_a']] == U['catapult']) & (v.attacks[:, E['towner_b']] == 2)]
    for cl in clusters(cat[:, E['turn']].tolist(), 2):
        c = cat[(cat[:, E['turn']] >= cl[0]) & (cat[:, E['turn']] <= cl[-1])]
        if len(c) < 2:
            continue
        hits = c[:, E['b']].astype(np.int64)
        caps = v.city_caps[(v.city_caps[:, E['turn']] >= cl[0]) & (v.city_caps[:, E['turn']] <= cl[-1] + 3)]
        caps = [r for r in caps if cheb(hits, int(r[E['a']])).min() <= 3]
        t0 = v.buildup(cl[0], cl[0], {U['catapult']}, {TECH['mathematics']})
        if caps:
            tp = int(caps[0][E['turn']])
            out.append(episode(v, 'catapult_siege', t0, tp, True, f'{len(c)} catapult attacks, capture'))
        else:
            out.append(episode(v, 'catapult_siege', t0, cl[-1], False, f'{len(c)} catapult attacks'))
    return out


def defensive_stand(v):
    """Enemy units inside the player's territory at the start of >= 2 of its turns within 3 turns (the
    threat). The stand: over the threat turn and the 5 after, the player trains >= 2 defenders or
    archers, or its defenders and archers make >= 2 kills. Success: no city lost in that window, >= 3
    kills of units inside its own territory, and more kills than losses."""
    out = []
    thr = [t for t in v.turns if v.rows[t][S['enemy_in_mine']] >= 2 and t >= 3]
    line = {U['defender'], U['archer']}
    for cl in clusters(thr, 2):
        if len(cl) < 2:
            continue
        t0, t1 = cl[0], cl[0] + 5
        trained = v.trains[np.isin(v.trains[:, E['arg']], list(line)) & (v.trains[:, E['turn']] >= t0) & (v.trains[:, E['turn']] <= t1)]
        lk = v.kills_by(line, t0, t1)
        if len(trained) < 2 and lk < 2:
            continue
        home = v.kills_by(set(U.values()), t0, t1, target=lambda a: a[:, E['towner_b']] == 1)
        ok = v.lost_cities(t0, t1) == 0 and home >= 3 and v.net(t0, t1) > 0
        out.append(episode(v, 'defensive_line', t0, t1, ok, f'{len(trained)} line units, {lk} line kills, {home} home kills'))
    return out


def city_wall(v):
    """The CityWall reward taken with an enemy unit within 3 tiles of the city. Success: the city not lost
    in the 6 turns after, and >= 1 kill of a unit within 2 tiles of it."""
    out = []
    w = v.rewards[(v.rewards[:, E['arg']] == REWARD['CityWall']) & (v.rewards[:, E['threat']] <= 3)]
    last = -99
    for r in w:
        tp, tile = int(r[E['turn']]), int(r[E['a']])
        if tp < last + 3:
            continue
        last = tp
        lost = v.city_losses[(v.city_losses[:, E['a']] == tile) & (v.city_losses[:, E['turn']] >= tp) & (v.city_losses[:, E['turn']] <= tp + 6)]
        near = v.kills_by(set(U.values()), tp, tp + 6, target=lambda a: cheb(a[:, E['b']].astype(np.int64), tile) <= 2)
        out.append(episode(v, 'city_wall', tp, tp, len(lost) == 0 and near >= 1 and v.rows[v.turns[-1]][S['turn']] >= tp + 2,
                           f'wall at {tile}, {near} kills near'))
    return out


def landings(v):
    """Boat moves from water onto land carrying a unit: (turn, tile, passenger type)."""
    m = v.moves
    m = m[np.isin(m[:, E['terr_a']], WATER_TERRAIN) & ~np.isin(m[:, E['terr_b']], WATER_TERRAIN)
          & np.isin(m[:, E['u_a']], list(NAVAL)) & (m[:, E['pas_a']] >= 0)]
    return m


def giant_embarks(v):
    """Turns a giant boarded a boat (moved from land onto water: at a port it becomes a juggernaut)."""
    m = v.moves
    m = m[(m[:, E['u_a']] == U['giant']) & ~np.isin(m[:, E['terr_a']], WATER_TERRAIN) & np.isin(m[:, E['terr_b']], WATER_TERRAIN)]
    return set(int(t) for t in m[:, E['turn']])


def embark_turns(v):
    m = v.moves
    m = m[~np.isin(m[:, E['terr_a']], WATER_TERRAIN) & np.isin(m[:, E['terr_b']], WATER_TERRAIN) & ~np.isin(m[:, E['u_a']], list(NAVAL))]
    return set(m[:, E['turn']].tolist())


def naval_capture(v, kind, caps, back=3, dist=2):
    """Captures made by a unit that landed from a boat: a landing of the same unit type within `back`
    turns before and `dist` tiles of the captured tile. t0: the first embarkation within 3 turns before
    the landing."""
    out = []
    land = landings(v)
    emb = embark_turns(v)
    last = -99
    for r in caps:
        tp, tile, ut = int(r[E['turn']]), int(r[E['a']]), int(r[E['u_a']])
        if tp < last + 3:
            continue
        l = land[(land[:, E['turn']] >= tp - back) & (land[:, E['turn']] <= tp) & (land[:, E['pas_a']] == ut)]
        l = l[cheb(l[:, E['b']].astype(np.int64), tile) <= dist] if len(l) else l
        if not len(l):
            continue
        tl = int(l[0, E['turn']])
        e = [t for t in emb if tl - 3 <= t <= tl]
        t0 = min(e) if e else tl
        out.append(episode(v, kind, t0, tp, True, f'landed {tl}, captured {tile}'))
        last = tp
    return out


def naval_landing(v):
    """An enemy city captured by a unit that came ashore from a boat (naval_capture). Attempts: landings
    within 2 tiles of an enemy-owned tile (the boat's target tile in enemy territory) that captured no city."""
    eps = naval_capture(v, 'naval_landing', v.city_caps)
    land = landings(v)
    hostile = land[land[:, E['towner_b']] == 2]
    used = {e['tp'] for e in eps}
    for cl in clusters(hostile[:, E['turn']].tolist(), 2):
        if not any(cl[0] <= t <= cl[-1] + 3 for t in used):
            eps.append(episode(v, 'naval_landing', cl[0], cl[-1], False, 'landing, no capture'))
    return eps


def naval_expansion(v):
    """>= 2 villages captured by units that came ashore from boats within 5 turns (naval_capture on
    villages, dist 1). Success: by construction (the villages were taken)."""
    eps = naval_capture(v, 'naval_expansion', v.village_caps, back=2, dist=1)
    out = []
    for cl in clusters([e['tp'] for e in eps], 4):
        mine = [e for e in eps if cl[0] <= e['tp'] <= cl[-1]]
        if len(mine) >= 2:
            out.append(episode(v, 'naval_expansion', min(e['t0'] for e in mine), mine[1]['tp'], True, f'{len(mine)} villages by boat'))
    return out


def ship_losses(v, t0, t1):
    a = v.opp_attacks
    m = (a[:, E['turn']] >= t0) & (a[:, E['turn']] <= t1) & np.isin(a[:, E['u_b']], list(NAVAL)) & (a[:, E['kills']] > 0)
    return int(m.sum())


def naval_battle(v):
    """Warships killing enemy boats: >= 2 such kills within 4 turns. Attempt: a run of warship attacks on
    boats. Success: more boats sunk than boats lost over the episode."""
    out = []
    ships = WARSHIPS - {U['juggernaut']}
    tgt = lambda a: np.isin(a[:, E['u_b']], list(NAVAL))
    for e in unit_push(v, 'naval_battle', ships, (TECH['sailing'], TECH['ramming'], TECH['navigation']), 2, target=tgt,
                       capture_units=()):
        if e['success']:
            e['success'] = v.kills_by(ships, e['t0'], e['tp'], tgt) > ship_losses(v, e['t0'], e['tp'] + 1)
        out.append(e)
    return out


def naval_bombard(v):
    """Warships killing land units from the water: >= 2 within 4 turns (water control, sniping). Success:
    paid off, and fewer boats lost than land units killed."""
    out = []
    ships = WARSHIPS - {U['juggernaut']}
    tgt = lambda a: ~np.isin(a[:, E['u_b']], list(NAVAL)) & ~np.isin(a[:, E['terr_b']], WATER_TERRAIN)
    for e in unit_push(v, 'naval_bombard', ships, (TECH['sailing'], TECH['ramming'], TECH['navigation']), 2, target=tgt,
                       capture_units=()):
        if e['success']:
            e['success'] = v.kills_by(ships, e['t0'], e['tp'], tgt) > ship_losses(v, e['t0'], e['tp'] + 1)
        out.append(e)
    return out


def mind_bender(v):
    """Conversions: a mind bender's attack after which the target is the player's. >= 2 within 4 turns.
    Attempt: a run of mind bender actions (conversions tried). Success: the conversions happened."""
    out = []
    a = v.attacks[v.attacks[:, E['u_a']] == U['mindbender']]
    conv = a[a[:, E['after_own_b']] == 1]
    for cl in clusters(a[:, E['turn']].tolist(), 2):
        c = conv[(conv[:, E['turn']] >= cl[0]) & (conv[:, E['turn']] <= cl[-1])]
        if len(c) >= 2:
            ts = sorted(c[:, E['turn']].tolist())
            tp = next(ts[i + 1] for i in range(len(ts) - 1) if ts[i + 1] - ts[i] <= 3) if any(ts[i + 1] - ts[i] <= 3 for i in range(len(ts) - 1)) else None
            if tp is not None:
                t0 = v.buildup(tp, cl[0], {U['mindbender']}, {TECH['philosophy']})
                out.append(episode(v, 'mind_bender', t0, tp, True, f'{len(c)} conversions'))
                continue
        out.append(episode(v, 'mind_bender', cl[0], cl[-1], False, f'{len(c)} conversions'))
    return out


def cloak_dagger(v):
    """A cloak infiltrates an enemy city (its attack on a city tile), then within 3 turns its daggers kill
    >= 1 unit or the player captures a city within 2 tiles. Success: that payoff."""
    out = []
    inf = v.attacks[(v.attacks[:, E['u_a']] == U['cloak']) & (v.attacks[:, E['imp_b']] == CITY_IMP) & (v.attacks[:, E['towner_b']] == 2)]
    last = -99
    for r in inf:
        tp, tile = int(r[E['turn']]), int(r[E['b']])
        if tp < last + 3:
            continue
        last = tp
        dk = v.kills_by({U['dagger']}, tp, tp + 3)
        caps = v.city_caps[(v.city_caps[:, E['turn']] >= tp) & (v.city_caps[:, E['turn']] <= tp + 3)]
        capped = any(cheb(int(c[E['a']]), tile) <= 2 for c in caps)
        t0 = v.buildup(tp, tp, {U['cloak']}, {TECH['diplomacy']})
        out.append(episode(v, 'cloak_dagger', t0, tp, dk >= 1 or capped, f'dagger kills {dk}{" capture" if capped else ""}'))
    return out


def recapture(v):
    """A city lost and taken back within 5 turns, then held for 2 turns. t0: the recapture's own
    turn (positions are taken between the loss and it). Attempt: every city lost before the last 3 turns."""
    out = []
    for r in v.city_losses:
        tl, tile = int(r[E['turn']]), int(r[E['a']])
        back = v.city_caps[(v.city_caps[:, E['a']] == tile) & (v.city_caps[:, E['d']] > r[E['d']]) & (v.city_caps[:, E['turn']] <= tl + 5)]
        if len(back):
            tp = int(back[0, E['turn']])
            again = v.city_losses[(v.city_losses[:, E['a']] == tile) & (v.city_losses[:, E['d']] > back[0, E['d']]) & (v.city_losses[:, E['turn']] <= tp + 2)]
            out.append(episode(v, 'recapture', tp, tp, len(again) == 0, f'lost {tl} at {tile}', ))
            out[-1]['after_d'] = int(r[E['d']])
        elif tl <= v.meta['turns'] - 3:
            out.append(episode(v, 'recapture', tl + 1, tl + 5, False, 'not retaken'))
    return out


def economic_boom(v, window=5):
    """A boom: in `window` of the player's turns from turn >= 4, >= 4 techs researched and >= 1 city gained
    (or >= 5 techs) while training <= 2 units, with no attacks of its own. Success: by turn t0 + 8 the
    player's income is at least the opponent's latest income and 1.5x its own at t0, and it won the game."""
    out = []
    last = -99
    for t0 in v.turns:
        if t0 < 4 or t0 < last + window:
            continue
        t1 = t0 + window - 1
        nt = int(((v.research[:, E['turn']] >= t0) & (v.research[:, E['turn']] <= t1)).sum())
        nu = int(((v.trains[:, E['turn']] >= t0) & (v.trains[:, E['turn']] <= t1)).sum())
        na = int(((v.attacks[:, E['turn']] >= t0) & (v.attacks[:, E['turn']] <= t1)).sum())
        r0, r1 = v.row_at(t0), v.row_at(t1 + 1)
        if r1 is None:
            break
        dc = int(r1[S['cities']] - r0[S['cities']])
        if not ((nt >= 4 and dc >= 1) or nt >= 5) or nu > 2 or na > 0:
            continue
        r8 = v.row_at(t0 + 8)
        if r8 is None:
            r8 = v.rows[v.turns[-1]]
        o8 = [v.orows[t] for t in v.orows if t <= int(r8[S['turn']])]
        oinc = int(o8[-1][S['income']]) if o8 else 0
        ok = v.won and r8[S['income']] >= max(oinc, 1.5 * r0[S['income']])
        out.append(episode(v, 'economic_boom', t0, t1, ok, f'{nt} techs, +{dc} cities, {nu} units'))
        last = t0
    return out


def diplomacy(v):
    """Peace treaties and embassies: the player's PeaceTreaty (proposal), PeaceRequestResponse or
    EstablishEmbassy. Success: the player won the game."""
    m = v.ev[np.isin(v.ev[:, E['type']], [CMD['PeaceTreaty'], CMD['PeaceRequestResponse'], CMD['EstablishEmbassy']])]
    return [episode(v, 'diplomacy', cl[0], cl[0], v.won, 'diplomacy') for cl in clusters(m[:, E['turn']].tolist(), 3)]


DETECTORS = {
    'capital_rush': capital_rush,
    'rider_rush': lambda v: unit_push(v, 'rider_rush', {U['rider']}, {TECH['riding']}, 3, max_turn=14),
    'early_rush': lambda v: unit_push(v, 'early_rush', {U['warrior'], U['archer']}, {TECH['archery']}, 3, max_turn=10),
    'knight_chain': knight_chain,
    'swordsman_push': lambda v: unit_push(v, 'swordsman_push', {U['swordsman']}, {TECH['smithery']}, 3),
    'giant_push': lambda v: unit_push(v, 'giant_push', {U['giant']}, (), 3, rewards={REWARD['SuperUnit']}),
    'juggernaut': lambda v: unit_push(v, 'juggernaut', {U['juggernaut']}, (), 2, rewards={REWARD['SuperUnit']},
                                      extra=giant_embarks(v)),
    'catapult_siege': catapult_siege,
    'defensive_line': defensive_stand,
    'city_wall': city_wall,
    'naval_landing': naval_landing,
    'naval_expansion': naval_expansion,
    'naval_battle': naval_battle,
    'naval_bombard': naval_bombard,
    'economic_boom': economic_boom,
    'mind_bender': mind_bender,
    'cloak_dagger': cloak_dagger,
    'recapture': recapture,
    'diplomacy': diplomacy,
}


def is_val(game):
    """train_bc.is_val for a human game id (not imported: train_bc needs torch)."""
    return zlib.crc32(game.encode()) % HUMAN_VAL_EVERY == 0


def start_candidates(v, e, min_ratio):
    """Start positions for episode e: the executing player's turns t0 - 1, t0 - 2, t0 - 3 (lead, turn,
    decision), each at its first recorded decision of the turn, from turn 1 on, and only where the game
    is not decided yet: both players have a city and the lower score is >= min_ratio of the higher
    (as py/human_positions.py). A recapture's positions must come after the city was lost."""
    out = []
    for lead in (1, 2, 3):
        t = e['t0'] - lead
        r = v.rows.get(t)
        if t < 1 or r is None:
            continue
        d = int(r[S['d']])
        if 'after_d' in e and d <= e['after_d']:
            continue
        prev = [o for o in v.orows.values() if o[S['d']] < d]
        if not prev:
            continue
        o = max(prev, key=lambda o: o[S['d']])
        s, so = int(r[S['score']]), int(o[S['score']])
        if r[S['cities']] < 1 or o[S['cities']] < 1 or min(s, so) < min_ratio * max(s, so):
            continue
        out.append((lead, t, d))
    return out


def game_episodes(g, kinds=None, min_ratio=0.4):
    out = []
    for p in (1, 2):
        v = View(g, p)
        if not v.turns:
            continue
        for k, f in DETECTORS.items():
            if kinds is None or k in kinds:
                for e in f(v):
                    e['split'] = 'val' if is_val(e['game']) else 'train'
                    e['cands'] = start_candidates(v, e, min_ratio) if e['success'] else []
                    out.append(e)
    return out


def _episodes_job(args):
    g, min_ratio = args
    return game_episodes(g, min_ratio=min_ratio)


def select(eps, kinds, split, cap, rng, taken):
    """At most `cap` positions per kind from the successful episodes of `split`, spread over games: first
    one position per game, then one per remaining episode, then the episodes' other leads. A position
    (game, turn, player) is used once: kinds with fewer episodes choose first."""
    rows = {}
    pools = {k: [e for e in eps if e['kind'] == k and e['split'] == split and e['cands']] for k in kinds}
    for k in sorted(kinds, key=lambda k: len(pools[k])):
        pool = pools[k]
        rng.shuffle(pool)
        seen, first, rest = set(), [], []
        for e in pool:
            (rest if e['game'] in seen else first).append(e)
            seen.add(e['game'])
        chosen, used = [], []
        for e in first + rest:
            if len(chosen) >= cap:
                break
            for lead, t, d in sorted(e['cands'], key=lambda c: rng.random()):
                key = (e['game'], t, e['player'])
                if key not in taken:
                    taken.add(key)
                    chosen.append((e, lead, t, d))
                    used.append(e)
                    break
        for e in used:
            for lead, t, d in e['cands']:
                key = (e['game'], t, e['player'])
                if len(chosen) < cap and key not in taken:
                    taken.add(key)
                    chosen.append((e, lead, t, d))
        rows[k] = chosen
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default=os.path.join(REPO, 'data', 'human1'))
    ap.add_argument('--cache', default=os.path.join(REPO, 'runs', 'strategy', 'events.pkl'))
    ap.add_argument('--out', default=os.path.join(REPO, 'runs', 'strategy', 'strategy_positions'))
    ap.add_argument('--cap', type=int, default=600, help='positions per kind from the training games')
    ap.add_argument('--val_cap', type=int, default=60, help='positions per kind from the held-out games')
    ap.add_argument('--min_episodes', type=int, default=100, help='drop kinds with fewer successful training episodes')
    ap.add_argument('--min_version', type=int, default=121, help='as py/human_starts.py: older games are not built')
    ap.add_argument('--min_ratio', type=float, default=0.4)
    ap.add_argument('--only', default=None, help='a file of game ids to use (e.g. the replays the PC has)')
    ap.add_argument('--procs', type=int, default=4)
    ap.add_argument('--seed', type=int, default=0)
    args = ap.parse_args()
    if os.path.exists(args.cache):
        games = pickle.load(open(args.cache, 'rb'))
        print(f'{len(games)} games from {args.cache}', flush=True)
    else:
        games = extract_all(args.data, args.cache, args.procs)
    only = {l.split()[0] for l in open(args.only) if l.strip()} if args.only else None
    use = [g for g in games.values() if g['events'] is not None and g['meta']['exact'] and g['meta']['version'] >= args.min_version
           and (only is None or g['meta']['game'] in only)]
    print(f"{len(use)} games: exact replay, version >= {args.min_version}{', in --only' if only else ''} "
          f"({sum(is_val(g['meta']['game']) for g in use)} held out)", flush=True)
    with mp.Pool(args.procs) as pool:
        eps = [e for es in pool.imap(_episodes_job, [(g, args.min_ratio) for g in use], chunksize=32) for e in es]
    meta = {g['meta']['game']: g['meta'] for g in use}
    os.makedirs(os.path.dirname(args.out) or '.', exist_ok=True)
    with open(args.out + '_episodes.tsv', 'w') as f:
        f.write('kind\tgame\tplayer\tt0\tpayoff_turn\tsuccess\twon\tsplit\tstarts\tnote\n')
        for e in eps:
            f.write(f"{e['kind']}\t{e['game']}\t{e['player']}\t{e['t0']}\t{e['tp']}\t{int(e['success'])}\t{int(e['won'])}\t"
                    f"{e['split']}\t{len(e['cands'])}\t{e['note']}\n")
    stat = {k: collections.Counter() for k in DETECTORS}
    for e in eps:
        s = stat[e['kind']]
        s['attempts'] += 1
        if e['success']:
            s['success'] += 1
            s['won'] += e['won']
            s[e['split']] += 1
            s['startable_' + e['split']] += bool(e['cands'])
    kinds = [k for k in DETECTORS if stat[k]['startable_train'] >= args.min_episodes]
    rng = random.Random(args.seed)
    taken = set()
    chosen = {'train': select(eps, kinds, 'train', args.cap, rng, taken), 'val': select(eps, kinds, 'val', args.val_cap, rng, taken)}
    cols = ['game', 'turn', 'player', 'decision', 'tribes', 'version', 'game_turns', 'strategy', 'lead', 'episode_t0',
            'payoff_turn', 'split', 'won', 'note']
    n = 0
    with open(args.out + '.tsv', 'w') as f:
        f.write('\t'.join(cols) + '\n')
        for split in ('train', 'val'):
            for k in kinds:
                for e, lead, t, d in sorted(chosen[split][k], key=lambda c: (c[0]['game'], c[2])):
                    m = meta[e['game']]
                    f.write('\t'.join(str(x) for x in (e['game'], t, e['player'], d, f"{m['tribes'][0]},{m['tribes'][1]}", m['version'],
                                                         m['turns'], k, lead, e['t0'], e['tp'], split, int(e['won']), e['note'])) + '\n')
                    n += 1
    hdr = 'kind attempts success rate won% train_eps val_eps train_rows val_rows'
    lines = [hdr]
    for k in DETECTORS:
        s = stat[k]
        lines.append(f"{k} {s['attempts']} {s['success']} {s['success'] / max(s['attempts'], 1):.2f} {s['won'] / max(s['success'], 1):.2f} "
                     f"{s['startable_train']} {s['startable_val']} {len(chosen['train'].get(k, []))} {len(chosen['val'].get(k, []))}"
                     f"{'' if k in kinds else ' dropped'}")
    with open(args.out + '_summary.tsv', 'w') as f:
        f.write('\n'.join(l.replace(' ', '\t') for l in lines) + '\n')
    print('\n'.join(lines))
    print(f'{n} positions ({len(kinds)} kinds) -> {args.out}.tsv; episodes -> {args.out}_episodes.tsv')


if __name__ == '__main__':
    main()


def extract_all(data, cache, procs):
    files = sorted(os.path.join(data, f) for f in os.listdir(data) if f.endswith('.npz') and not f.endswith('.tmp.npz'))
    out = {}
    with mp.Pool(procs) as pool:
        for i, g in enumerate(pool.imap_unordered(extract, files, chunksize=8)):
            out[g['meta']['game']] = g
            if (i + 1) % 1000 == 0:
                print(f'extract: {i + 1}/{len(files)} games', flush=True)
    os.makedirs(os.path.dirname(cache) or '.', exist_ok=True)
    with open(cache + '.tmp', 'wb') as f:
        pickle.dump(out, f, protocol=pickle.HIGHEST_PROTOCOL)
    os.replace(cache + '.tmp', cache)
    return out
