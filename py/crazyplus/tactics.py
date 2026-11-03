"""The board as the bot sees it (full view: both players' observations of the true state) and the
candidate command sequences for one decision.

Candidates are only proposals: each is scored exactly by a rollout on a clone (bot.py), so the
estimates here (the strategy book's damage formula, a crude reach-based danger map) only decide
which sequences are worth simulating.
"""
import numpy as np

import polyenv
from rulebot import combat as C
from rulebot import data as D
from crazyplus.evalfn import UNIT_VALUE, HULL_VALUE

F = {n: i for i, n in enumerate(polyenv.TILE_FEATURES)}
MOVE, ATTACK, CAPTURE, RECOVER = 6, 2, 7, 3
# the order candidates are tried in (a decision's time budget may cut the tail)
ORDER = ('pending', 'city', 'kills', 'unit', 'garrison', 'attacks')
UNIT_CMDS = {1: False, 2: True, 3: True, 4: True, 6: True, 7: True, 13: True, 14: True, 16: True, 36: True}


class U:
    __slots__ = ('tile', 'type', 'hp', 'maxhp', 'moved', 'attacked', 'passenger', 'vet', 'mine', 'st', 'value')

    def __init__(self, tile, f, mine):
        self.tile = tile
        self.type = int(f[F['unit']]) - 1
        self.hp = int(f[F['unit_health']])
        self.maxhp = max(1, int(f[F['unit_max_health']]))
        self.moved = bool(f[F['unit_moved']])
        self.attacked = bool(f[F['unit_attacked']])
        self.passenger = int(f[F['unit_passenger']]) - 1
        self.vet = bool(f[F['unit_promoted']])
        self.mine = mine
        self.st = D.stats(self.type)
        base = UNIT_VALUE[max(self.passenger, 0)] + HULL_VALUE[self.type] if self.type in D.NAVAL else UNIT_VALUE[self.type]
        self.value = float(base)

    def __repr__(self):
        return f"{'' if self.mine else 'E:'}{D.UNIT_NAME.get(self.type, self.type)}@{D.xy(self.tile)}{self.hp}"


class Board:
    def __init__(self, obs_me, obs_op, legal):
        tm = obs_me.tiles.reshape(D.N, -1)
        to = obs_op.tiles.reshape(D.N, -1)
        self.tm, self.to = tm, to
        seen = tm[:, F['explored']] > 0
        self.terrain = np.where(seen, tm[:, F['terrain']], to[:, F['terrain']])
        self.my_units, self.en_units, self.unit_at = [], [], {}
        for i in np.nonzero((tm[:, F['unit']] > 0) & (tm[:, F['unit_owner']] == 1))[0]:
            u = U(int(i), tm[i], True)
            self.my_units.append(u)
            self.unit_at[u.tile] = u
        for i in np.nonzero((to[:, F['unit']] > 0) & (to[:, F['unit_owner']] == 1))[0]:
            u = U(int(i), to[i], False)
            self.en_units.append(u)
            self.unit_at[u.tile] = u
        # cities: owner relative to me (1 mine, 2 enemy, 0 village)
        self.city = {}
        for i in np.nonzero(tm[:, F['improvement']] == D.CITY)[0]:
            i = int(i)
            self.city[i] = (int(tm[i, F['owner']]), int(tm[i, F['imp_level']]), bool(tm[i, F['imp_rewards']] & (1 << D.R_WALL)),
                            int(tm[i, F['capital_of']]) != 0)
        for i in np.nonzero((to[:, F['improvement']] == D.CITY) & (to[:, F['owner']] == 1))[0]:
            i = int(i)
            self.city[i] = (2, int(to[i, F['imp_level']]), bool(to[i, F['imp_rewards']] & (1 << D.R_WALL)),
                            int(to[i, F['capital_of']]) != 0)
        self.my_techs = set(int(t) for t in np.nonzero(obs_me.techs)[0])
        self.en_techs = set(int(t) for t in np.nonzero(obs_op.techs)[0])
        self.legal = legal
        self.moves, self.attacks, self.captures, self.trains, self.recover = {}, {}, [], {}, {}
        for k, (ty, a, b, arg) in enumerate(legal.tolist()):
            if ty == 5:
                self.trains.setdefault(a, {})[arg] = k
            elif ty == RECOVER:
                self.recover[a] = k
            elif ty == MOVE:
                self.moves.setdefault(a, {})[b] = k
            elif ty == ATTACK:
                self.attacks.setdefault(a, {})[b] = k
            elif ty == CAPTURE:
                self.captures.append(k)
        self._danger = {}

    # ---- combat estimates
    def bonus(self, tile, u):
        techs = self.my_techs if u.mine else self.en_techs
        c = self.city.get(tile)
        if c is not None and c[0] == (1 if u.mine else 2) and 'fortify' in u.st.abil:
            return 4.0 if c[2] else 1.5
        ter = self.terrain[tile]
        naval = u.type in D.NAVAL
        if ter == D.FOREST and D.T['archery'] in techs and not naval:
            return 1.5
        if ter == D.MOUNTAIN and D.T['climbing'] in techs and not naval:
            return 1.5
        if ter in (D.WATER, D.OCEAN) and naval and D.T['aquatism'] in techs:
            return 1.5
        return 1.0

    def battle(self, att, from_tile, dfn, dhp=None, at_tile=None):
        """(damage, retaliation) of `att` attacking from `from_tile` at `dfn` (standing on at_tile)."""
        at_tile = dfn.tile if at_tile is None else at_tile
        dhp = dfn.hp if dhp is None else dhp
        return C.battle(att.type, att.hp, att.maxhp, dfn.type, dhp, dfn.maxhp, self.bonus(at_tile, dfn),
                        D.dist(from_tile, at_tile))

    def reach(self, u):
        mv = u.st.mov if 'dash' in u.st.abil else 0
        return mv + u.st.rng

    def danger(self, u, tile):
        """Rough expected loss (stars) of my unit `u` if it stands on `tile` through the enemy's turn."""
        key = (u.tile, tile)
        if key in self._danger:
            return self._danger[key]
        hp = u.hp
        for e in sorted(self.en_units, key=lambda e: -e.st.atk):
            if e.st.atk <= 0 or D.dist(e.tile, tile) > self.reach(e):
                continue
            dmg, _ = C.battle(e.type, e.hp, e.maxhp, u.type, hp, u.maxhp, self.bonus(tile, u), 1)
            hp -= dmg
            if hp <= 0:
                break
        loss = u.value * (1.0 if hp <= 0 else 0.5 * (u.hp - hp) / u.maxhp)
        self._danger[key] = loss
        return loss

    def enemy_capital(self):
        for t, c in self.city.items():
            if c[0] == 2 and c[3]:
                return t
        return None

    def near_enemy(self, tile, r):
        return any(D.dist(e.tile, tile) <= r for e in self.en_units)

    def attack_score(self, att, from_tile, dfn, dhp=None):
        if 'convert' in att.st.abil:
            return 2.0 * dfn.value, False
        dmg, ret = self.battle(att, from_tile, dfn, dhp)
        hp = dfn.hp if dhp is None else dhp
        kill = dmg >= hp
        gain = dfn.value * (1.0 if kill else 0.6 * dmg / dfn.maxhp)
        loss = att.value * 0.6 * min(ret, att.hp) / att.maxhp + (att.value if ret >= att.hp else 0)
        return gain - loss, kill


def kill_plans(board, legal, max_attackers=4, max_targets=4):
    """Greedy multi-unit kill plans: for the most urgent enemy units (on or next to my cities first,
    then the most valuable), pick attackers one by one (no-retaliation hits first, the finisher
    last), from where they stand or after one move, until the estimate says the target dies.
    Returns [(score, seq)]."""
    mine_cities = [t for t, c in board.city.items() if c[0] == 1]
    def urgency(e):
        d = min([D.dist(e.tile, t) for t in mine_cities] or [9])
        on_capital = any(board.city[t][3] and t == e.tile for t in mine_cities)
        return (100 if on_capital else 30 if d == 0 else 12 if d == 1 else 4 if d == 2 else 0) + e.value
    targets = sorted((e for e in board.en_units if e.hp > 0), key=urgency, reverse=True)[:max_targets]
    out = []
    occupied = set(board.unit_at)
    for e in targets:
        opts = []  # (unit, from tile, prefix rows, attack row)
        for u in board.my_units:
            if u.attacked or u.st.atk <= 0 and 'convert' not in u.st.abil:
                continue
            k = board.attacks.get(u.tile, {}).get(e.tile)
            if k is not None:
                opts.append((u, u.tile, [], _row(legal, k)))
            elif not u.moved and 'dash' in u.st.abil and D.dist(u.tile, e.tile) <= u.st.mov + u.st.rng + 1:
                best = None
                for m, km in board.moves.get(u.tile, {}).items():
                    if m in occupied or D.dist(m, e.tile) > u.st.rng:
                        continue
                    dz = board.danger(u, m)
                    if best is None or dz < best[0]:
                        best = (dz, m, km)
                if best is not None:
                    opts.append((u, best[1], [_row(legal, best[2])], (ATTACK, best[1], e.tile, None)))
        if not opts:
            continue
        hp, plan, used, taken, ret_cost = e.hp, [], set(), set(), 0.0
        while hp > 0 and len(used) < max_attackers:
            best = None
            for u, m, pre, att in opts:
                if id(u) in used or (pre and m in taken):
                    continue
                if 'convert' in u.st.abil:
                    dmg, ret = hp, 0
                else:
                    dmg, ret = C.battle(u.type, u.hp, u.maxhp, e.type, hp, e.maxhp, board.bonus(e.tile, e), D.dist(m, e.tile))
                if dmg <= 0:
                    continue
                kills = dmg >= hp
                # finish if we can; otherwise soften with the hit that costs least retaliation per damage
                key = (kills, -ret if not kills else -u.value, dmg)
                if best is None or key > best[0]:
                    best = (key, u, m, pre, att, dmg, ret)
            if best is None:
                break
            _, u, m, pre, att, dmg, ret = best
            used.add(id(u))
            if pre:
                taken.add(m)
            plan += pre + [att]
            hp -= dmg
            ret_cost += u.value * min(ret, u.hp) / u.maxhp
        if hp <= 0 and len(used) >= 2:
            out.append((urgency(e) - ret_cost, plan))
    out.sort(key=lambda x: -x[0])
    return out


def _row(legal, k):
    return tuple(int(x) for x in legal[k])


def candidates(board, ai_row, p, has_ops, pending):
    """[(sequence of rows, reason)] worth simulating against the AI's command `ai_row`."""
    legal = board.legal
    seen = set()
    groups = {g: [] for g in ORDER}
    if ai_row is not None:
        seen.add((ai_row,))

    def add(seq, why, g):
        key = tuple(seq)
        if key not in seen:
            seen.add(key)
            groups[g].append((list(seq), why))

    for seq, why in pending:
        add(seq, why, 'pending')
    mine = {u.tile: u for u in board.my_units}
    # 1. attacks from where units stand (every one, best first), and kill combinations
    singles = []
    for a, targets in board.attacks.items():
        u = mine.get(a)
        if u is None:
            continue
        for b, k in targets.items():
            e = board.unit_at.get(b)
            if e is None or e.mine:
                continue
            s, kill = board.attack_score(u, a, e)
            singles.append((s + (3 if kill else 0), [_row(legal, k)], 'kill' if kill else 'attack', u, e))
    singles.sort(key=lambda x: -x[0])
    # two attacks that kill together
    pairs = []
    by_target = {}
    for s, seq, why, u, e in singles:
        by_target.setdefault(e.tile, []).append((u, seq[0]))
    for et, atts in by_target.items():
        e = board.unit_at[et]
        if len(atts) < 2:
            continue
        for i, (u1, r1) in enumerate(atts):
            d1, _ = board.battle(u1, u1.tile, e)
            if d1 >= e.hp:
                continue
            for u2, r2 in atts:
                if u2 is u1:
                    continue
                d2, _ = board.battle(u2, u2.tile, e, dhp=e.hp - d1)
                if d1 + d2 >= e.hp:
                    sc, _ = board.attack_score(u1, u1.tile, e)
                    pairs.append((e.value + sc, [r1, r2], 'focus kill'))
    pairs.sort(key=lambda x: -x[0])
    # move, then attack (units that can still move and attack)
    mkills = []
    for u in board.my_units:
        if u.moved or u.attacked or 'dash' not in u.st.abil or u.st.atk <= 0 or u.tile not in board.moves:
            continue
        for e in board.en_units:
            if D.dist(u.tile, e.tile) > u.st.mov + u.st.rng + 1:
                continue
            best = None
            for m in board.moves[u.tile]:
                if D.dist(m, e.tile) > u.st.rng or m == e.tile:
                    continue
                dmg, ret = C.battle(u.type, u.hp, u.maxhp, e.type, e.hp, e.maxhp, board.bonus(e.tile, e), D.dist(m, e.tile))
                if dmg < e.hp and 'convert' not in u.st.abil:
                    continue
                dz = board.danger(u, m if e.tile != m else e.tile)
                sc = e.value - 0.5 * dz
                if best is None or sc > best[0]:
                    best = (sc, m)
            if best is not None:
                mkills.append((best[0], [_row(legal, board.moves[u.tile][best[1]]), (ATTACK, best[1], e.tile, None)], 'move+kill'))
    mkills.sort(key=lambda x: -x[0])
    for s, seq, why, u, e in singles:
        if why == 'kill':
            add(seq, why, 'kills')
    for s, seq, why in pairs[:3]:
        add(seq, why, 'kills')
    for s, seq, why in mkills[:3]:
        add(seq, why, 'kills')
    for s, seq, why, u, e in singles[:5]:
        add(seq, why, 'attacks')
    if p.get('kill_plans', True):
        for s, seq in kill_plans(board, legal)[:3]:
            add(seq, 'kill plan', 'kills')
    # 2. captures, and walking into an empty enemy city
    for k in board.captures:
        add([_row(legal, k)], 'capture', 'city')
    walk = []
    for u in board.my_units:
        for b, k in board.moves.get(u.tile, {}).items():
            c = board.city.get(b)
            if c is not None and c[0] != 1 and b not in board.unit_at:
                walk.append(((20 if c[3] else 5 + c[1]) - board.danger(u, b), [_row(legal, k)], 'walk into city'))
    walk.sort(key=lambda x: -x[0])
    for s, seq, why in walk[:3]:
        add(seq, why, 'city')
    # 3. garrisons: an empty city of mine with enemies near gets a unit (walked in, or trained)
    if p.get('garrison', True):
        for t, c in board.city.items():
            if c[0] != 1 or t in board.unit_at or not board.near_enemy(t, 2):
                continue
            walkers = [(board.danger(u, t) - u.st.df / 10.0, u, board.moves[u.tile][t]) for u in board.my_units
                       if t in board.moves.get(u.tile, {})]
            if walkers:
                add([_row(legal, min(walkers, key=lambda x: x[0])[2])], 'garrison (walk in)', 'garrison')
            trains = [(D.stats(int(legal[k][3])).df * D.stats(int(legal[k][3])).hp, k) for k in board.trains.get(t, {}).values()]
            if trains and (board.near_enemy(t, 1) or c[3]):  # (an enemy within 2 of a non-capital: mostly vetoed)
                add([_row(legal, max(trains)[1])], 'garrison (train)', 'garrison')
    if p.get('garrison', True):
        for t, c in board.city.items():
            u = board.unit_at.get(t)
            if c[0] != 1 or u is None or not u.mine or not board.near_enemy(t, 2):
                continue
            if u.hp < u.maxhp and t in board.recover:
                add([_row(legal, board.recover[t])], 'recover garrison', 'garrison')
            if not u.moved and u.hp < 0.7 * u.maxhp and board.danger(u, t) >= 0.5 * u.value:
                outs = [(board.danger(u, b), b, k) for b, k in board.moves.get(u.tile, {}).items() if b not in board.unit_at]
                if outs:
                    dz, b, k = min(outs)
                    add([_row(legal, k), ('train_best', t)], 'rotate garrison', 'garrison')
    # 4. the unit the AI wants to act with: stay put, or retreat
    if ai_row is not None and UNIT_CMDS.get(ai_row[0]) and ai_row[1] in mine:
        u = mine[ai_row[1]]
        dest = ai_row[2] if ai_row[0] == MOVE else u.tile
        on_capital = board.city.get(u.tile, (0, 0, False, False))[0] == 1 and board.city[u.tile][3]
        if board.near_enemy(u.tile, p['near']) or board.near_enemy(dest, p['near']) or \
                (on_capital and p.get('capital_watch', 0) and board.near_enemy(u.tile, p['capital_watch'])):  # v4, not adopted
            if has_ops and ai_row[0] in (MOVE, ATTACK):
                add([('stay', u.tile)], 'stay', 'unit')
            d_ai = board.danger(u, dest)
            if ai_row[0] == MOVE and d_ai > 0.5:
                opts = [(board.danger(u, b), b, k) for b, k in board.moves.get(u.tile, {}).items()]
                if opts:
                    dz, b, k = min(opts)
                    if dz < d_ai - 0.5:
                        add([_row(legal, k)], 'retreat', 'unit')
        # advance toward the enemy capital (raiding: it is empty about half the time)
        if p.get('advance') and ai_row[0] == MOVE and u.st.atk > 0 and u.type not in (D.EXPLORER_UNIT,):
            tgt = board.enemy_capital()
            if tgt is not None and D.dist(u.tile, tgt) <= p.get('advance_range', 6):
                opts = [(D.dist(b, tgt), board.danger(u, b), b, k) for b, k in board.moves.get(u.tile, {}).items()]
                if opts:
                    dd, dz, b, k = min(opts)
                    if dd < D.dist(dest, tgt):
                        add([_row(legal, k)], 'advance', 'unit')
    return [c for g in ORDER for c in groups[g]]
