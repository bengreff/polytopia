"""CrazyPlus: the game's own AI, improved by exact lookahead and a few hard-coded decisions
(docs/design/crazyplus.md).

Each decision:
1. A committed winning line (below) is replayed while it still wins.
2. Ask the built-in AI for its command c* (env.bot on the real game).
3. City rewards: `rules.choose_reward` (checked by rollout against the AI's pick).
4. Once a turn, the win check: the AI's own play is rolled out `win_depth` rounds; a won game is committed.
5. Candidates (`tactics.candidates`; in the opening also `econ.candidates`): kills, multi-unit kill plans,
   captures, walking into empty cities, garrisons, the AI's unit staying put or retreating, attacks; every
   research, training and useful build up to turn `econ_turns`.
6. Each candidate and c* are scored exactly: on a clone of the full state the candidate is played, the AI
   finishes our turn, the opponent model (the game's own Crazy AI) plays its whole reply, `depth` rounds in
   all (`econ_horizon` for economic decisions), and `evalfn` scores the position.
7. The best candidate is played if it beats c* by `margin` stars and, for a tactical one, still does so at
   `verify_depth` rounds; else c*. A rollout that wins the game is played and its whole line committed.
"""
import time

import numpy as np

import polyenv
from rulebot import data as D
from crazyplus import econ, evalfn, sim, tactics, rules

DEFAULTS = dict(
    mode='search',        # 'ai': the plain built-in AI (the baseline); 'search': CrazyPlus
    model=3,              # the opponent model in rollouts: the game's AI at this difficulty
    depth=1,              # rounds simulated after the candidate: 1 = finish our turn + the reply
    margin=1.0,           # stars an alternative must gain over the AI's command
    max_cands=10,
    near=3,               # a unit is in contact when an enemy unit is within this distance
    rewards=True,         # hard-coded city-reward choices (rules.choose_reward)
    reward_rules=True,    # ... beyond the giant: population growth at level 4, wall only when threatened at 3
    wall_radius=3,
    reward_check=True,    # a rule's reward only if its rollout (econ_horizon rounds) is within reward_margin of the AI's
    reward_margin=3.0,
    budget_ms=1500,       # stop trying candidates once a decision's rollouts took this long
    weights=None,         # evalfn weight overrides
    verify_depth=2,       # re-check a proposed tactical override at this depth (both it and the AI's command)
    econ_turns=12,        # opening economy search up to this turn (0: off)
    econ_horizon=4,       # rounds simulated for an economic alternative
    econ_max=10,          # alternatives tried per economic decision
    econ_margin=2.0,
    win_depth=3,          # once a turn: rounds of the AI's own play checked for a forced win (0: off)
    skip_below=5,         # skip a candidate rejected earlier this turn by more than this many stars
)


class CrazyPlus:
    def __init__(self, env, slot, spec, me, params=None, log=False):
        self.p = dict(DEFAULTS, **(params or {}))
        self.env, self.slot, self.me = env, slot, me
        self.spec = spec
        bots = [None, None]
        bots[2 - me] = self.p['model']
        self.sim_spec = polyenv.Spec(tribes=spec.tribes, bots=tuple(bots), mode=spec.mode, map_preset=spec.map_preset,
                                     map_size=spec.map_size, seed=spec.seed)
        self.scratch = slot + 1
        self.w = dict(evalfn.DEFAULT_W, **(self.p['weights'] or {}))
        self.log = [] if log else None
        self.stats = dict(decisions=0, searched=0, rollouts=0, overrides=0, rollout_ms=0.0, rewards=0)
        self.pending = []      # the rest of a chosen command sequence, offered again next decision
        self.has_ops = self.p['mode'] != 'ai' and self._probe_ops()
        self.rejected = {}     # candidate -> (turn, stars below the AI's command), for skip_below
        self.turn = -1
        self.line = []         # a committed winning line (rows), replayed command by command
        self.pred = None       # (turn, depth), the AI commands the last chosen rollout played next, its value
        self._cont = None

    # ---- engine helpers
    def _probe_ops(self):
        """Whether polyhost has crazyplus.hpp's ops (the AI playing zero commands: nothing changes)."""
        try:
            sim.bot_turn(self.env, self.slot, 0)
            return True
        except polyenv.EnvError as ex:
            if 'unknown op' not in str(ex):
                raise
            return False

    def _find(self, legal, row):
        if row[0] == 'stay' or row[0] == 21:
            return sim.stay_code(row[1])
        if row[0] == 'train_best':  # the sturdiest unit the city can train now
            ks = np.nonzero((legal[:, 0] == 5) & (legal[:, 1] == row[1]))[0]
            if not len(ks):
                return None
            return int(max(ks, key=lambda k: D.stats(int(legal[k][3])).df * D.stats(int(legal[k][3])).hp))
        if None in row:  # a wildcard field (e.g. the attack of a move + attack, planned before the move)
            m = np.ones(len(legal), bool)
            for j, v in enumerate(row):
                if v is not None:
                    m &= legal[:, j] == v
            hit = np.nonzero(m)[0]
        else:
            hit = np.nonzero((legal == np.array(row, dtype=legal.dtype)).all(1))[0]
        return int(hit[0]) if len(hit) else None

    def _finish_turn(self, slot, st):
        """The AI plays the current player's turn to its end (and bot seats move). Returns (the
        commands it played, or [] when unknown, Status)."""
        if self.has_ops:
            try:
                return sim.bot_turn(self.env, slot)
            except polyenv.EnvError as ex:
                if 'unknown op' not in str(ex):
                    raise
                self.has_ops = False
        t, cur = st.turn, st.current
        while not st.ended and st.current == cur and st.turn == t:
            ok, st = self.env.step([(slot, polyenv.STEP_BOT_OWN)])[0]
        return [], st

    def rollout(self, blob, seq, depth=None):
        """Value of playing `seq` (rows or ('stay', tile)) now, then the AI to the end of our turn,
        the opponent model's reply, and `depth` - 1 more rounds."""
        depth = depth or self.p['depth']
        if seq is None:
            seq = []  # the AI's own StayCommand: the AI simply goes on
        t0 = time.time()
        env, s = self.env, self.scratch
        st = env.load(s, self.sim_spec, blob)
        line = []  # every command of ours this rollout played, as rows (a StayCommand as type 21)
        self._line, self._won = None, False
        for row in seq:
            if st.ended or st.current != self.me:
                break
            a = self._find(st.legal, row)
            if a is None:
                return None
            line.append((21, sim.stay_tile(a), -1, 0) if a <= sim.STAY else tuple(int(x) for x in st.legal[a]))
            ok, st = sim.play(env, s, a)
            if not ok:
                return None
        self._cont = None
        for k in range(depth):
            if st.ended:
                break
            if st.current == self.me:
                played, st = self._finish_turn(s, st)
                line += played
                if k == 0:
                    self._cont = played
            if not st.ended and st.current != self.me:  # a non-bot opponent seat (never in sim_spec)
                _, st = self._finish_turn(s, st)
        self._line = line
        self._won = st.ended and st.winner == self.me
        v = evalfn.evaluate_slot(env, s, st, self.me, self.w)
        self.stats['rollouts'] += 1
        self.stats['rollout_ms'] += 1000 * (time.time() - t0)
        return v

    # ---- the decision
    def act(self, st):
        self.stats['decisions'] += 1
        env, slot = self.env, self.slot
        if self.p['mode'] == 'ai':
            return polyenv.STEP_BOT_OWN
        legal = st.legal
        new_turn = st.turn != self.turn
        if new_turn:
            self.turn, self.rejected = st.turn, {}
        # a committed winning line: replay it command for command while it still wins
        if self.line:
            if new_turn and not self._line_wins(env.save(slot)):
                self._log(st, 'line dropped', None, None, 'no longer wins')
                self.line = []
            else:
                a = self._find(legal, self.line[0])
                if a is not None:
                    self.line.pop(0)
                    self.stats['line'] = self.stats.get('line', 0) + 1
                    return a
                self._log(st, 'line dropped', None, None, f'{self.line[0]} not legal')
                self.line = []
        c = env.bot([slot])[0]
        ai_row = tuple(int(x) for x in legal[c]) if c >= 0 else None
        # once a turn: does the AI's own play win within win_depth rounds (the opponent model replying)?
        if new_turn and self.p['win_depth'] and not (len(legal) and legal[0][0] == 11):
            blob = env.save(slot)
            v = self.rollout(blob, [ai_row] if ai_row is not None else None, self.p['win_depth'])
            if self._won:
                self._commit(st, 'win check', ai_row, self._line)
                return c if c >= 0 else polyenv.STEP_BOT_OWN
        # pending choices: city rewards
        if len(legal) and legal[0][0] == 11:
            self.pred = None
            if self.p['rewards']:
                pick, why = rules.choose_reward(env, slot, st, self.me, c, self.p)
                if pick is not None and pick != c and c >= 0 and self.p['reward_check']:
                    # the rule's pick must not lose more than reward_margin to the AI's own pick a few turns on
                    blob = env.save(slot)
                    h = self.p['econ_horizon']
                    v_rule = self.rollout(blob, [tuple(int(x) for x in legal[pick])], h)
                    v_ai = self.rollout(blob, [ai_row], h)
                    if v_rule is None or v_ai is None or v_rule < v_ai - self.p['reward_margin']:
                        self._log(st, 'reward kept', ai_row, ai_row, f'{why}: rule {(v_rule or 0) - (v_ai or 0):+.1f}')
                        pick = None
                if pick is not None and pick != c:
                    self.stats['rewards'] += 1
                    self._log(st, 'reward', ai_row, tuple(int(x) for x in legal[pick]), why)
                    return pick
            return c if c >= 0 else polyenv.STEP_BOT_OWN
        obs_me, obs_op = env.observe([(slot, self.me), (slot, 3 - self.me)])
        board = tactics.Board(obs_me, obs_op, legal)
        ai_seq = [ai_row] if ai_row is not None else None
        cands = tactics.candidates(board, ai_row, self.p, self.has_ops, self.pending)
        self.pending = []
        depth, margin = self.p['depth'], self.p['margin']
        if self.p['econ_turns'] and st.turn <= self.p['econ_turns'] and econ.is_econ(ai_row, obs_me.get('stars')):
            cands = cands + econ.candidates(legal, ai_row, self.p['econ_max'])
            depth, margin = self.p['econ_horizon'], self.p['econ_margin']
        if not cands:
            self._advance_pred(ai_row)
            return c if c >= 0 else polyenv.STEP_BOT_OWN
        self.stats['searched'] += 1
        blob = env.save(slot)
        pred = self.pred
        self.pred = None
        if pred is not None and pred[0] == (st.turn, depth) and pred[1] and \
                (pred[1][0] == ai_row or (ai_row is None and pred[1][0][0] == 21)):
            base, base_cont = pred[2], pred[1][1:]  # the AI's command is the one the last rollout predicted
            self.stats['cached'] = self.stats.get('cached', 0) + 1
            if self.p.get('check_cache'):
                fresh = self.rollout(blob, ai_seq, depth)
                self.stats['cache_err'] = self.stats.get('cache_err', 0) + abs(fresh - base)
                self.stats['cache_bad'] = self.stats.get('cache_bad', 0) + (abs(fresh - base) > 1e-6)
        else:
            base = self.rollout(blob, ai_seq, depth)
            base_cont = self._cont
            if self._won:
                self._commit(st, 'ai (wins)', ai_row, self._line)
                return c if c >= 0 else polyenv.STEP_BOT_OWN
        best, best_v, best_why, best_cont = None, base, 'ai', None
        scored = []
        skip = self.p['skip_below']
        t_start = time.time()
        budget = self.p['budget_ms']
        for seq, why in cands[:self.p['max_cands'] + (self.p['econ_max'] if depth != self.p['depth'] else 0)]:
            if budget is not None and 1000 * (time.time() - t_start) > budget:
                self.stats['budget_cut'] = self.stats.get('budget_cut', 0) + 1
                break
            key = tuple(seq)
            if skip is not None:
                r = self.rejected.get(key)
                if r is not None and r[0] == st.turn and r[1] < -skip:
                    continue
            v = self.rollout(blob, seq, depth)
            cont = self._cont
            if self._won:  # this candidate wins outright against the opponent model: play its line
                self._commit(st, why + ' (wins)', ai_row, self._line)
                self.pending = []
                return self._find(legal, self._line[0]) if self._line else (c if c >= 0 else polyenv.STEP_BOT_OWN)
            if v is not None:
                self.rejected[key] = (st.turn, v - base)
            scored.append((why, None if v is None else round(v - base, 1)))
            if v is not None and v > best_v + (margin if best is None else 0):
                best, best_v, best_why, best_cont = seq, v, why, cont
        if best is not None and self.p['verify_depth'] and depth < self.p['verify_depth']:
            d = self.p['verify_depth']
            vb, va = self.rollout(blob, best, d), self.rollout(blob, ai_seq, d)
            self.stats['verified'] = self.stats.get('verified', 0) + 1
            if vb is None or va is None or vb <= va + self.p['margin']:
                self.stats['vetoed'] = self.stats.get('vetoed', 0) + 1
                self._log(st, 'vetoed ' + best_why, ai_row, best[0], f'depth {d}: {(vb or 0) - (va or 0):+.1f}', scored)
                best = None
            else:
                scored.append((f'depth{d}', round(vb - va, 1)))
        if best is None:
            if base_cont is not None:
                self.pred = ((st.turn, depth), base_cont, base)
            return c if c >= 0 else polyenv.STEP_BOT_OWN
        if len(best) == 1 and best_cont is not None:
            self.pred = ((st.turn, depth), best_cont, best_v)
        self.stats['overrides'] += 1
        self._log(st, best_why, ai_row, best[0], f'+{best_v - base:.1f}', scored)
        if len(best) > 1:
            self.pending = [(best[1:], best_why + ' (cont.)')]
        a = self._find(legal, best[0])
        return a

    def _commit(self, st, why, ai_row, line):
        """Follow `line` (our commands of a rollout that won) from the next decision on; its first
        command is played now."""
        self.line = list(line[1:])
        self.pred = None
        self.stats['commits'] = self.stats.get('commits', 0) + 1
        self._log(st, 'commit: ' + why, ai_row, line[0] if line else None, f'{len(line)} commands to a win')

    def _line_wins(self, blob):
        """Replays the committed line on a clone (the opponent model replying): still a win?"""
        env, s = self.env, self.scratch
        st = env.load(s, self.sim_spec, blob)
        for row in self.line:
            if st.ended:
                break
            if st.current != self.me:
                _, st = self._finish_turn(s, st)
                continue
            a = self._find(st.legal, row)
            if a is None:
                return False
            ok, st = sim.play(env, s, a)
            if not ok:
                return False
        while not st.ended and st.current != self.me:
            _, st = self._finish_turn(s, st)
        return st.ended and st.winner == self.me

    def _advance_pred(self, ai_row):
        """The AI's command is played without a search: the prediction moves one command on."""
        pred = self.pred
        if pred is not None and pred[1] and (pred[1][0] == ai_row or (ai_row is None and pred[1][0][0] == 21)):
            self.pred = (pred[0], pred[1][1:], pred[2])
        else:
            self.pred = None

    def _log(self, st, kind, ai_row, chosen, why, scored=None):
        if self.log is not None:
            self.log.append(dict(turn=st.turn, kind=kind, ai=ai_row, chosen=chosen, why=why, scored=scored))
