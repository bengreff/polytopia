"""Game constants for the rule bot, read from the game's own data file (GameLogicData28.txt).

Stats are kept in the data file's units: health 100 = a Warrior's 10 HP, attack 20 = attack 2.
Observation unit types are stored +1 (0 = no unit); `unit_type()` undoes that.
"""
import json
import os

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
_D = json.load(open(os.path.join(REPO, 'game', 'dump', 'gamedata', 'GameLogicData28.txt')))

W = H = 14
N = W * H

# Command types (polyenv.COMMAND_TYPES)
BUILD, ATTACK, RECOVER, HEAL, TRAIN, MOVE, CAPTURE, RESEARCH, DESTROY, DISBAND, REWARD = 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
PROMOTE, RUINS, ENDTURN, UPGRADE, STAY, PEACE, PEACE_RESP, BREAK_PEACE, EMBASSY, DISEMBARK = 13, 14, 15, 16, 21, 27, 28, 29, 30, 36

# Terrain
WATER, OCEAN, FIELD, MOUNTAIN, FOREST = 1, 2, 3, 4, 5
# Resources
GAME, CROP, FISH, WHALE, METAL, FRUIT, STARFISH = 1, 2, 3, 4, 5, 6, 8
# Improvements
CITY, RUIN, ROAD, FARM, WINDMILL, FISHING_HUT, PORT, HUNTING_HUT, CLEARFOREST, BURNFOREST, LUMBERHUT, SAWMILL = \
    1, 2, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13
GROWFOREST, HARVESTFRUIT, TEMPLE, FORESTTEMPLE, WATERTEMPLE, MOUNTAINTEMPLE, MINE, FORGE = 14, 15, 17, 18, 19, 20, 21, 22
STARFISHING, BRIDGE, MARKET = 46, 48, 50
# City rewards (CityReward enum)
R_WALL, R_PARK, R_WORKSHOP, R_EXPLORER, R_BORDER, R_SUPER, R_RESOURCES, R_POPGROWTH = 1, 2, 3, 4, 5, 6, 7, 8

UNIT_IDX = {n: v['idx'] for n, v in _D['unitData'].items()}
UNIT_NAME = {v: n for n, v in UNIT_IDX.items()}
TECH_IDX = {n: v['idx'] for n, v in _D['techData'].items()}
TECH_NAME = {v: n for n, v in TECH_IDX.items()}
IMP_IDX = {n: v['idx'] for n, v in _D['improvementData'].items()}
IMP_NAME = {v: n for n, v in IMP_IDX.items()}

WARRIOR, RIDER, KNIGHT, DEFENDER, CATAPULT, ARCHER, MINDBENDER, SWORDSMAN, GIANT = \
    (UNIT_IDX[n] for n in ('warrior', 'rider', 'knight', 'defender', 'catapult', 'archer', 'mindbender', 'swordsman', 'giant'))
CLOAK, DAGGER, POLYTAUR, EXPLORER_UNIT = UNIT_IDX['cloak'], UNIT_IDX['dagger'], UNIT_IDX['polytaur'], UNIT_IDX['scout']
RAFT, SCOUTSHIP, RAMMER, BOMBER, JUGGERNAUT = (UNIT_IDX[n] for n in ('transportship', 'scoutship', 'rammership', 'bombership', 'juggernaut'))
NAVAL = {RAFT, SCOUTSHIP, RAMMER, BOMBER, JUGGERNAUT, UNIT_IDX['cloak_boat'], UNIT_IDX['pirate']}
RANGED = {ARCHER, CATAPULT}


class UnitStats:
    __slots__ = ('type', 'name', 'hp', 'atk', 'df', 'mov', 'rng', 'cost', 'abil')

    def __init__(self, t, v):
        self.type, self.name = t, UNIT_NAME[t]
        self.hp, self.atk, self.df = v.get('health', 100), v.get('attack', 0), v.get('defence', 0)
        self.mov, self.rng, self.cost = v.get('movement', 1), v.get('range', 1), v.get('cost', 0)
        self.abil = set(v.get('unitAbilities', []))


UNITS = {v['idx']: UnitStats(v['idx'], v) for v in _D['unitData'].values()}


def stats(t):
    return UNITS.get(t) or UNITS[WARRIOR]


# Star value of units, for trade-offs (cost, with a floor for free units).
def unit_value(t):
    s = stats(t)
    if t == GIANT:
        return 14
    if t in (BOMBER,):
        return 15
    if t in (SCOUTSHIP, RAMMER):
        return 7
    if t == RAFT:
        return 3
    if t == JUGGERNAUT:
        return 12
    if t == EXPLORER_UNIT:
        return 0.5
    return max(s.cost, 2)


# Tech tree
TECH_TIER = {v['idx']: v.get('cost', 0) for v in _D['techData'].values()}
TECH_PARENT = {}
for _n, _v in _D['techData'].items():
    for _c in _v.get('techUnlocks', []):
        TECH_PARENT[TECH_IDX[_c]] = _v['idx']
TECH_UNITS = {v['idx']: [UNIT_IDX[u] for u in v.get('unitUnlocks', []) if u in UNIT_IDX] for v in _D['techData'].values()}
TECH_IMPS = {v['idx']: [IMP_IDX[u] for u in v.get('improvementUnlocks', []) if u in IMP_IDX] for v in _D['techData'].values()}
T = TECH_IDX  # shorthand: T['riding']

# Which tech unlocks a unit / improvement (for the regular tree)
UNIT_TECH = {}
IMP_TECH = {}
for _tid, _us in TECH_UNITS.items():
    for _u in _us:
        UNIT_TECH.setdefault(_u, _tid)
for _tid, _is in TECH_IMPS.items():
    for _i in _is:
        IMP_TECH.setdefault(_i, _tid)

IMP_COST = {v['idx']: v.get('cost', 0) for v in _D['improvementData'].values()}

TRIBE = {'aimo': 2, 'bardur': 4, 'hoodrick': 6, 'imperius': 7, 'kickoo': 8, 'luxidoor': 9, 'oumaji': 10,
         'quetzali': 11, 'vengir': 12, 'xinxi': 13, 'yadakk': 14, 'zebasi': 15}
TRIBE_NAME = {v: k for k, v in TRIBE.items()}
TRIBE_START_TECH = {}
for _n, _v in _D['tribeData'].items():
    if _n in TRIBE:
        TRIBE_START_TECH[TRIBE[_n]] = {TECH_IDX[t] for t in _v.get('startingTech', [])}


def tech_cost(tier, cities, literacy=False):
    c = tier * cities + 4
    if literacy:
        c = -(-c * 2 // 3)  # 33% off, rounded up (community-sourced; checked by the bot's star log)
    return c


def xy(i):
    return i % W, i // W


def idx(x, y):
    return y * W + x


def dist(a, b):
    return max(abs(a % W - b % W), abs(a // W - b // W))


NEIGH = []
for _i in range(N):
    _x, _y = _i % W, _i // W
    NEIGH.append([(_y + dy) * W + _x + dx for dy in (-1, 0, 1) for dx in (-1, 0, 1)
                  if (dx or dy) and 0 <= _x + dx < W and 0 <= _y + dy < H])


def within(i, r):
    """Tiles at Chebyshev distance 1..r from i."""
    x, y = i % W, i // W
    return [(yy * W + xx) for yy in range(max(0, y - r), min(H, y + r + 1)) for xx in range(max(0, x - r), min(W, x + r + 1))
            if (xx, yy) != (x, y)]


# Where the enemy capital is, given ours: capital pairs from 4,000 fresh championship maps
# (seeds 21,000,000-21,003,999, outside every evaluation range; py/rulebot/capital_pairs.npy).
# A prior over map layouts, as an experienced player knows them; the bot multiplies it by
# "not yet seen" to steer its scouts.
_PAIRS = None


def enemy_capital_prior(my_cap):
    """Array [N]: prior probability that the enemy capital is on each tile."""
    global _PAIRS
    import numpy as np
    if _PAIRS is None:
        p = np.load(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'capital_pairs.npy')).astype(int)
        _PAIRS = np.concatenate([p, p[:, ::-1]])
    mx, my = my_cap % W, my_cap // W
    a, b = _PAIRS[:, 0], _PAIRS[:, 1]
    ax, ay, bx, by = a % W, a // W, b % W, b // W
    prior = np.zeros(N)
    for r in (0, 1, 2):
        sel = (np.abs(ax - mx) <= r) & (np.abs(ay - my) <= r)
        if sel.sum() >= 40 or r == 2:
            ex = np.clip(bx[sel] + (mx - ax[sel]), 0, W - 1)
            ey = np.clip(by[sel] + (my - ay[sel]), 0, H - 1)
            np.add.at(prior, ey * W + ex, 1.0)
            break
    if prior.sum() == 0:
        prior[:] = 1.0
    return prior / prior.sum()
