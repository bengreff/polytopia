"""Planner v3 (research log "Planner v3", Ben 2026-11-24): planner v2 (py/tactics/planner2.py) plus

1. Fog-correct samples: the engine's determinizer now moves enemy cities the planning seat can't locate to
   sites of the sample's own map (polyhost/determinize.hpp, relocateHiddenCities). That is in the engine
   build, not here: run v3 with an engine built after 2026-11-24.
2. A critic ensemble: a plan's score is the mean win estimate of all of P.scorers (the critics of several
   generations), not one critic's.
3. More fog-safe samples: P.D (v3 runs use 4; v2 used 2).
4. Threat response (Ben: "Is an enemy knight in a position where it could do some damage next turn? If so,
   defend or attack it... if a city is under siege the planner should try to figure out a way to stop that:
   giant popping, killing that unit"). After the lanes are scored, if the opponent's best tactical reply to
   one of the best lanes takes material, or an enemy unit stands on one of our cities (it captures next
   turn), then:
   - kill lanes: the planner seat's tactical search runs again with a bonus (kill_bonus stars) for each of
     those units killed (the reply's attackers, the besiegers); its best sequences become lanes, finished by
     the network;
   - a giant lane: the network's top-move plan, taking the SuperUnit reward (a giant) whenever a city
     level-up offers it.
   Both are scored like every other lane (the critic decides). v2's defensive repairs (moving threatened
   units away, blocking entry tiles) still run, for more lanes (repair_lanes 3).
5. Re-planning within the turn: the chosen lane is played command by command on the real game's copy. When a
   command shows new tiles or new enemy units, the rest of the turn is planned again from that position (at
   most max_replans times a turn), with the old plan's remaining commands as one of the lanes ('cont'), so a
   re-plan never loses sight of the old plan. Commands already played are never taken back, so nothing the
   real game showed after a command leaks into an earlier choice.

The reply model (P.model: how the opponent's turns are simulated) is chosen when the Planner is made
(model_path): the planner's own network, or R7 (the human-imitation network).
"""
import time

import numpy as np

import polyenv
from tactics.planner2 import DEFAULTS as V2_DEFAULTS, MAXL, ROOT, V2, END_TURN, MOVE, end_row, find
from tactics.search import Tactician

CMD = {v: k for k, v in polyenv.COMMAND_TYPES.items()}
ATTACK, CAPTURE, CITY_REWARD = CMD['Attack'], CMD['Capture'], CMD['CityReward']
SUPER_UNIT = 6  # CityReward 'SuperUnit' (the giant), as py/strategy_positions.py's REWARD
F = {n: i for i, n in enumerate(polyenv.TILE_FEATURES)}
CITY_IMP = 1

DEFAULTS = dict(
    V2_DEFAULTS,
    threat_lanes=8,     # (v2: 6)
    repair_lanes=3,     # (v2: 2)
    kill_lanes=2,       # tactical lanes aimed at the threat's attackers and the besiegers
    kill_bonus=4.0,     # stars per such unit killed, on top of its material value
    giant_lane=True,    # the top-move plan taking the giant at every level-up, when threatened
    response_lanes=4,   # best lanes whose threats give the kill lanes their targets
    max_replans=3,      # re-plans per turn, after a command shows new tiles or enemy units
    replan_min=2,       # ... only if at least this many planned commands remain
    replan_lanes=8,     # network lanes in a re-plan (the first plan uses P.K)
    budget=60.0,        # seconds from the turn's start: past this, no repairs, responses or re-plans
)


def attacker_tiles(seq):
    """Start tiles of the units a tactical sequence uses (a unit that moved earlier in it counts once)."""
    moved_to, out = set(), set()
    for r in seq:
        t, a, b = int(r[0]), int(r[1]), int(r[2])
        if t == MOVE:
            if a not in moved_to:
                out.add(a)
            moved_to.add(b)
        elif t in (ATTACK, CAPTURE):
            if a not in moved_to:
                out.add(a)
    return out


class V3(V2):
    def __init__(self, P, spec, blob, opt, off=0, depth=0, cont=None, t_turn=None):
        super().__init__(P, spec, blob, dict(DEFAULTS, **(opt or {})), off)
        self.o = dict(DEFAULTS, **(opt or {}))
        for k in ('offence', 'threat', 'check'):
            self.o[k] = dict(DEFAULTS[k], **((opt or {}).get(k) or {}))
        self.depth, self.cont = depth, list(cont or [])
        self.t_turn = t_turn if t_turn is not None else self.t0
        self.sub_infos = []

    def over_budget(self):
        return time.time() - self.t_turn > self.o['budget']

    # ---- the critic ensemble
    def score(self, slots):
        P, env, sts, a_id = self.P, self.env, self.sts, self.a_id
        live = [i for i in slots if not sts[i].ended]
        est = {}
        if live:
            obs = env.observe([(i, a_id) for i in live])
            vs = [P.infer(c, live, sts, obs)[1] for c in getattr(P, 'scorers', None) or [P.scorer]]
            est = dict(zip(live, (float(x) for x in np.mean(np.asarray(vs, np.float64), 0))))
        return {i: (1.0 if sts[i].winner == a_id else 0.0) if sts[i].ended else est[i] for i in slots}

    # ---- lockstep play, with the giant rule
    def plan_lanes(self, ks, sample_ks, net=None, temp=None, giant=()):
        """V2.plan_lanes, except that lanes in `giant` take the SuperUnit reward whenever it is offered."""
        if not giant:
            return super().plan_lanes(ks, sample_ks, net, temp)
        P, env, sts = self.P, self.env, self.sts
        net, temp = net or P.net, temp or P.temperature
        active = {k for k in ks if not sts[self.ls(0, k)].ended and sts[self.ls(0, k)].current == self.a_id}
        n = {k: 0 for k in ks}
        while active:
            kk = sorted(active)
            ids = [self.ls(0, k) for k in kk]
            logits, _ = P.infer(net, ids, sts, env.observe([(i, 0) for i in ids]))
            acts = []
            for j, k in enumerate(kk):
                st = sts[ids[j]]
                row = logits[j][:len(st.legal)]
                g = np.nonzero((st.legal[:, 0] == CITY_REWARD) & (st.legal[:, 3] == SUPER_UNIT))[0] if k in giant else []
                if len(g):
                    a = int(g[0])
                elif k in sample_ks:
                    z = (row - row.max()) / temp
                    p = np.exp(z) / np.exp(z).sum()
                    a = int(P.rng.choice(len(p), p=p))
                else:
                    a = int(np.argmax(row))
                n[k] += 1
                if st.legal[a][0] == END_TURN or n[k] > 400:
                    active.discard(k)
                else:
                    self.plans[k].append(P.key(st.legal[a]))
                    acts.append((ids[j], a, k))
            for (i, _, k), (ok, st) in zip(acts, env.step([(i, a) for i, a, _ in acts]) if acts else []):
                sts[i] = st
                if st.ended or st.current != self.a_id:
                    active.discard(k)

    # ---- what the planning seat sees, for the re-planning trigger and the sieges
    def seen(self, slot):
        o = self.env.observe([(slot, self.a_id)])[0]
        tl = o.tiles.reshape(-1, o.tiles.shape[-1])
        enemy = frozenset(np.nonzero((tl[:, F['unit']] > 0) & (tl[:, F['unit_owner']] == 2))[0].tolist())
        return int((tl[:, F['explored']] > 0).sum()), enemy, tl

    def besiegers(self, slot):
        """Tiles of enemy units standing on the planning seat's cities (they capture at their next turn)."""
        _, _, tl = self.seen(slot)
        mine = (tl[:, F['improvement']] == CITY_IMP) & (tl[:, F['owner']] == 1)
        return set(np.nonzero(mine & (tl[:, F['unit']] > 0) & (tl[:, F['unit_owner']] == 2))[0].tolist())

    def new_lanes_done(self, new):
        """New lanes (built on sample 0): replayed on the other samples, saved before End Turn, evaluated."""
        env, spec, D = self.env, self.spec, self.D
        for d in range(1, D):
            for k in new:
                self.sts[self.ls(d, k)] = env.load(self.ls(d, k), spec, self.root_blobs[d])
        self.replay_rows([(self.ls(d, k), self.plans[k]) for d in range(1, D) for k in new])
        for d in range(D):
            for k in new:
                self.pre[(d, k)] = env.save(self.ls(d, k))
        self.evaluate(new, always=new)

    # ---- threat response
    def responses(self):
        o, env, spec = self.o, self.env, self.spec
        targets = set(self.sieged)
        ranked = sorted(self.checked, key=lambda k: -self.combined(k))[:o['response_lanes']]
        for k in ranked:
            for d in range(self.D):
                if self.loss.get((d, k), 0.0) >= o['threat_min'] and self.tseq.get((d, k)):
                    targets |= attacker_tiles(self.tseq[(d, k)])
        if not targets:
            return []
        new, info = [], []
        if o['kill_lanes'] > 0:
            tac = Tactician(env, spec, self.scratch, **dict(o['offence'], targets=tuple(sorted(targets)), target_bonus=o['kill_bonus']))
            st0 = env.load(self.off + ROOT, spec, self.root_blobs[0])
            have = {tuple(map(tuple, self.plans[k])) for k in self.plans if self.kind[k] == 'tac'}
            for r in tac.search(self.off + ROOT, st0):
                if len(new) >= o['kill_lanes'] or max(self.plans) + 1 >= MAXL:
                    break
                if tuple(map(tuple, r['seq'])) in have:
                    continue
                k = max(self.plans) + 1
                self.sts[self.ls(0, k)] = env.load(self.ls(0, k), spec, r['blob'])
                self.plans[k], self.kind[k] = list(r['seq']), 'kill'
                new.append(k)
                info.append(dict(kind='kill', obj=r['obj'], kills=r['kills'], n=len(r['seq'])))
            if new:
                self.plan_lanes(list(new), set())
        if o['giant_lane'] and max(self.plans) + 1 < MAXL:
            k = max(self.plans) + 1
            self.sts[self.ls(0, k)] = env.load(self.ls(0, k), spec, self.root_blobs[0])
            self.plans[k], self.kind[k] = [], 'giant'
            self.plan_lanes([k], set(), giant={k})
            if any(int(r[0]) == CITY_REWARD and int(r[3]) == SUPER_UNIT for r in self.plans[k]):
                new.append(k)
                info.append(dict(kind='giant', n=len(self.plans[k])))
            else:
                self.plans.pop(k)
                self.kind.pop(k)
        self.lap('response_build')
        if new:
            self.new_lanes_done(new)
            self.lap('response_eval')
        return [dict(x, targets=len(targets)) for x in info]

    # ---- the turn
    def run(self):
        P, env, spec, o = self.P, self.env, self.spec, self.o
        K = P.K if self.depth == 0 else min(P.K, o['replan_lanes'])
        D = P.D
        self.D = D
        real = env.load(self.real, spec, self.blob)
        self.a_id = a_id = real.current
        turn = int(real.turn)
        seed = int(P.rng.integers(1 << 30))
        self.sts, self.plans, self.kind, self.rkind = {}, {}, {}, {}
        self.g, self.t, self.loss, self.tseq, self.checked, self.cache, self.pre = {}, {}, {}, {}, set(), {}, {}
        self.n_threat = 0
        roots = []
        for d in range(D):
            st = env.determinize(self.real, self.off + ROOT + d, a_id, seed * 1000 + d)
            roots.append((env.save(self.off + ROOT + d), st))
        self.root_blobs = [r[0] for r in roots]
        self.sieged = self.besiegers(self.off + ROOT)  # (the planning seat's own cities: no hidden part)
        self.lap('samples')
        # 1. network lanes on sample 0
        for k in range(K):
            self.sts[self.ls(0, k)] = env.load(self.ls(0, k), spec, roots[0][0])
            self.plans[k], self.kind[k] = [], 'net'
        self.plan_lanes(range(K), set(range(1, K)))
        self.lap('net_lanes')
        # 1b. the old plan's remaining commands, finished by the network (re-plans only)
        if self.cont:
            k = max(self.plans) + 1
            self.sts[self.ls(0, k)] = st = env.load(self.ls(0, k), spec, roots[0][0])
            self.plans[k], self.kind[k] = [], 'cont'
            for r in self.cont:
                j = find(st, r)
                if j < 0 or st.ended or st.current != a_id:
                    continue
                st = env.step([(self.ls(0, k), j)])[0][1]
                self.sts[self.ls(0, k)] = st
                self.plans[k].append(P.key(r))
            self.plan_lanes([k], set())
            self.lap('cont_lane')
        # 2. tactical lanes
        tac_info = []
        if o['lanes'] > 0:
            tac = Tactician(env, spec, self.scratch, **o['offence'])
            res = tac.search(self.off + ROOT, roots[0][1])
            ks = []
            base = max(self.plans) + 1
            for r in res[:o['lanes']]:
                k = base + len(ks)
                self.sts[self.ls(0, k)] = env.load(self.ls(0, k), spec, r['blob'])
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
                self.sts[self.ls(d, k)] = env.load(self.ls(d, k), spec, roots[d][0])
        self.replay_rows([(self.ls(d, k), self.plans[k]) for d in range(1, D) for k in lanes])
        for d in range(D):
            for k in lanes:
                self.pre[(d, k)] = env.save(self.ls(d, k))
        self.lap('replay')
        # 4. replies and threats
        tks = [k for k in lanes if self.kind[k] in ('tac', 'cont')]
        self.evaluate(lanes, always=tks)
        # 4b. threat response: kill lanes and the giant lane
        resp_info = [] if self.over_budget() else self.responses()
        # 5. repairs
        rep_info = []
        if o['repair_lanes'] > 0 and not self.over_budget():
            rep_info = self.repairs()
        # 6. choose and play
        score = {k: self.combined(k) for k in self.checked}
        best = max(score, key=lambda k: score[k])
        g_mean = {k: float(np.mean([self.g[(d, k)] for d in range(D)])) for k in self.plans if (0, k) in self.g}
        played, broken = self.play(best)
        info = dict(turn=turn, plans=K, samples=D, version=3, depth=self.depth, lanes=len(self.plans), tac_lanes=len(tks),
                    repair_lanes=len(rep_info), responses=resp_info, sieged=len(self.sieged),
                    best=int(best), kind=self.kind[best], best_score=round(score[best], 4),
                    mean_score=round(float(np.mean(list(score.values()))), 4),
                    spread=round(float(np.std(list(score.values()))), 4),
                    greedy_best=int(max(g_mean, key=lambda k: g_mean[k])) if g_mean else -1,
                    top_move_score=round(score.get(0, g_mean.get(0, 0.0)), 4),
                    threat_loss=round(float(np.mean([self.loss.get((d, best), 0.0) for d in range(D)])), 2),
                    threat_loss_top_move=round(float(np.mean([self.loss.get((d, 0), 0.0) for d in range(D)])), 2),
                    threat_searches=self.n_threat, tactical=tac_info, repairs=rep_info, broken=broken,
                    commands=len(played), seconds=round(time.time() - self.t0, 2), times=self.times,
                    replans=[dict(kind=s.get('kind'), depth=s.get('depth'), seconds=s.get('seconds')) for s in self.sub_infos])
        if self.depth == 0:  # the whole turn's figures
            info['turn_seconds'] = round(time.time() - self.t_turn, 2)
            info['n_replans'] = self.count_replans()
        return played, info

    def count_replans(self):
        return len(self.sub_infos) + sum(s.get('n_replans_below', 0) for s in self.sub_infos)

    def play(self, best):
        """V2.play, command by command, with re-planning when a command shows something new."""
        P, env, a_id, o = self.P, self.env, self.a_id, self.o
        real = env.load(self.real, self.spec, self.blob)
        played, broken = [], False
        rows = self.plans[best]
        explored0, enemy0, _ = self.seen(self.real)
        for i, c in enumerate(rows):
            r = find(real, c)
            if r < 0:
                broken = True
                if o['skip_broken']:
                    continue
                break
            played.append(P.key(real.legal[r]))
            real = env.step([(self.real, r)])[0][1]
            if real.ended or real.current != a_id:
                return played, broken
            rest = rows[i + 1:]
            if self.depth < o['max_replans'] and len(rest) >= o['replan_min'] and not self.over_budget():
                explored1, enemy1, _ = self.seen(self.real)
                if explored1 > explored0 or (enemy1 - enemy0):
                    sub = V3(P, self.spec, env.save(self.real), self.o, self.off, depth=self.depth + 1, cont=rest, t_turn=self.t_turn)
                    played2, info2 = sub.run()
                    info2['n_replans_below'] = sub.count_replans()
                    self.sub_infos.append(info2)
                    return played + played2, broken or bool(info2.get('broken'))
                explored0, enemy0 = explored1, enemy1
        eap = o['end_after_plan']
        end_now = eap is True or (eap == 'post' and self.rkind.get(best) == 'post')
        if (not broken or o['skip_broken']) and end_now and not real.ended and real.current == a_id:
            e = end_row(real)
            if e >= 0:
                played.append(P.key(real.legal[e]))
                return played, broken
        k = 0
        while not real.ended and real.current == a_id and k < 400:
            logits, _ = P.infer(P.net, [self.real], {self.real: real}, env.observe([(self.real, 0)]))
            a = int(np.argmax(logits[0][:len(real.legal)]))
            played.append(P.key(real.legal[a]))
            real = env.step([(self.real, a)])[0][1]
            if played[-1][0] == END_TURN:
                break
            k += 1
        if not real.ended and real.current == a_id:
            e = end_row(real)
            if e >= 0:
                played.append(P.key(real.legal[e]))
        return played, broken


def plan_turn(P, spec, blob, opt, off=0):
    return V3(P, spec, blob, opt, off).run()
