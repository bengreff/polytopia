"""Hard-coded decisions: the clear choices where the built-in AI is wasteful.

City rewards (measured: the AI on a person seat, 40 games vs Crazy, py/crazyplus docs §4.2):
- level 3: the AI takes a wall about half the time (33 of 67). A wall only pays where an enemy can
  reach the city soon; elsewhere +5 stars now. Rule: wall only if an enemy unit is within
  `wall_radius` of the city, else +5 stars.
- level 4: the AI always takes border growth (27 of 27). Population growth (+3 population, most of
  a level) is the stronger pick almost always (strategy book Part I §1.2). Rule: population growth.
- level 5+: a super unit (Giant), as the AI already does; kept as a guard.
"""
import numpy as np

import polyenv
from rulebot import data as D

REWARD = 11
F = {n: i for i, n in enumerate(polyenv.TILE_FEATURES)}


def choose_reward(env, slot, st, me, ai_choice, p=None):
    """(legal index or None to keep the AI's choice, reason)."""
    p = p or {}
    legal = st.legal
    opts = {int(r[3]): k for k, r in enumerate(legal.tolist()) if r[0] == REWARD}
    ai_reward = int(legal[ai_choice][3]) if ai_choice is not None and ai_choice >= 0 else None
    if D.R_SUPER in opts:
        return (opts[D.R_SUPER], f'giant instead of {ai_reward}') if ai_reward != D.R_SUPER else (None, '')
    if p.get('reward_rules', True) and D.R_POPGROWTH in opts and ai_reward != D.R_POPGROWTH:
        return opts[D.R_POPGROWTH], f'population growth instead of {ai_reward}'
    if p.get('reward_rules', True) and D.R_WALL in opts and D.R_RESOURCES in opts:
        tile = int(legal[opts[D.R_WALL]][1])
        threatened = _enemy_near(env, slot, me, tile, p.get('wall_radius', 3))
        want = D.R_WALL if threatened else D.R_RESOURCES
        if want != ai_reward:
            return opts[want], ('wall: enemy near' if threatened else '+5 stars: no enemy near') + f' (AI {ai_reward})'
    return None, ''


def _enemy_near(env, slot, me, tile, r):
    """Any enemy unit (seen by the enemy itself: cloaks too) within r of the tile."""
    o = env.observe([(slot, 3 - me)])[0]
    t = o.tiles.reshape(D.N, -1)
    units = np.nonzero((t[:, F['unit']] > 0) & (t[:, F['unit_owner']] == 1))[0]
    return any(D.dist(int(u), tile) <= r for u in units)
