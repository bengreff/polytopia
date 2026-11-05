"""Planner v2: the --reply planner (py/plan_agent.py, v1) plus hand-coded tactics (Ben, 2026-11-04).

Everything runs on the fog-safe samples of the game for the planning seat (Env.determinize), as in v1;
the real game is only touched to play the chosen lane. Per turn:
1. Network lanes, as v1: the top-move plan and plans - 1 sampled at the temperature, on sample 0.
2. Offence: the planner seat's tactical search (tactics/search.py) on sample 0; its best `lanes`
   sequences become extra lanes: the sequence first, then the network finishes the turn with its top
   move.
3. Every lane is replayed on the other samples (a command that isn't legal there is skipped), and
   saved before its End Turn.
4. The replies, after each lane's End Turn:
   - the network's greedy reply from the opponent's view (v1);
   - the threat: the opponent's best tactical sequence (the same search, for the opponent, on the same
     sample), then the opponent's network finishes its turn. Checked for the `threat_lanes` lanes
     with the best greedy-reply score, and for every tactical and repair lane.
   Both are scored by the critic at the planner's next turn; a lane's score per sample combines the
   two (`combine`: 'min' = the worse, 'mix' = a weighted mean), averaged over the samples.
5. Defensive repair: for the `repair_lanes` best lanes whose threat takes at least `repair_min` stars,
   repairs before End Turn are tried on the sample where the threat is worst: move a threatened unit
   out of reach (spacing), or put the sturdiest unit that can get there (Defender, Swordsman, Giant
   first) on a tile the attackers enter from (blocking). The threat search is re-run; repairs that
   cut its gain by `repair_cut` stars become lanes and are scored like the others.
6. The best lane (among the threat-checked ones) is played on the real game's copy; if all its
   commands apply, End Turn follows at once (what was scored), else the raw policy finishes the turn.
"""
import time

import numpy as np

import polyenv
from crazyplus.tactics import Board
from tactics.search import Tactician, material, row_of, seq_info, state_key

COMMANDS_PER_TURN_CAP = 400
END_TURN = 15
MOVE = 6

DEFAULTS = dict(
    lanes=3,               # tactical lanes from the planner seat's search on sample 0
    offence=dict(depth=14, beam=6, branch=10, max_sims=600, top=8),
    threat=dict(depth=8, beam=2, branch=6, max_sims=80, top=1),
    check=dict(depth=6, beam=2, branch=5, max_sims=50, top=1),   # the repairs' quick re-check
    threat_lanes=6,        # network lanes (best greedy-reply score) checked for the tactical reply
    threat_samples=1,      # samples the threat is searched on; on the others sample 0's threat is played
    threat_min=1.0,        # a threat counts if it gains the opponent at least this many stars
    combine='min',         # 'min' | 'mix'
    mix=0.5,               # 'mix': the tactical reply's weight
    repair_lanes=2,        # best lanes that get repairs ...
    repair_min=2.0,        # ... if their threat takes at least this many stars (mean over samples)
    repair_cands=4,        # repair candidates checked per lane
    repairs=2,             # repairs kept per lane
    repair_cut=1.0,        # a kept repair cuts the threat's gain by at least this many stars
    end_after_plan='post', # End Turn right after the chosen lane's commands: 'post' only for a repair played
                           # after the lane (so the policy can't undo it); True always; False never (v1: the
                           # raw policy plays on until it picks End Turn)
    skip_broken=False,     # a planned command not legal in the real game: skipped (True), or the raw
                           # policy finishes the turn from there (False, v1)
    budget=18.0,           # seconds: past this, the optional steps (repairs) are skipped
)

MAXL = 64           # lane slots per sample
ROOT = 200          # + d: sample d before anything is played
TS = 300            # + d * MAXL + k: the tactical reply of lane k on sample d
SCRATCH = range(500, 540)
CHECK = 560


def lane_slot(d, k):
    return 1 + d * MAXL + k


def ts_slot(d, k):
    return TS + d * MAXL + k


def end_row(st):
    e = np.nonzero(st.legal[:, 0] == END_TURN)[0] if len(st.legal) else []
    return int(e[0]) if len(e) else -1


def find(st, k):
    hit = np.nonzero((st.legal == np.asarray(k, st.legal.dtype)).all(1))[0] if len(st.legal) else []
    return int(hit[0]) if len(hit) else -1


class V2:
    def __init__(self, P, spec, blob, opt):
        self.P, self.env, self.spec, self.blob = P, P.env, spec, blob
        self.o = dict(DEFAULTS, **(opt or {}))
        for k in ('offence', 'threat', 'check'):
            self.o[k] = dict(DEFAULTS[k], **((opt or {}).get(k) or {}))
        self.t0 = time.time()
        self.times = {}

    def lap(self, name):
        t = time.time()
        self.times[name] = round(self.times.get(name, 0.0) + t - getattr(self, '_t', self.t0), 2)
        self._t = t

    # ---- lockstep play with the networks
    def plan_lanes(self, ks, sample_ks):
        """Lockstep on sample 0: lane k plays the planner's turn by its policy (argmax, or sampled at the
        temperature for k in sample_ks) until it picks End Turn (not played). Appends to self.plans."""
        P, env, sts = self.P, self.env, self.sts
        active = {k for k in ks if not sts[lane_slot(0, k)].ended and sts[lane_slot(0, k)].current == self.a_id}
        n = {k: 0 for k in ks}
        while active:
            kk = sorted(active)
            ids = [lane_slot(0, k) for k in kk]
            logits, _ = P.infer(P.net, ids, sts, env.observe([(i, 0) for i in ids]))
            acts = []
            for j, k in enumerate(kk):
                st = sts[ids[j]]
                row = logits[j][:len(st.legal)]
                if k in sample_ks:
                    z = (row - row.max()) / P.temperature
                    p = np.exp(z) / np.exp(z).sum()
                    a = int(P.rng.choice(len(p), p=p))
                else:
                    a = int(np.argmax(row))
                n[k] += 1
                if st.legal[a][0] == END_TURN or n[k] > COMMANDS_PER_TURN_CAP:
                    active.discard(k)
                else:
                    self.plans[k].append(P.key(st.legal[a]))
                    acts.append((ids[j], a, k))
            for (i, _, k), (ok, st) in zip(acts, env.step([(i, a) for i, a, _ in acts]) if acts else []):
                sts[i] = st
                if st.ended or st.current != self.a_id:
                    active.discard(k)

    def reply(self, slots):
        """Lockstep: the opponent's turn played greedily by the reply model from its own view, in each
        slot, until the planner is to move again (or the game ends)."""
        P, env, sts, a_id = self.P, self.env, self.sts, self.a_id
        replying = {i for i in slots if not sts[i].ended and sts[i].current != a_id}
        m = {i: 0 for i in replying}
        while replying:
            ids = sorted(replying)
            logits, _ = P.infer(P.model, ids, sts, env.observe([(i, 0) for i in ids]))
            acts = []
            for j, i in enumerate(ids):
                st = sts[i]
                a = int(np.argmax(logits[j][:len(st.legal)]))
                m[i] += 1
                if m[i] > COMMANDS_PER_TURN_CAP:
                    e = end_row(st)
                    a = e if e >= 0 else a
                acts.append((i, a))
            for (i, _), (ok, st) in zip(acts, env.step(acts)):
                sts[i] = st
                if st.ended or st.current == a_id:
                    replying.discard(i)

    def score(self, slots):
        """The critic's win estimate for the planner at its next turn (1 / 0 for a finished game)."""
        P, env, sts, a_id = self.P, self.env, self.sts, self.a_id
        live = [i for i in slots if not sts[i].ended]
        est = {}
        if live:
            _, v = P.infer(P.scorer, live, sts, env.observe([(i, a_id) for i in live]))
            est = dict(zip(live, (float(x) for x in v)))
        return {i: (1.0 if sts[i].winner == a_id else 0.0) if sts[i].ended else est[i] for i in slots}

    def replay_rows(self, pairs):
        """pairs: [(slot, rows)]: plays each slot's rows in order, batched across slots; a row that isn't
        legal in its slot is skipped (as v1's replay on the other samples)."""
        env, sts = self.env, self.sts
        L = max((len(r) for _, r in pairs), default=0)
        for i in range(L):
            acts = []
            for s, rows in pairs:
                st = sts[s]
                if i < len(rows) and not st.ended and st.current == self.a_id:
                    k = find(st, rows[i])
                    if k >= 0:
                        acts.append((s, k))
            for (s, _), (ok, st) in zip(acts, env.step(acts) if acts else []):
                sts[s] = st

    # ---- lanes, evaluated
    def evaluate(self, ks, always=()):
        """Lanes ks (their slots hold the position before End Turn on every sample): End Turn, the
        greedy reply, and the threat for the `threat_lanes` best of them by the greedy-reply score plus
        every lane in `always`; fills self.g, self.t, self.loss, self.tseq, self.checked."""
        env, sts, a_id, o, D = self.env, self.sts, self.a_id, self.o, self.D
        ids = [(d, k) for k in ks for d in range(D)]
        acts = []
        for d, k in ids:
            s = lane_slot(d, k)
            st = sts[s]
            if not st.ended and st.current == a_id:
                e = end_row(st)
                if e >= 0:
                    acts.append((s, e))
        for (s, _), (ok, st) in zip(acts, env.step(acts) if acts else []):
            sts[s] = st
        post = {(d, k): env.save(lane_slot(d, k)) for d, k in ids if not sts[lane_slot(d, k)].ended}
        self.lap('end_turn')
        self.reply([lane_slot(d, k) for d, k in ids])
        sc = self.score([lane_slot(d, k) for d, k in ids])
        for d, k in ids:
            self.g[(d, k)] = sc[lane_slot(d, k)]
        self.lap('greedy_reply')
        # the threat, for the chosen lanes
        always = set(always)
        mean_g = {k: np.mean([self.g[(d, k)] for d in range(D)]) for k in ks if k not in always}
        chk = sorted(mean_g, key=lambda k: -mean_g[k])[:o['threat_lanes']] + [k for k in ks if k in always]
        tac = Tactician(env, self.spec, SCRATCH, **o['threat'])
        replies = []
        for k in chk:
            self.checked.add(k)
            for d in range(D):
                self.t[(d, k)], self.loss[(d, k)], self.tseq[(d, k)] = self.g[(d, k)], 0.0, []
                if (d, k) not in post:
                    continue
                s = ts_slot(d, k)
                st = env.load(s, self.spec, post[(d, k)])
                obs = env.observe([(s, st.current), (s, a_id)])
                key = state_key(*obs)
                if key in self.cache:
                    res = self.cache[key]
                elif d >= o['threat_samples']:  # sample 0's threat, played on this sample
                    r0 = self.tseq.get((0, k))
                    res = dict(seq=r0, owner=None) if r0 else None
                else:
                    res = tac.search(s, st)
                    res = dict(res[0], owner=(d, k)) if res and res[0]['obj'] >= o['threat_min'] else None
                    self.cache[key] = res
                    self.n_threat += 1
                if res is None:
                    continue
                if res['owner'] == (d, k):  # the search's own end position
                    self.sts[s] = env.load(s, self.spec, res['blob'])
                    self.loss[(d, k)] = res['obj']
                else:  # the same sequence, played here (a row that isn't legal ends it)
                    played = 0
                    for r in res['seq']:
                        j = find(st, r)
                        if j < 0 or st.ended:
                            break
                        st = env.step([(s, j)])[0][1]
                        played += 1
                    if not played:
                        continue
                    self.sts[s] = st
                    m0, m1 = material(*obs), material(*env.observe([(s, 3 - a_id), (s, a_id)]))
                    self.loss[(d, k)] = (m1[0] - m0[0]) - (m1[1] - m0[1])
                self.tseq[(d, k)] = res['seq']
                replies.append((d, k))
        self.lap('threat_search')
        if replies:
            self.reply([ts_slot(d, k) for d, k in replies])
            sc = self.score([ts_slot(d, k) for d, k in replies])
            for d, k in replies:
                self.t[(d, k)] = sc[ts_slot(d, k)]
        self.lap('threat_reply')

    def combined(self, k):
        vals = []
        for d in range(self.D):
            g = self.g[(d, k)]
            t = self.t.get((d, k), g)
            vals.append(min(g, t) if self.o['combine'] == 'min' else (1 - self.o['mix']) * g + self.o['mix'] * t)
        return float(np.mean(vals))

    # ---- defensive repairs
    def _board(self, blob):
        st = self.env.load(CHECK, self.spec, blob)
        if st.ended or st.current != self.a_id:
            return None, st
        obs_me, obs_op = self.env.observe([(CHECK, self.a_id), (CHECK, 3 - self.a_id)])
        return Board(obs_me, obs_op, st.legal), st

    def _moves_for(self, board, legal, threatened, entries):
        """Spacing (each threatened unit to its two safest tiles, and all of them at once) and blocking
        (the two sturdiest units that can reach an entry tile) moves on this board: [rows]."""
        cands, safest = [], []
        for t in threatened:
            u = board.unit_at.get(t)
            if u is None or not u.mine:
                continue
            here = board.danger(u, u.tile)
            opts = sorted((board.danger(u, m), m, km) for m, km in board.moves.get(u.tile, {}).items() if m not in board.unit_at)
            opts = [x for x in opts if x[0] < here]
            for dz, m, km in opts[:2]:
                cands.append([row_of(legal, km)])
            if opts:
                safest.append(opts[0])
        if len(safest) > 1:
            used, rows = set(), []
            for dz, m, km in safest:
                if m not in used:
                    used.add(m)
                    rows.append(row_of(legal, km))
            cands.insert(0, rows)
        for e in entries:
            if e in board.unit_at:
                continue
            movers = sorted(((u.st.df * u.maxhp, u, board.moves[u.tile][e]) for u in board.my_units if e in board.moves.get(u.tile, {})),
                            key=lambda x: -x[0])
            for _, u, km in movers[:2]:
                cands.append([row_of(legal, km)])
        return cands

    def repair_candidates(self, k, d):
        """Repairs against lane k's threat on sample d: [(kind, rows)].
        - 'post': moves played after the lane's commands, for units that can still move;
        - 'pre': one move at the start of the turn (where the threatened unit stood then, or a sturdy
          unit onto the entry tile), after which the network plays the rest of the turn."""
        seq = self.tseq[(d, k)]
        entries, origins, targets = seq_info(seq)
        entries = list(dict.fromkeys(entries + origins))
        targets = list(dict.fromkeys(targets))
        out = []
        board, st = self._board(self.pre[(0, k)])
        if board is not None:
            out += [('post', c) for c in self._moves_for(board, st.legal, targets, entries)]
        pos = {}  # tile at the end of the lane -> tile at the start of the turn, through the lane's moves
        for r in self.plans[k]:
            if r[0] == MOVE:
                pos[int(r[2])] = pos.pop(int(r[1]), int(r[1]))
        board, st = self._board(self.root_blobs[0])
        if board is not None:
            out += [('pre', c) for c in self._moves_for(board, st.legal, [pos.get(t, t) for t in targets], entries)]
        seen, res = set(), []
        for kind, c in out:
            key = (kind, tuple(c))
            if key not in seen:
                seen.add(key)
                res.append((kind, c))
        return res[:self.o['repair_cands']]

    def threat_after(self, slot_blob, tac):
        """The threat's gain after End Turn from a pre-End-Turn position (None: can't end the turn)."""
        env = self.env
        st = env.load(CHECK, self.spec, slot_blob)
        if st.ended:
            return 0.0
        e = end_row(st)
        if e < 0:
            return None
        ok, st = env.step([(CHECK, e)])[0]
        if st.ended:
            return 0.0
        res = tac.search(CHECK, st)
        return res[0]['obj'] if res else 0.0

    def repairs(self):
        """Step 5: repair lanes for the best lanes with a real threat; returns their info."""
        env, spec, o, D = self.env, self.spec, self.o, self.D
        ranked = sorted(self.checked, key=lambda k: -self.combined(k))[:o['repair_lanes']]
        ctac = Tactician(env, spec, SCRATCH, **o['check'])
        cands = []  # (new lane, parent lane, kind, rows, sample, base loss)
        for k in ranked:
            losses = [self.loss.get((d, k), 0.0) for d in range(D)]
            if np.mean(losses) < o['repair_min'] or time.time() - self.t0 > o['budget']:
                continue
            d = int(np.argmax(losses))
            for kind, rows in self.repair_candidates(k, d):
                j = max(self.plans) + 1
                if j >= MAXL:
                    break
                self.kind[j] = 'rep'
                self.rkind[j] = kind
                if kind == 'post':
                    self.plans[j] = self.plans[k] + rows
                    for dd in range(D):
                        self.sts[lane_slot(dd, j)] = env.load(lane_slot(dd, j), spec, self.pre[(dd, k)])
                else:
                    self.plans[j] = list(rows)
                    for dd in range(D):
                        self.sts[lane_slot(dd, j)] = env.load(lane_slot(dd, j), spec, self.root_blobs[dd])
                cands.append((j, k, kind, rows, d, self.loss[(d, k)]))
        if not cands:
            return []
        # the repair rows on every sample, then the network finishes the 'pre' lanes on sample 0
        self.replay_rows([(lane_slot(dd, j), rows) for j, k, kind, rows, d, b in cands for dd in range(D)])
        pre_ks = [j for j, k, kind, rows, d, b in cands if kind == 'pre']
        if pre_ks:
            self.plan_lanes(pre_ks, set())
            for j in pre_ks:
                for dd in range(1, D):
                    self.sts[lane_slot(dd, j)] = env.load(lane_slot(dd, j), spec, self.root_blobs[dd])
            self.replay_rows([(lane_slot(dd, j), self.plans[j]) for j in pre_ks for dd in range(1, D)])
        for j, *_ in cands:
            for dd in range(D):
                self.pre[(dd, j)] = env.save(lane_slot(dd, j))
        self.lap('repair_build')
        # the quick check on the sample where the threat was found; keep the repairs that cut it most
        kept, info = {}, []
        for j, k, kind, rows, d, base in cands:
            if time.time() - self.t0 > o['budget']:
                break
            g2 = self.threat_after(self.pre[(d, j)], ctac)
            if g2 is not None and g2 <= base - o['repair_cut']:
                kept.setdefault(k, []).append((g2, j, kind, rows, base))
        new = []
        for k, lst in kept.items():
            lst.sort(key=lambda x: x[0])
            for g2, j, kind, rows, base in lst[:o['repairs']]:
                new.append(j)
                info.append(dict(lane=k, kind=kind, rows=len(rows), loss=round(base, 1), after=round(g2, 1)))
        for j, *_ in cands:
            if j not in new:
                self.plans.pop(j)
                self.kind.pop(j)
        self.lap('repair_check')
        if new:
            self.evaluate(new, always=new)
        return info

    # ---- the turn
    def run(self):
        P, env, spec, o = self.P, self.env, self.spec, self.o
        K, D = P.K, P.D
        self.D = D
        real = env.load(0, spec, self.blob)
        self.a_id = a_id = real.current
        turn = int(real.turn)
        seed = int(P.rng.integers(1 << 30))
        self.sts, self.plans, self.kind, self.rkind = {}, {}, {}, {}
        self.g, self.t, self.loss, self.tseq, self.checked, self.cache, self.pre = {}, {}, {}, {}, set(), {}, {}
        self.n_threat = 0
        roots = []
        for d in range(D):
            st = env.determinize(0, ROOT + d, a_id, seed * 1000 + d)
            roots.append((env.save(ROOT + d), st))
        self.root_blobs = [r[0] for r in roots]
        self.lap('samples')
        # 1. network lanes on sample 0
        for k in range(K):
            self.sts[lane_slot(0, k)] = env.load(lane_slot(0, k), spec, roots[0][0])
            self.plans[k], self.kind[k] = [], 'net'
        self.plan_lanes(range(K), set(range(1, K)))
        self.lap('net_lanes')
        # 2. tactical lanes
        tac_info = []
        if o['lanes'] > 0:
            tac = Tactician(env, spec, SCRATCH, **o['offence'])
            res = tac.search(ROOT, roots[0][1])
            ks = []
            for r in res[:o['lanes']]:
                k = K + len(ks)
                self.sts[lane_slot(0, k)] = env.load(lane_slot(0, k), spec, r['blob'])
                self.plans[k], self.kind[k] = list(r['seq']), 'tac'
                ks.append(k)
                tac_info.append(dict(obj=r['obj'], kills=r['kills'], lost=r['lost'], n=len(r['seq'])))
            self.lap('offence_search')
            if ks:
                self.plan_lanes(ks, set())
            self.lap('tac_lanes')
        lanes = sorted(self.plans)
        # 3. the other samples
        for d in range(1, D):
            for k in lanes:
                self.sts[lane_slot(d, k)] = env.load(lane_slot(d, k), spec, roots[d][0])
        self.replay_rows([(lane_slot(d, k), self.plans[k]) for d in range(1, D) for k in lanes])
        for d in range(D):
            for k in lanes:
                self.pre[(d, k)] = env.save(lane_slot(d, k))
        self.lap('replay')
        # 4. replies and threats
        tks = [k for k in lanes if self.kind[k] == 'tac']
        self.evaluate(lanes, always=tks)
        # 5. repairs
        rep_info = []
        if o['repair_lanes'] > 0 and time.time() - self.t0 < o['budget']:
            rep_info = self.repairs()
        n_rep = len(rep_info)
        # 6. choose and play
        score = {k: self.combined(k) for k in self.checked}
        best = max(score, key=lambda k: score[k])
        g_mean = {k: float(np.mean([self.g[(d, k)] for d in range(D)])) for k in self.plans if (0, k) in self.g}
        played, broken = self.play(best)
        info = dict(turn=turn, plans=K, samples=D, version=2, lanes=len(self.plans), tac_lanes=len(tks), repair_lanes=n_rep,
                    best=int(best), kind=self.kind[best], best_score=round(score[best], 4),
                    greedy_best=int(max(g_mean, key=lambda k: g_mean[k])), top_move_score=round(score.get(0, g_mean.get(0, 0.0)), 4),
                    threat_loss=round(float(np.mean([self.loss.get((d, best), 0.0) for d in range(D)])), 2),
                    threat_loss_top_move=round(float(np.mean([self.loss.get((d, 0), 0.0) for d in range(D)])), 2),
                    threat_searches=self.n_threat, tactical=tac_info, repairs=rep_info, broken=broken,
                    commands=len(played), seconds=round(time.time() - self.t0, 2), times=self.times)
        return played, info

    def play(self, best):
        """The chosen lane on the real game's copy (slot 0), as v1's step 5; End Turn right after it
        for a repair played after the lane (end_after_plan 'post'); otherwise, as v1, the raw policy plays
        on until it picks End Turn (it fills the turn after a sampled lane's End Turn)."""
        P, env, a_id = self.P, self.env, self.a_id
        real = env.load(0, self.spec, self.blob)
        played, broken = [], False
        for c in self.plans[best]:
            r = find(real, c)
            if r < 0:
                broken = True
                if self.o['skip_broken']:
                    continue
                break
            played.append(P.key(real.legal[r]))
            real = env.step([(0, r)])[0][1]
            if real.ended:
                return played, broken
        eap = self.o['end_after_plan']
        end_now = eap is True or (eap == 'post' and self.rkind.get(best) == 'post')
        if (not broken or self.o['skip_broken']) and end_now and not real.ended and real.current == a_id:
            e = end_row(real)
            if e >= 0:
                played.append(P.key(real.legal[e]))
                return played, broken
        k = 0
        while not real.ended and real.current == a_id and k < COMMANDS_PER_TURN_CAP:
            logits, _ = P.infer(P.net, [0], {0: real}, env.observe([(0, 0)]))
            a = int(np.argmax(logits[0][:len(real.legal)]))
            played.append(P.key(real.legal[a]))
            real = env.step([(0, a)])[0][1]
            if played[-1][0] == END_TURN:
                break
            k += 1
        if not real.ended and real.current == a_id:
            e = end_row(real)
            if e >= 0:
                played.append(P.key(real.legal[e]))
        return played, broken


def plan_turn(P, spec, blob, opt):
    return V2(P, spec, blob, opt).run()
