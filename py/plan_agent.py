"""The --reply planner (py/turn_search.py, research log S9 read parts 3-4) for play against a person:
py/agent_server.py --planner uses it for the network's seat in the browser game.

Each of the network's turns is planned once, at its first decision: polyhost serve sends the game's
saved state (POLY_AGENT_STATE), which is loaded into this module's own engine process (slot 0).
The planning is turn_search.py's search_reply at the commit that measured it (2026-11-02):
- 1 top-move plan and plans - 1 plans sampled at the temperature, made in lockstep on a fog-safe
  sample of the game for the network's own view (Env.determinize), then replayed on samples - 1 more;
- End Turn, then the opponent's whole reply played greedily by the network from the opponent's own
  view of the sample;
- each plan scored by the scorer's win estimate at the network's next turn (a plan that wins scores 1),
  averaged over the samples.
The best plan is then played on the copy of the real game in slot 0, the raw policy finishing the turn
if one of its commands doesn't apply there; the commands played (End Turn included) are what the
browser game is sent, one per decision. The copy is the real game, so the same commands apply there.

The network decides only from what its seat may know: plans are made and scored on fog-safe samples,
and the raw policy's moves use its own fog-limited observation, as in the evaluations.
"""
import time

import numpy as np
import torch

import eval_vs_bot
import fast_infer
import polyenv
from selfplay import load_net

COMMANDS_PER_TURN_CAP = 400
END_TURN = {v: k for k, v in polyenv.COMMAND_TYPES.items()}['EndTurn']


class Planner:
    def __init__(self, env, net_path, scorer_path, device, plans=16, samples=2, temperature=0.5, model_path=None, seed=None,
                 tactical=None):
        """tactical: None plans as v1 (the default, unchanged); a dict (possibly empty) plans with planner
        v2's tactics (py/tactics/planner2.py: tactical lanes, the opponent's tactical reply, defensive
        repairs), the dict overriding planner2.DEFAULTS."""
        self.tactical = tactical
        self.env, self.device = env, device
        self.K, self.D, self.temperature = plans, samples, temperature
        self.net = fast_infer.FastPolicy(load_net(net_path, device), device, bf16=False)
        self.model = self.net if not model_path else fast_infer.FastPolicy(load_net(model_path, device), device, bf16=False)
        self.scorer = self.net if not scorer_path else fast_infer.FastPolicy(load_net(scorer_path, device), device, bf16=False)
        self.rng = np.random.default_rng(seed)

    def infer(self, net, slots, sts, obs):
        logits, v = net(eval_vs_bot.batch_inputs(obs, [sts[s] for s in slots]))
        return logits.float().cpu().numpy(), torch.sigmoid(v.float()).cpu().numpy()

    @staticmethod
    def key(row):
        return tuple(int(x) for x in row)

    @staticmethod
    def find(st, k):
        hit = np.nonzero((st.legal == np.asarray(k, st.legal.dtype)).all(1))[0] if len(st.legal) else []
        return int(hit[0]) if len(hit) else -1

    def plan_turn(self, spec, blob):
        """Loads the real game (blob) and plans the current player's turn. Returns (the commands to play,
        as (type, a, b, arg) rows, End Turn included; a dict about the search)."""
        if self.tactical is not None:
            from tactics import planner2
            return planner2.plan_turn(self, spec, blob, self.tactical)
        env, K, D = self.env, self.K, self.D
        t0 = time.time()
        real = env.load(0, spec, blob)
        a_id, turn = real.current, int(real.turn)
        seed = int(self.rng.integers(1 << 30))
        slot = lambda d, k: 1 + d * K + k
        sts = {}
        for d in range(D):
            for k in range(K):
                sts[slot(d, k)] = env.determinize(0, slot(d, k), a_id, seed * 1000 + d)
        plans = {k: [] for k in range(K)}
        # 1. Plans on sample 0, in lockstep.
        active, n = set(range(K)), {k: 0 for k in range(K)}
        while active:
            ks = sorted(active)
            ids = [slot(0, k) for k in ks]
            logits, _ = self.infer(self.net, ids, sts, env.observe([(i, 0) for i in ids]))
            acts = []
            for j, k in enumerate(ks):
                st = sts[ids[j]]
                row = logits[j][:len(st.legal)]
                if k == 0:
                    a = int(np.argmax(row))
                else:
                    z = (row - row.max()) / self.temperature
                    p = np.exp(z) / np.exp(z).sum()
                    a = int(self.rng.choice(len(p), p=p))
                n[k] += 1
                if st.legal[a][0] == END_TURN or n[k] > COMMANDS_PER_TURN_CAP:
                    active.discard(k)
                else:
                    plans[k].append(self.key(st.legal[a]))
                    acts.append((ids[j], a))
            for (i, _), (ok, st) in zip(acts, env.step(acts) if acts else []):
                sts[i] = st
                if st.ended:
                    active.discard((i - 1) % K)
        # 2. The same plans on the other samples.
        for d in range(1, D):
            for k in range(K):
                for c in plans[k]:
                    st = sts[slot(d, k)]
                    if st.ended:
                        break
                    r = self.find(st, c)
                    if r >= 0:
                        sts[slot(d, k)] = env.step([(slot(d, k), r)])[0][1]
        # 3. End Turn, then the opponent's reply (greedy, from its own view), until A is to move again.
        ends = []
        for i, st in sts.items():
            if not st.ended and st.current == a_id:
                e = np.nonzero(st.legal[:, 0] == END_TURN)[0]
                if len(e):
                    ends.append((i, int(e[0])))
        for (i, _), (ok, st) in zip(ends, env.step(ends) if ends else []):
            sts[i] = st
        replying = {i for i, st in sts.items() if not st.ended and st.current != a_id}
        m = {i: 0 for i in replying}
        while replying:
            ids = sorted(replying)
            logits, _ = self.infer(self.model, ids, sts, env.observe([(i, 0) for i in ids]))
            acts = []
            for j, i in enumerate(ids):
                st = sts[i]
                a = int(np.argmax(logits[j][:len(st.legal)]))
                m[i] += 1
                if m[i] > COMMANDS_PER_TURN_CAP:
                    e = np.nonzero(st.legal[:, 0] == END_TURN)[0]
                    a = int(e[0]) if len(e) else a
                acts.append((i, a))
            for (i, _), (ok, st) in zip(acts, env.step(acts)):
                sts[i] = st
                if st.ended or st.current == a_id:
                    replying.discard(i)
        # 4. Scores: A's win estimate at its next turn, averaged over the samples.
        live = [i for i, st in sts.items() if not st.ended]
        est = {}
        if live:
            _, v = self.infer(self.scorer, live, sts, env.observe([(i, a_id) for i in live]))
            est = dict(zip(live, v))
        score = {}
        for k in range(K):
            vals = []
            for d in range(D):
                st = sts[slot(d, k)]
                vals.append((1.0 if st.winner == a_id else 0.0) if st.ended else float(est[slot(d, k)]))
            score[k] = float(np.mean(vals))
        best = max(score, key=lambda k: score[k])
        # 5. The best plan on the real game's copy; the raw policy finishes the turn if a command doesn't apply.
        real = env.load(0, spec, blob)
        played, broken = [], False
        for c in plans[best]:
            r = self.find(real, c)
            if r < 0:
                broken = True
                break
            played.append(self.key(real.legal[r]))
            real = env.step([(0, r)])[0][1]
            if real.ended:
                break
        k = 0
        while not real.ended and real.current == a_id and k < COMMANDS_PER_TURN_CAP:
            logits, _ = self.infer(self.net, [0], {0: real}, env.observe([(0, 0)]))
            a = int(np.argmax(logits[0][:len(real.legal)]))
            played.append(self.key(real.legal[a]))
            real = env.step([(0, a)])[0][1]
            if played[-1][0] == END_TURN:
                break
            k += 1
        if not real.ended and real.current == a_id:
            e = np.nonzero(real.legal[:, 0] == END_TURN)[0]
            if len(e):
                played.append(self.key(real.legal[int(e[0])]))
        info = dict(turn=turn, plans=K, samples=D, best=int(best), best_score=round(score[best], 4),
                    top_move_score=round(score[0], 4), gain=round(score[best] - score[0], 4),
                    spread=round(max(score.values()) - min(score.values()), 4), broken=broken,
                    commands=len(played), seconds=round(time.time() - t0, 2))
        return played, info
