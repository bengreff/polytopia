"""Strategy statistics z (radical-ideas.md B5, AlphaStar's z): facts of how one player played one
game, read from the game's own record. Nothing here judges a position; it only counts what the
player chose.

For each player of a recorded game (py/gen_data.py or py/convert_replays.py layout) z holds:
  - the first 6 techs researched, in order (TechType + 1; 0 = none);
  - the first 10 units trained, in order (UnitType + 1; 0 = none);
  - scalars: techs researched by turn 10 and by turn 20, cities at turn 10, the turn of the first
    port and of the first naval unit (with "had one" flags), units trained by turn 20, and the units
    trained by turn 20 per class (warrior, rider, archer, defender, catapult, swordsman, knight,
    giant, mind bender, cloak or dagger, other) plus ship upgrades by turn 20.
Turns are the game's own turn counter (0 at the start). A fact the game never reached (it ended
before turn 20) is counted up to the end.

The network input is one float vector of Z_DIM: [present, 6 tech ids, 10 unit ids, scalars].
present = 0 means "unconditioned": the network then gets no z at all (model.Net adds nothing).
"""
import numpy as np

import polyenv

G = {n: i for i, n in enumerate(polyenv.GLOBAL_FEATURES)}
T = {n: i for i, n in enumerate(polyenv.TILE_FEATURES)}

N_TECHS, N_UNITS = 6, 10
TECH_VOCAB, UNIT_VOCAB = 64, 64  # ids + 1, clamped
BUILD, TRAIN, RESEARCH, UPGRADE = 1, 5, 8, 16  # CommandType
PORT = 8  # ImprovementType.Port
# UnitData.Type values of naval units (boats, ships, the raft that carries a land unit).
NAVAL = {6, 7, 14, 39, 40, 41, 42, 43, 44, 45}
# Unit classes for the counts (UnitData.Type).
CLASSES = ['warrior', 'rider', 'archer', 'defender', 'catapult', 'swordsman', 'knight', 'giant', 'mindbender',
           'cloak', 'other']
CLASS_OF = {2: 0, 3: 1, 9: 2, 5: 3, 8: 4, 11: 5, 4: 6, 12: 7, 10: 8, 38: 9, 37: 9}
SCALARS = ['techs_t10', 'techs_t20', 'cities_t10', 'port_turn', 'has_port', 'naval_turn', 'has_naval', 'units_t20'] + \
    [f'{c}_t20' for c in CLASSES] + ['ship_upgrades_t20']
SCALE = dict(techs_t10=10, techs_t20=20, cities_t10=10, port_turn=30, has_port=1, naval_turn=30, has_naval=1,
             units_t20=20, ship_upgrades_t20=5, **{f'{c}_t20': 5 for c in CLASSES})
Z_DIM = 1 + N_TECHS + N_UNITS + len(SCALARS)


def facts(turns, rows, cities, naval):
    """The facts of one player from their decisions in order: turns [D], chosen rows [D, 4] (type, a, b,
    arg), cities [D] (their city count when deciding), naval [D] (whether they had a naval unit then).
    Returns (tech ids, unit ids, scalar facts dict) with raw (unscaled) values."""
    turns, rows = np.asarray(turns), np.asarray(rows).reshape(-1, 4)
    ty, arg = rows[:, 0], rows[:, 3]
    research = ty == RESEARCH
    train = ty == TRAIN
    techs = (arg[research][:N_TECHS] + 1).tolist()
    units = (arg[train][:N_UNITS] + 1).tolist()
    f = dict(techs_t10=int((research & (turns <= 10)).sum()), techs_t20=int((research & (turns <= 20)).sum()))
    early = turns <= 10
    cities = np.asarray(cities)
    f['cities_t10'] = int(cities[early][-1]) if early.any() else (int(cities[0]) if len(cities) else 0)
    port = np.nonzero((ty == BUILD) & (arg == PORT))[0]
    f['has_port'] = int(len(port) > 0)
    f['port_turn'] = int(turns[port[0]]) if len(port) else 0
    nav = np.nonzero(np.asarray(naval))[0]
    f['has_naval'] = int(len(nav) > 0)
    f['naval_turn'] = int(turns[nav[0]]) if len(nav) else 0
    t20 = turns <= 20
    trained = arg[train & t20]
    f['units_t20'] = int(len(trained))
    for c in CLASSES:
        f[f'{c}_t20'] = 0
    for u in trained.tolist():
        f[f'{CLASSES[CLASS_OF.get(int(u), len(CLASSES) - 1)]}_t20'] += 1
    f['ship_upgrades_t20'] = int(((ty == UPGRADE) & t20 & np.isin(arg, list(NAVAL))).sum())
    return techs, units, f


def vector(techs, units, f):
    """The network's z input (float32 [Z_DIM]) from facts()."""
    z = np.zeros(Z_DIM, np.float32)
    z[0] = 1.0
    t = np.minimum(np.asarray(techs[:N_TECHS], np.int64), TECH_VOCAB - 1)
    u = np.minimum(np.asarray(units[:N_UNITS], np.int64), UNIT_VOCAB - 1)
    z[1:1 + len(t)] = t
    z[1 + N_TECHS:1 + N_TECHS + len(u)] = u
    z[1 + N_TECHS + N_UNITS:] = [f[n] / SCALE[n] for n in SCALARS]
    return z


def naval_flags(tiles):
    """Per decision: does the player to move have a naval unit (tiles int16 [D, 196, F], the mover's view)?"""
    u = tiles[..., T['unit']].astype(np.int64) - 1  # the observation stores UnitType + 1
    mine = tiles[..., T['unit_owner']] == 1
    return (mine & np.isin(u, list(NAVAL))).any(-1)


def game_facts(z):
    """{player: (techs, units, facts)} for a recorded game (an npz or a dict with its arrays). Uses the
    recorded label of each decision (rows outside the legal list are not recorded, so not counted)."""
    label = np.asarray(z['label']).astype(np.int64)
    off = np.asarray(z['legal_offsets'])
    legal = np.asarray(z['legal'])
    player = np.asarray(z['player']).astype(np.int64)
    glob = np.asarray(z['globals'])
    ok = label >= 0
    rows = np.zeros((len(label), 4), np.int64)
    rows[ok] = legal[off[:-1][ok] + label[ok]]
    rows[~ok] = -1
    naval = naval_flags(np.asarray(z['tiles']))
    out = {}
    for p in sorted(set(player.tolist())):
        m = (player == p) & ok
        out[p] = facts(glob[m, G['turn']], rows[m], glob[m, G['cities']], naval[m])
    return out


def game_z(z):
    """z per decision (float32 [D, Z_DIM]): each decision gets its own player's z for the whole game."""
    player = np.asarray(z['player']).astype(np.int64)
    out = np.zeros((len(player), Z_DIM), np.float32)
    for p, fx in game_facts(z).items():
        out[player == p] = vector(*fx)
    return out


def scalars(zv):
    """The named scalar facts (unscaled) of a z vector."""
    s = zv[1 + N_TECHS + N_UNITS:]
    return {n: float(v) * SCALE[n] for n, v in zip(SCALARS, s)}
