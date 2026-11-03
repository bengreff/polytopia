"""The hand-written evaluation of a position, in stars, from one player's side.

The bot keeps a full view: a position is read from two observations of the same (cloned) state,
one from each player, so every unit (cloaks included), city, star and tech is known.

    value = material + cities + economy + occupation, mine minus the opponent's

- material: each unit's star value x (0.3 + 0.7 x health share), +1.5 for a veteran; a boat counts
  its passenger plus its hull;
- cities: city_base + city_level x level (+ capital bonus, + wall);
- economy: income x w_income, stars x w_stars, techs x w_tech;
- occupation (the position is read at the start of a player's turn): an enemy unit standing on one
  of my cities is a capture threat; my unit standing on an enemy city captures it this turn; an empty
  capital with an enemy unit within `open_radius` (capital_open, and capital_open_enemy for theirs).
A finished game is +-WIN.
"""
import numpy as np

import polyenv
from rulebot import data as D

F = {n: i for i, n in enumerate(polyenv.TILE_FEATURES)}
G = {n: i for i, n in enumerate(polyenv.GLOBAL_FEATURES)}
WIN = 10000.0

# Star value of a unit by type (data units: type, not type + 1).
UNIT_VALUE = np.zeros(80)
for _t, _s in D.UNITS.items():
    if _t < 80:
        UNIT_VALUE[_t] = max(_s.cost, 2)
UNIT_VALUE[D.WARRIOR] = 2.5
UNIT_VALUE[D.GIANT] = 15
UNIT_VALUE[D.EXPLORER_UNIT] = 0.5
UNIT_VALUE[D.KNIGHT] = 7.5
UNIT_VALUE[D.CATAPULT] = 7
UNIT_VALUE[D.CLOAK] = 6
UNIT_VALUE[D.DAGGER] = 2.5
# hulls: value added to the passenger
HULL_VALUE = np.zeros(80)
HULL_VALUE[D.RAFT] = 0
HULL_VALUE[D.SCOUTSHIP] = 4
HULL_VALUE[D.RAMMER] = 4
HULL_VALUE[D.BOMBER] = 12
HULL_VALUE[D.JUGGERNAUT] = 0  # the giant's own value is the passenger's
NAVAL = np.zeros(80, bool)
NAVAL[list(D.NAVAL)] = True

DEFAULT_W = dict(city_base=8.0, city_level=3.0, capital=12.0, wall=2.0, income=3.0, stars=0.6, tech=1.5,
                 occupied_mine=0.6, occupied_capital=150.0, occupying=0.8, occupying_capital=200.0,
                 hp_floor=0.3, veteran=1.5, press=0.0, press_range=6,
                 capital_open=0.0, capital_open_enemy=0.0, open_radius=2)  # tried (v4): not adopted


class Side:
    """One player's own pieces, read from that player's observation (unit_owner/owner == 1)."""
    __slots__ = ('unit_tiles', 'unit_types', 'unit_vals', 'city_tiles', 'city_vals', 'capital', 'income', 'stars',
                 'techs', 'all_units_tile')

    def __init__(self, obs, w):
        t = obs.tiles.reshape(D.N, -1)
        own = (t[:, F['unit']] > 0) & (t[:, F['unit_owner']] == 1)
        tiles = np.nonzero(own)[0]
        typ = t[tiles, F['unit']].astype(int) - 1
        pas = t[tiles, F['unit_passenger']].astype(int) - 1
        hp = t[tiles, F['unit_health']].astype(float)
        mx = np.maximum(1, t[tiles, F['unit_max_health']]).astype(float)
        base = np.where(NAVAL[typ], UNIT_VALUE[np.maximum(pas, 0)] * (pas >= 0) + HULL_VALUE[typ], UNIT_VALUE[typ])
        vals = base * (w['hp_floor'] + (1 - w['hp_floor']) * np.clip(hp / mx, 0, 1))
        vals += w['veteran'] * (t[tiles, F['unit_promoted']] > 0)
        self.unit_tiles, self.unit_types, self.unit_vals = tiles, typ, vals
        cities = np.nonzero((t[:, F['improvement']] == D.CITY) & (t[:, F['owner']] == 1))[0]
        lvl = t[cities, F['imp_level']].astype(float)
        cap = t[cities, F['capital_of']] != 0
        wall = (t[cities, F['imp_rewards']] & (1 << D.R_WALL)) != 0
        self.city_tiles = cities
        self.city_vals = w['city_base'] + w['city_level'] * lvl + w['capital'] * cap + w['wall'] * wall
        self.capital = int(cities[cap][0]) if cap.any() else -1
        g = obs.globals
        self.income, self.stars = float(g[G['income']]), float(g[G['stars']])
        self.techs = float(obs.techs.sum())


def evaluate(obs_me, obs_op, st, me, w=None, detail=False):
    """Value of the position for player `me` (stars). obs_me / obs_op: observations of the same state by
    `me` and by the opponent. st: its Status."""
    w = w or DEFAULT_W
    if st.ended:
        return WIN if st.winner == me else -WIN
    a, b = Side(obs_me, w), Side(obs_op, w)
    mat = a.unit_vals.sum() - b.unit_vals.sum()
    cities = a.city_vals.sum() - b.city_vals.sum()
    econ = w['income'] * (a.income - b.income) + w['stars'] * (a.stars - b.stars) + w['tech'] * (a.techs - b.techs)
    occ = 0.0
    # whose turn starts: the occupant of the other side's city captures it on its own turn
    mine_turn = st.current == me
    b_on = set(b.unit_tiles.tolist())
    a_on = set(a.unit_tiles.tolist())
    for tile, v in zip(a.city_tiles.tolist(), a.city_vals.tolist()):
        if tile in b_on:
            occ -= (w['occupied_capital'] if tile == a.capital else w['occupied_mine'] * v) * (0.6 if mine_turn else 1.0)
    for tile, v in zip(b.city_tiles.tolist(), b.city_vals.tolist()):
        if tile in a_on:
            occ += (w['occupying_capital'] if tile == b.capital else w['occupying'] * v) * (1.0 if mine_turn else 0.6)
    # an empty capital with an enemy unit close by (the capture comes two turns later, beyond one round)
    if w['capital_open'] and a.capital >= 0 and a.capital not in a_on and _near(b, a.capital, w['open_radius']):
        occ -= w['capital_open']
    if w['capital_open_enemy'] and b.capital >= 0 and b.capital not in b_on and _near(a, b.capital, w['open_radius']):
        occ += w['capital_open_enemy']
    press = 0.0
    if w['press']:  # military units near the other side's capital (both sides)
        R = w['press_range']
        press = w['press'] * (_press(a, b.capital, R) - _press(b, a.capital, R))
    total = mat + cities + econ + occ + press
    if detail:
        return total, dict(mat=round(mat, 1), cities=round(cities, 1), econ=round(econ, 1), occ=round(occ, 1),
                           press=round(press, 1))
    return total


ATTACKERS = np.zeros(80, bool)
for _t, _s in D.UNITS.items():
    if _t < 80 and _s.atk > 0 and _t != D.EXPLORER_UNIT:
        ATTACKERS[_t] = True
for _t in D.NAVAL:
    ATTACKERS[_t] = False


def _near(side, tile, r):
    t = side.unit_tiles
    if not len(t):
        return False
    return bool((np.maximum(np.abs(t % D.W - tile % D.W), np.abs(t // D.W - tile // D.W)) <= r).any())


def _press(side, capital, R):
    if capital < 0 or not len(side.unit_tiles):
        return 0.0
    t = side.unit_tiles
    d = np.maximum(np.abs(t % D.W - capital % D.W), np.abs(t // D.W - capital // D.W))
    return float((np.clip(R - d, 0, None) * ATTACKERS[side.unit_types]).sum() / R)


def evaluate_slot(env, slot, st, me, w=None, detail=False):
    obs_me, obs_op = env.observe([(slot, me), (slot, 3 - me)])
    return evaluate(obs_me, obs_op, st, me, w, detail)
