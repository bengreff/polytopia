"""Planner v2's hand-coded tactics (Ben, 2026-11-04): kill chains on offence and defence.

- search.py: a beam search over attack sequences for the side to move, every step simulated by the
  game's own rules on a copy of the (fog-safe, determinized) sample;
- planner2.py: the turn planner with tactical lanes, the opponent's tactical reply and defensive
  repairs (plan_agent.Planner(tactical=...));
- match.py: planner against planner (v2 vs v1), or against a network or CrazyPlus;
- suite.py: the tactical test positions and the v1 vs v2 measurements.
"""
