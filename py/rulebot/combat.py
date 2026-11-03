"""Combat arithmetic: the strategy book's verified damage formula (Part II §1.1), defence bonuses
(§1.2) and retaliation rules (§1.4). Health and stats are in the data file's units (Warrior 100 HP,
attack 20). Damage rounds to whole HP (10 data units), halves up."""
from . import data as D


def round10(x):
    return int(x / 10 + 0.5 + 1e-9) * 10


def defence_bonus(view, tile, utype, owner_rel, techs_of_owner):
    """Multiplier for a unit of type `utype` owned by `owner_rel` (1 me, 2 enemy) standing on `tile`.
    techs_of_owner: the techs we know or assume that player has."""
    st = D.stats(utype)
    c = view.city_at.get(tile)
    if c is not None and c.owner == owner_rel and 'fortify' in st.abil:
        return 4.0 if c.wall else 1.5
    ter = view.terrain[tile]
    if ter == D.FOREST and D.T['archery'] in techs_of_owner and utype not in D.NAVAL:
        return 1.5
    if ter == D.MOUNTAIN and D.T['climbing'] in techs_of_owner and utype not in D.NAVAL:
        return 1.5
    if ter in (D.WATER, D.OCEAN) and utype in D.NAVAL and D.T['aquatism'] in techs_of_owner:
        return 1.5
    return 1.0


def attack_stat(utype):
    return D.stats(utype).atk


def defence_stat(utype):
    return D.stats(utype).df


def battle(atype, ahp, amax, dtype, dhp, dmax, bonus, dist, a_naval_type=None, d_naval_type=None):
    """(damage to defender, retaliation to attacker) in data HP units. a_naval_type / d_naval_type:
    the hull type when the unit is a boat (boats fight with the hull's stats)."""
    at = a_naval_type if a_naval_type is not None else atype
    dt = d_naval_type if d_naval_type is not None else dtype
    sa, sd = D.stats(at), D.stats(dt)
    if sa.atk <= 0:
        return 0, 0
    aF = sa.atk * ahp / amax
    dF = sd.df * dhp / dmax * bonus
    tot = aF + dF
    if tot <= 0:
        return dhp, 0
    dmg = round10(aF / tot * sa.atk * 4.5)
    if dmg >= dhp:
        return dmg, 0
    if 'stiff' in sd.abil or 'surprise' in sa.abil:
        return dmg, 0
    if dist > sd.rng:
        return dmg, 0
    ret = round10(dF / tot * sd.df * 4.5)
    return dmg, ret


def unit_battle(view, att, att_tile, dfn, enemy_techs, my_techs, attacker_is_me=True):
    """Battle of Unit `att` attacking from `att_tile` at Unit `dfn` (both state.Unit)."""
    if attacker_is_me:
        bonus = defence_bonus(view, dfn.tile, dfn.type, dfn.owner, enemy_techs)
    else:
        bonus = defence_bonus(view, dfn.tile, dfn.type, dfn.owner, my_techs)
    return battle(att.type, att.hp, att.maxhp, dfn.type, dfn.hp, dfn.maxhp, bonus, D.dist(att_tile, dfn.tile))
