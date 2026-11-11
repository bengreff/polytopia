"""Planner v2 in the loop (research log S11, track s11x): py/tactics/planner2.py, the planner Ben plays
against, run inside py/selfplay.py's training games (--search_v2), through the shard's batched inference.

It replaces py/search_loop.py's planner on the searched turns. At the start of a searched turn the real
game (slot src) is saved and loaded into this thread group's own copy (slot off); planner v2 then plans
there exactly as in play (fog-safe samples for the learner's view, network lanes, tactical lanes, the
opponent's greedy and tactical replies, defensive repairs, the fog critic's scores) and returns the
commands of the lane it would play, End Turn included. Unlike search_loop's draw among sampled lanes,
v2 plays its best lane (its own rule, as measured in the gate): the training target is the planner's
choice. Every slot the planner uses is shifted by off, so the two thread groups of a game process keep
apart (off = OFFSET * (group + 1); planner v2 uses off ... off + 560).

Nothing here sees what the learner can't: planner v2 makes and scores its lanes on samples built from
the learner's own view; the real game's copy is only used to check and finish the chosen lane, as in
play.
"""
from search_loop import INFO as INFO_V1, find, pick  # noqa: F401 (find and pick are used through this module)
from tactics import planner2

INFO = INFO_V1 + ('kind',)
KINDS = {'net': 0, 'tac': 1, 'rep': 2}
OFFSET = 1000
SLOT_STRIDE = 10 ** 9  # no limit on plans x samples (planner v2 has 64 lane slots per sample)


class Agent:
    """What planner v2 reads from py/plan_agent.Planner, on top of selfplay's Inference: the learner
    ('A') makes the lanes and the replies, the fog critic ('C') scores them."""

    def __init__(self, env, infer, plans, samples, temperature, rng):
        self.env, self.q = env, infer
        self.K, self.D, self.temperature, self.rng = plans, samples, temperature, rng
        self.net = self.model = 'A'
        self.scorer = 'C'

    def infer(self, key, slots, sts, obs):
        """(logits over each position's legal rows, padded; win probability for the player viewed)."""
        logits, _, value = self.q.request(key, obs, [sts[s] for s in slots], None)
        return logits, value

    @staticmethod
    def key(row):
        return tuple(int(x) for x in row)


def plan_turn(env, infer, src, spec, opt, off, plans, samples, temperature, rng):
    """Plans the current player's turn in slot src. Returns (commands, info with INFO's fields)."""
    played, v = planner2.plan_turn(Agent(env, infer, plans, samples, temperature, rng), spec, env.save(src), opt, off)
    info = dict(top=v['top_move_score'], chosen=v['best_score'], best=v['best_score'], mean=v['mean_score'], kl=0.0,
                commands=len(played), broken=int(v['broken']), seconds=v['seconds'], lane=v['best'], spread=v['spread'],
                noise=0.0, kind=KINDS[v['kind']], tac_lanes=v['tac_lanes'], repair_lanes=v['repair_lanes'])
    return played, info
