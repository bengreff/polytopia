"""Attack planning: enumerate this turn's attacks (from where units stand, or after one move),
predict each with the verified damage formula, and pick kill sequences (soften with attacks that
draw no retaliation, finish with the rest) and good trades. Also the danger map: how much damage
the visible enemy units could deal to a unit on a tile next turn."""
import itertools

from . import data as D
from .combat import battle, defence_bonus


def reach(u):
    s = u.st
    return (s.mov if 'dash' in s.abil else 0) + s.rng


class Danger:
    """Enemy threats per tile, from the visible enemy units (the fog may hide more)."""

    def __init__(self, view, my_techs):
        self.v = view
        self.my_techs = my_techs
        self.threats = {}  # tile -> [enemy Unit]
        for e in view.enemy_units:
            if e.st.atk <= 0:
                continue
            r = reach(e)
            naval_e = e.naval
            for t in D.within(e.tile, r):
                if naval_e and not self._near_water(t, e.st.rng):
                    continue
                if not naval_e and r <= e.st.rng and False:
                    continue
                self.threats.setdefault(t, []).append(e)
        self._cache = {}

    def _near_water(self, t, rng):
        v = self.v
        if v.terrain[t] in (D.WATER, D.OCEAN):
            return True
        return any(v.terrain[j] in (D.WATER, D.OCEAN) for j in D.within(t, rng))

    def damage(self, tile, utype, hp, maxhp, exclude=()):
        """Total damage the threats on `tile` could deal to such a unit (attacks in sequence)."""
        key = (tile, utype, hp, maxhp, tuple(sorted(exclude)))
        r = self._cache.get(key)
        if r is not None:
            return r
        es = [e for e in self.threats.get(tile, ()) if e.tile not in exclude]
        bonus = defence_bonus(self.v, tile, utype, 1, self.my_techs)
        total, h = 0, hp
        # strongest first
        es.sort(key=lambda e: -e.st.atk * e.hp / e.maxhp)
        for e in es:
            if h <= 0:
                break
            dmg, _ = battle(e.type, e.hp, e.maxhp, utype, h, maxhp, bonus, 1 if e.st.rng == 1 else 2)
            total += dmg
            h -= dmg
        self._cache[key] = (total, len(es))
        return total, len(es)

    def kills(self, tile, utype, hp, maxhp, exclude=()):
        dmg, n = self.damage(tile, utype, hp, maxhp, exclude)
        return dmg >= hp

    def count(self, tile):
        return len(self.threats.get(tile, ()))


class Option:
    __slots__ = ('unit', 'tile', 'move_row', 'target')

    def __init__(self, unit, tile, move_row, target):
        self.unit, self.tile, self.move_row, self.target = unit, tile, move_row, target


def on_my_city(v, e):
    c = v.city_at.get(e.tile)
    return c if (c is not None and c.owner == 1) else None


def enemy_value(bot, v, e):
    val = D.unit_value(e.type)
    if e.naval and e.passenger >= 0:
        val += 0.6 * D.unit_value(e.passenger)
    mine = on_my_city(v, e)
    if mine is not None:
        val += 100 if mine.capital else 12 + 2 * mine.level
    else:
        for c in v.my_cities:
            d = D.dist(c.tile, e.tile)
            if d == 1:
                val += 8 if c.capital else 2.5
                break
            if d == 2 and c.capital:
                val += 3
                break
    ec = v.city_at.get(e.tile)
    if ec is not None and ec.owner == 2 and ec.capital:
        val += bot.p.get('capital_kill_bonus', 12)  # opens the enemy capital
    if e.type in (D.CATAPULT, D.KNIGHT, D.BOMBER):
        val += 2
    if e.type == D.GIANT:
        val += 4
    return val


def my_value(u):
    val = D.unit_value(u.type)
    if u.naval and u.passenger >= 0:
        val = max(val, D.unit_value(u.passenger)) + 1
    return val


def options(bot, v):
    """Every (my unit, attack tile, move row, target) this turn: direct attacks from the legal list,
    and attacks after one legal move for units that may move then attack."""
    out = []
    enemies = {e.tile: e for e in v.enemy_units}
    for u in v.my_units:
        if u.st.atk <= 0 or u.tile in bot.turn_hold_attack:
            continue
        for b, row in v.attacks.get(u.tile, {}).items():
            e = enemies.get(b)
            if e is not None:
                out.append(Option(u, u.tile, None, e))
        if not u.moved and 'dash' in u.st.abil and u.tile in v.moves and not u.attacked:
            rng = u.st.rng
            for d, row in v.moves[u.tile].items():
                # boats attack from water; land units from land. A move onto a port embarks (no attack).
                if not u.naval and (v.terrain[d] in (D.WATER, D.OCEAN)):
                    continue
                if u.naval and v.terrain[d] not in (D.WATER, D.OCEAN):
                    continue
                for e in v.enemy_units:
                    if D.dist(d, e.tile) <= rng:
                        out.append(Option(u, d, row, e))
    return out


class Plan:
    __slots__ = ('score', 'steps', 'target', 'kill', 'why')

    def __init__(self, score, steps, target, kill, why):
        self.score, self.steps, self.target, self.kill, self.why = score, steps, target, kill, why


def plan_attacks(bot, v, danger):
    """Best attack sequence (or None). steps: [(Option, dmg, ret)] in order."""
    opts = options(bot, v)
    if not opts:
        return None
    et = bot.enemy_techs(v)
    my_t = v.techs
    by_target = {}
    for o in opts:
        by_target.setdefault(o.target.tile, []).append(o)
    best = None
    for tt, os_ in by_target.items():
        e = os_[0].target
        ev = enemy_value(bot, v, e)
        bonus_e = defence_bonus(v, e.tile, e.type, 2, et)
        # one option per unit for this target: the attack tile with least retaliation, then least danger
        per_unit = {}
        for o in os_:
            u = o.unit
            dmg, ret = battle(u.type, u.hp, u.maxhp, e.type, e.hp, e.maxhp, bonus_e, D.dist(o.tile, e.tile))
            exp_after = _exposure(bot, v, danger, u, o.tile, u.hp - ret, e)
            key = (ret, exp_after, 0 if o.move_row is None else 1)
            cur = per_unit.get(u.tile)
            if cur is None or key < cur[0]:
                per_unit[u.tile] = (key, o)
        cands = [o for _, o in per_unit.values()]
        # limit the search: the 6 most damaging
        cands.sort(key=lambda o: -battle(o.unit.type, o.unit.hp, o.unit.maxhp, e.type, e.hp, e.maxhp, bonus_e,
                                          D.dist(o.tile, e.tile))[0])
        emergency = on_my_city(v, e) is not None
        cands = cands[:7 if emergency else 6]

        def order_key(o):
            u = o.unit
            _, ret0 = battle(u.type, u.hp, u.maxhp, e.type, e.hp, e.maxhp, bonus_e, D.dist(o.tile, e.tile))
            return (0 if ret0 == 0 else 1, my_value(u))  # free hits first; cheap units soften, strong finish

        cands.sort(key=order_key)
        for k in ((1, 2, 3, 4, 5) if emergency else (1, 2, 3)):
            if k > len(cands):
                break
            for seq in itertools.combinations(cands, k):  # kept in order_key order
                p = _simulate(bot, v, danger, e, ev, bonus_e, seq)
                if p is not None and (best is None or p.score > best.score):
                    best = p
    return best


def _exposure(bot, v, danger, u, tile, hp_after, target, target_dies=False):
    """Expected star loss from leaving unit u on `tile` with hp_after through the enemy's turn."""
    if hp_after <= 0:
        return 0.0
    excl = (target.tile,) if target_dies else ()
    dmg, n = danger.damage(tile, u.type, hp_after, u.maxhp, exclude=excl)
    if n == 0:
        return 0.0
    k = bot.p.get('exposure_scale', 1.0)
    if dmg >= hp_after:
        return 0.75 * my_value(u) * k
    return 0.4 * my_value(u) * dmg / u.maxhp * k


def _simulate(bot, v, danger, e, ev, bonus_e, seq):
    hp = e.hp
    steps = []
    cost = 0.0
    kill = False
    for i, o in enumerate(seq):
        if hp <= 0:
            return None  # a later attacker would be wasted: shorter sequence covers it
        u = o.unit
        d = D.dist(o.tile, e.tile)
        dmg, ret = battle(u.type, u.hp, u.maxhp, e.type, hp, e.maxhp, bonus_e, d)
        hp -= dmg
        dies_now = hp <= 0
        if dies_now:
            ret = 0
        uv = my_value(u)
        if ret >= u.hp:
            cost += uv
        else:
            cost += uv * ret / u.maxhp * 0.9
            # where the unit ends: melee killers advance onto the target's tile
            end_tile = e.tile if (dies_now and u.st.rng == 1 and not u.naval and v.land(e.tile)) else o.tile
            cost += _exposure(bot, v, danger, u, end_tile, u.hp - ret, e, target_dies=dies_now)
            # leaving a city we garrison open
            if bot.is_garrison(v, u) and (end_tile != u.tile or o.tile != u.tile):
                cost += bot.garrison_cost(v, u)
        steps.append((o, dmg, ret))
        if dies_now:
            kill = True
            break
    if kill:
        gain = ev + 1.5
    elif on_my_city(v, e) is not None:
        gain = D.unit_value(e.type) * (e.hp - hp) / e.hp * 0.55  # only a kill stops the capture
    else:
        frac = (e.hp - hp) / e.hp
        gain = ev * frac * bot.p.get('hit_gain', 0.55)
    score = gain - cost - 0.25 * len(steps)
    if not kill and len(steps) > 1:
        score -= 1.0  # multi-unit non-kills: rarely worth committing several units
    return Plan(score, steps, e, kill, f'{"kill" if kill else "hit"} {e} gain {gain:.1f} cost {cost:.1f}')
