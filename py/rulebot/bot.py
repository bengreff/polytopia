"""RuleBot: a fixed-logic player for the championship format (1v1 Might, Lakes, Small).

Each call to act() picks one row of the game's own legal-command list from the bot's own
observation (its client's view). A turn runs through phases in priority order; every decision
re-plans from the current view, so the bot never tracks the rules itself:

  pending choice (city reward, peace) -> promote -> urgent garrison -> research (before captures:
  cheaper) -> capture / ruins -> attacks (kill sequences, good trades) -> high-value improvements ->
  unit moves (garrisons, defence, villages, army, exploration) -> training -> other improvements ->
  boat upgrades -> heal -> end turn.

Strategy state kept across turns (memory, all from what the bot has seen): enemy unit types and
improvements seen (to infer enemy techs), known enemy cities, each unit's long-range goal, and the
army's mode (gather near the target city, or assault it).
"""
import collections

from . import data as D
from . import econ
from . import geo
from . import tactics
from .state import View

DEFAULTS = dict(
    research_ratio=1.0,       # research a tech when its value >= this x its price
    w_swordsman=2.8, w_catapult=3.2, w_archer=2.9, w_knight=2.2, w_rider=1.3, w_defender=0.9,
    ranged_share=0.25,        # ranged units are preferred while below this share of the army
    cloak_guard=True,         # once enemy cloaks/daggers are seen, keep every city garrisoned
    naval_transport=True,     # army units sail from our ports when the land route is long
    embark_min=4.0,           # land cost to the front above which embarking is considered (sweep: 3-5 flat, 7 lower)
    capital_bias=3.0,         # target choice: the enemy capital counts this much nearer
    attack_min=-1.5,          # attack plans need at least this score (aggression paid: sweep v13-v14)
    explore_until=8,          # turns in which spare units explore
    explorers=2,              # max units exploring at once (while no enemy city is known: +1)
    capital_scouts=1,         # units kept exploring until the enemy capital is found
    capital_prior_w=40.0,     # weight of the enemy-capital posterior in scout goals
    snipe_bonus=60,           # move score for walking into an empty enemy capital
    village_turns=7,          # max travel turns to chase a village
    army_base=4.0,            # army value target (stars): base + per_turn * turn
    army_per_turn=2.2,
    bank_cap_base=6,          # stars above base + turn/3 at the train phase: train anyway
    mil_frac_early=0.25,      # share of stars kept for units before contact
    mil_frac_war=0.55,        # after contact
    assault_ratio=1.3,        # army strength near the target vs its defenders to assault
    assault_min_units=3,
    stage_dist=3.5,           # gather this far (land cost) from the target city
    build_ratio_hi=0.95,      # value/cost of builds done before training
    build_ratio_lo=0.45,      # with leftover stars
    assault_danger=0.35,      # danger penalty weight while assaulting
    danger_scale=1.0,         # all movement danger penalties x this
    defend_need=3.0,          # enemy strength near a city (minus ours) that pulls the front there
    advance_ratio=1.3,        # local strength needed to move the front forward
    advance_min=6.0,
    capital_wall=True,        # take the wall at the capital's level 3 once the enemy is met
    capital_wall_turn=99,     # ... or from this turn on regardless
    cap_defend_ratio=0.6,     # recall the army when enemy strength near the capital >= this x ours
    cap_defend_radius=8,      # land cost from the capital within which units are recalled
)


def pen_for(dmg, u):
    return (4.0 + tactics.my_value(u)) if dmg >= u.hp else 3.0 * dmg / u.maxhp


class RuleBot:
    def __init__(self, params=None, log=False):
        self.p = dict(DEFAULTS)
        if params:
            self.p.update(params)
        self.mem = {'seen_types': set(), 'seen_imps': set(), 'contact_turn': None, 'enemy_cities': {},
                    'mode': 'gather', 'target': None}
        self.ugoal = {}  # my unit's tile -> (kind, goal tile), kept across turns
        self.turn = -1
        self.logging = log
        self.log = []
        self.stats = collections.Counter()
        self.threat_level = 0
        self._last_move = None
        self._last_attack = None
        self.checks = []  # (predicted dmg, actual dmg, predicted ret, actual ret, attacker, defender, bonus info)

    # ------------------------------------------------------------------ entry
    def act(self, obs, status):
        import time as _t
        t0 = _t.time()
        row = self._act(obs, status)
        dt = _t.time() - t0
        if dt > 0.25:
            self.stats['slow_decisions'] += 1
            if self.logging:
                self.log.append(f'   (slow decision: {dt:.2f} s)')
        return row

    def _act(self, obs, status):
        v = View(obs, status.legal)
        if self._last_attack is not None:
            self._check_attack(v)
        if v.turn != self.turn:
            self._new_turn(v)
        self._sync_goals(v)
        self.n_dec += 1
        if self.n_dec > 300 and v.end is not None:
            return self._do(v, v.end, 'decision cap')
        if v.rewards:
            row, why = econ.choose_reward(self, v)
            return self._do(v, row, 'reward ' + why)
        if v.peace_resp:
            rej = [k for arg, k in v.peace_resp if arg % 2 == 0]
            return self._do(v, rej[0] if rej else v.peace_resp[0][1], 'peace: reject')
        for phase in (self.ph_promote, self.ph_urgent_train, self.ph_research, self.ph_capture, self.ph_ruins,
                      self.ph_pending, self.ph_snipe, self.ph_attack, self.ph_build_hi, self.ph_moves, self.ph_train,
                      self.ph_research_late, self.ph_build_lo, self.ph_upgrade, self.ph_spend, self.ph_heal,
                      self.ph_recover):
            r = phase(v)
            if r is not None:
                return r
        return self._do(v, v.end, 'end turn')

    def _do(self, v, row, why):
        ty, a, b, arg = (int(x) for x in v.legal[row])
        if self.logging:
            import polyenv
            self.log.append(f't{v.turn} ★{v.stars} {polyenv.describe(v.legal[row])}: {why}')
        self.stats[ty] += 1
        if ty == D.TRAIN:
            self.stats['train_' + D.UNIT_NAME.get(arg, str(arg))] += 1
        if ty == D.MOVE:
            g = self.ugoal.pop(a, None)
            if g is not None:
                self.ugoal[b] = g
        return int(row)

    def _check_attack(self, v):
        """Compare the last attack's predicted outcome with what we now see (our own view)."""
        at, atype, ahp, tt, ttype, thp, dmg, ret, turn, moved, tter, tcity = self._last_attack
        self._last_attack = None
        if v.turn != turn:
            return
        tu = v.unit_at.get(tt)
        if tu is not None and tu.owner == 2 and tu.type == ttype:
            actual = thp - tu.hp
            au = v.unit_at.get(at)
            aret = ahp - au.hp if (au is not None and au.owner == 1 and au.type == atype) else None
            advanced = False
        elif tu is not None and tu.owner == 1 and tu.type == atype:
            actual, aret, advanced = thp, ahp - tu.hp, True  # killed it and moved in
        else:
            au = v.unit_at.get(at)
            actual, advanced = thp, False
            aret = ahp - au.hp if (au is not None and au.owner == 1) else None
        self.checks.append((dmg, actual, ret, aret, atype, ttype, thp, advanced, moved, tter, tcity))

    def _sync_goals(self, v):
        mine = {u.tile for u in v.my_units}
        for t in list(self.ugoal):
            if t not in mine:
                del self.ugoal[t]

    # ------------------------------------------------------------------ per turn
    def _new_turn(self, v):
        self.turn = v.turn
        self.n_dec = 0
        self.hold = set()
        self.turn_hold_attack = set()
        self.pending = None
        self.retrain_pending = set()
        self.retrain = set()
        self.turn_kinds = collections.Counter()  # roles of the moves made this turn, kind -> count
        self.turn_goals = collections.Counter()  # (kind, goal) -> count
        self.researched = 0
        self._dist_cache = {}
        self._remember(v)
        self.start_stars = v.stars
        self.threat = {c.tile: self.city_threat(v, c) for c in v.my_cities}
        if self.p['cloak_guard'] and (self.mem['seen_types'] & {D.CLOAK, D.DAGGER, D.UNIT_IDX['cloak_boat']}):
            for t in self.threat:
                self.threat[t] = max(self.threat[t], 0.3)  # invisible cloaks infiltrate empty cities
        self.threat_level = max(self.threat.values(), default=0)
        contact = self.mem['contact_turn'] is not None
        frac = self.p['mil_frac_war'] if contact else self.p['mil_frac_early']
        if self.threat_level > 0:
            frac = max(frac, 0.7)
        if self.threat_level >= 2.0:
            frac = 1.0
        self.army_deficit = self.army_target_value(v) - self.army_value(v)
        self.unit_reserve = min(int(v.stars * frac + 0.5), int(max(0, self.army_deficit))) if self.army_deficit > 0 else 0
        self._update_mode(v)
        if self.logging:
            self.log.append(f'== turn {v.turn} stars {v.stars} income {v.income} cities {len(v.my_cities)} '
                            f'units {len(v.my_units)} enemies seen {len(v.enemy_units)} threat {self.threat_level:.1f} '
                            f'reserve {self.unit_reserve} army {self.army_value(v):.0f}/{self.army_target_value(v):.0f} '
                            f'target {D.xy(self.mem["target"]) if self.mem["target"] is not None else None} '
                            f'front {D.xy(self.mem["front"]) if self.mem.get("front") is not None else None} '
                            f'mode {self.mem["mode"]} force {self.mem.get("force")}')

    def _remember(self, v):
        for e in v.enemy_units:
            self.mem['seen_types'].add(e.type)
            if e.passenger >= 0:
                self.mem['seen_types'].add(e.passenger)
        for i in range(D.N):
            if v.owner[i] == 2 and v.imp[i] not in (0, D.CITY, D.RUIN):
                self.mem['seen_imps'].add(int(v.imp[i]))
        if (v.enemy_units or v.enemy_cities) and self.mem['contact_turn'] is None:
            self.mem['contact_turn'] = v.turn
        for c in v.enemy_cities:
            self.mem['enemy_cities'][c.tile] = (v.turn, c.capital)
        for t in list(self.mem['enemy_cities']):
            if v.explored[t] and (t not in v.city_at or v.city_at[t].owner != 2):
                del self.mem['enemy_cities'][t]

    def enemy_techs(self, v):
        key = ('et', v.turn)
        r = self._dist_cache.get(key)
        if r is not None:
            return r
        techs = set(D.TRIBE_START_TECH.get(v.opp_tribe, {0}))
        for t in self.mem['seen_types']:
            if t in D.UNIT_TECH:
                techs.add(D.UNIT_TECH[t])
        for i in self.mem['seen_imps']:
            if i in D.IMP_TECH:
                techs.add(D.IMP_TECH[i])
        if v.turn >= 6:  # pessimistic for our kill planning: the defence techs are cheap and common
            techs |= {D.T['archery'], D.T['climbing']}
        for t in list(techs):
            p = D.TECH_PARENT.get(t)
            while p is not None and p not in techs:
                techs.add(p)
                p = D.TECH_PARENT.get(p)
        self._dist_cache[key] = techs
        return techs

    def city_threat(self, v, c):
        th = 0.0
        rad = 4 if c.capital else 3
        for e in v.enemy_units:
            if e.st.atk <= 0 and e.type != D.RAFT:
                continue
            d = D.dist(e.tile, c.tile)
            if d <= rad:
                w = {0: 2.0, 1: 1.0, 2: 0.7, 3: 0.35, 4: 0.2}[d]
                th += w * (D.unit_value(e.land_type if e.naval else e.type) / 3.0)
        return th

    def city_threat_at_reward(self, v):
        return max((self.city_threat(v, c) for c in v.my_cities), default=0)

    def army_value(self, v):
        return sum(D.unit_value(u.type) * u.hp / u.maxhp for u in v.my_units if u.st.atk > 0)

    def army_target_value(self, v):
        t = self.p['army_base'] + self.p['army_per_turn'] * v.turn
        if self.threat_level > 0:
            t += 4 + 2 * self.threat_level
        visible = sum(D.unit_value(e.type) for e in v.enemy_units)
        return max(t, 1.2 * visible)

    def _strength(self, v, tile, r, mine=True):
        units = v.my_units if mine else v.enemy_units
        return sum(D.unit_value(u.type) * u.hp / u.maxhp for u in units
                   if (u.st.atk > 0 or (u.naval and u.passenger >= 0)) and D.dist(u.tile, tile) <= r)

    def _path(self, v, a, b):
        """Land path a -> b (tile list, a first) following the cost-distance map to b."""
        dm = self.land_dist(v, (b,))
        if dm[a] >= geo.INF:
            return [a, b]
        path, cur = [a], a
        for _ in range(40):
            if cur == b:
                break
            nxt = min(D.NEIGH[cur], key=lambda j: (dm[j], D.dist(j, b)))
            if dm[nxt] >= dm[cur]:
                break
            path.append(nxt)
            cur = nxt
        if path[-1] != b:
            path.append(b)
        return path

    def _update_mode(self, v):
        """The army's front: the tile all army units gather around. It sits on our most threatened
        city when the enemy comes at us; otherwise it walks the path from our capital to the
        target enemy city, one or two steps a turn while we're locally stronger, back when not."""
        tgt = self.army_target(v)
        self.mem['target'] = tgt
        home = v.capital.tile if v.capital else (v.my_cities[0].tile if v.my_cities else None)
        if home is None:
            self.mem['mode'], self.mem['front'] = 'hold', None
            return
        # defence first
        worst, worst_need = None, 0.0
        for c in v.my_cities:
            en = self._strength(v, c.tile, 3, mine=False)
            if en <= 0:
                continue
            own = self._strength(v, c.tile, 3, mine=True)
            need = en - 0.8 * own + (3 if c.capital else 0)
            if c.capital:
                need *= 1.5
            if need > worst_need:
                worst, worst_need = c, need
        if worst is not None and worst_need >= self.p['defend_need']:
            self.mem['mode'], self.mem['front'] = 'defend', worst.tile
            self.mem['force'] = (round(self._strength(v, worst.tile, 2), 1), round(worst_need, 1))
            return
        if tgt is None:
            self.mem['mode'], self.mem['front'] = 'hold', home
            return
        path = self._path(v, home, tgt)
        prev = self.mem.get('front')
        if prev is None or self.mem.get('mode') == 'defend' or self.mem.get('front_target') != tgt:
            k = min(len(path) - 1, 2)
        else:
            k = min(range(len(path)), key=lambda i: D.dist(path[i], prev))
        my_s = self._strength(v, path[k], 2)
        nxt = min(len(path) - 1, k + 2)
        en_next = self._strength(v, path[nxt], 3, mine=False)
        c = v.city_at.get(tgt)
        if D.dist(path[nxt], tgt) <= 2:
            en_next += 2 + (c.level if c else 1)
        en_here = self._strength(v, path[k], 3, mine=False)
        mode = 'hold'
        if my_s >= self.p['advance_ratio'] * max(en_next, 1.0) and my_s >= self.p['advance_min']:
            k = min(len(path) - 1, k + (2 if en_next == 0 else 1))
            mode = 'advance'
        elif my_s < 0.8 * en_here:
            k = max(0, k - 1)
            mode = 'retreat'
        if D.dist(path[k], tgt) <= 1:
            mode = 'assault'
        self.mem['mode'], self.mem['front'], self.mem['front_target'] = mode, path[k], tgt
        self.mem['force'] = (round(my_s, 1), round(en_next, 1))

    # ------------------------------------------------------------------ helpers
    def danger(self, v):
        key = ('danger', tuple(sorted((e.tile, e.hp) for e in v.enemy_units)))
        d = self._dist_cache.get(key)
        if d is None:
            d = tactics.Danger(v, v.techs)
            self._dist_cache[key] = d
        return d

    def land_cost(self, v):
        key = ('landcost', int(v.explored.sum()), D.T['climbing'] in v.techs)
        c = self._dist_cache.get(key)
        if c is None:
            c = geo.land_cost_array(v, D.T['climbing'] in v.techs)
            self._dist_cache[key] = c
        return c

    def land_dist(self, v, goal_tiles):
        key = ('land', tuple(goal_tiles), int(v.explored.sum()))
        d = self._dist_cache.get(key)
        if d is None:
            d = geo.dijkstra(list(goal_tiles), self.land_cost(v), roads=v.road | (v.imp == D.CITY))
            self._dist_cache[key] = d
        return d

    def is_garrison(self, v, u):
        c = v.city_at.get(u.tile)
        return c is not None and c.owner == 1 and self.threat.get(c.tile, 0) > 0

    @staticmethod
    def garrison_quality(u, wall=False):
        """How well a unit holds a city: defence x city bonus (fortify units only) x health."""
        df = u.st.df * ((4.0 if wall else 1.5) if 'fortify' in u.st.abil else 1.0)
        return df * u.hp / 100.0

    def garrison_cost(self, v, u):
        c = v.city_at.get(u.tile)
        if c is None:
            return 0
        th = self.threat.get(c.tile, 0)
        return min(25.0 if c.capital else 10.0, 3.0 + 2.0 * th)

    # ------------------------------------------------------------------ phases
    def ph_promote(self, v):
        for t, row in v.promote.items():
            return self._do(v, row, 'promote veteran')
        return None

    def ph_urgent_train(self, v):
        """A threatened city without a garrison trains a defender now, before anything else."""
        for t in list(self.retrain_pending):
            c = v.city_at.get(t)
            if c is None or t not in v.trains:
                continue
            row, ut = self.pick_unit(v, c, role='defend')
            if row is not None:
                self.retrain_pending.discard(t)
                return self._do(v, row, f'rotation: fresh {D.UNIT_NAME[ut]} garrisons {c}')
        for c in v.my_cities:
            th = self.threat.get(c.tile, 0)
            if th < 1.0 or c.tile not in v.trains:
                continue
            if any(c.tile in v.moves.get(u.tile, {}) for u in v.my_units if not u.moved and not u.naval):
                continue
            row, ut = self.pick_unit(v, c, role='defend')
            if row is not None:
                return self._do(v, row, f'urgent garrison {D.UNIT_NAME[ut]} in {c} threat {th:.1f}')
        return None

    def ph_research(self, v):
        if self.researched >= 1:
            return None
        budget = v.stars - self.unit_reserve
        r = econ.choose_research(self, v, budget)
        if r is None:
            return None
        row, tech, cost = r
        self.researched += 1
        return self._do(v, row, f'research {D.TECH_NAME[tech]} cost {cost} (budget {budget})')

    def ph_research_late(self, v):
        if self.researched >= 3:
            return None
        r = econ.choose_research(self, v, v.stars - max(0, self.unit_reserve // 2))
        if r is None:
            return None
        row, tech, cost = r
        self.researched += 1
        return self._do(v, row, f'research (late) {D.TECH_NAME[tech]} cost {cost}')

    def ph_capture(self, v):
        for t, row in v.captures.items():
            return self._do(v, row, f'capture at {D.xy(t)}')
        return None

    def ph_ruins(self, v):
        for t, row in v.examine.items():
            return self._do(v, row, f'examine ruins {D.xy(t)}')
        return None

    def ph_pending(self, v):
        if self.pending is None:
            return None
        tile, target = self.pending
        self.pending = None
        row = v.attacks.get(tile, {}).get(target)
        if row is not None:
            return self._do(v, row, f'attack after move {D.xy(tile)} -> {D.xy(target)}')
        return None

    def ph_snipe(self, v):
        """Walk into the enemy capital when it's empty: capturing it next turn wins the game."""
        ec = v.enemy_capital
        if ec is None or v.unit_at.get(ec.tile) is not None:
            return None
        danger = self.danger(v)
        best = None
        for u in v.my_units:
            row = v.moves.get(u.tile, {}).get(ec.tile)
            if row is None or u.naval and not v.land(ec.tile):
                continue
            if self.is_garrison(v, u) and v.capital is not None and u.tile == v.capital.tile:
                continue
            dmg, n = danger.damage(ec.tile, u.type, u.hp, u.maxhp, exclude=())
            survive = dmg < u.hp
            sc = (2 if survive else 0) + u.hp / u.maxhp + u.st.df / 30
            if best is None or sc > best[0]:
                best = (sc, row, u, survive)
        if best is None:
            return None
        return self._do(v, best[1], f'snipe: {best[2]} into the empty enemy capital (survives: {best[3]})')

    def ph_attack(self, v):
        if not v.enemy_units:
            return None
        danger = self.danger(v)
        plan = tactics.plan_attacks(self, v, danger)
        if plan is None or plan.score < self.p['attack_min']:
            return None
        o, dmg, ret = plan.steps[0]
        why = f'{plan.why} step 1/{len(plan.steps)}: {o.unit} dmg {dmg} ret {ret} score {plan.score:.1f}'
        if o.move_row is not None:
            self.pending = (o.tile, o.target.tile)
            self.hold.add(o.tile)
            return self._do(v, o.move_row, 'move to attack: ' + why)
        row = v.attacks[o.unit.tile][o.target.tile]
        self._last_attack = (o.unit.tile, o.unit.type, o.unit.hp, o.target.tile, o.target.type, o.target.hp, dmg, ret,
                             v.turn, o.unit.moved, int(v.terrain[o.target.tile]), o.target.tile in v.city_at)
        if self.logging:
            self.mem.setdefault('attack_log', []).append((v.turn, o.unit.type, o.unit.hp, o.target.type, o.target.hp,
                                                          dmg, ret, o.unit.tile, o.target.tile))
        return self._do(v, row, why)

    # ---- builds
    def ph_build_hi(self, v):
        budget = v.stars - self.unit_reserve
        ratio = self.p['build_ratio_hi'] if self.threat_level < 1.0 else 1.25
        b = econ.choose_build(self, v, budget, ratio)
        if b is None:
            return None
        row, imp, tile, cost, val = b
        return self._do(v, row, f'build {D.IMP_NAME[imp]} at {D.xy(tile)} value {val:.1f} cost {cost}')

    def ph_build_lo(self, v):
        b = econ.choose_build(self, v, v.stars, self.p['build_ratio_lo'])
        if b is None:
            return None
        row, imp, tile, cost, val = b
        return self._do(v, row, f'build (late) {D.IMP_NAME[imp]} at {D.xy(tile)} value {val:.1f} cost {cost}')

    def ph_spend(self, v):
        """Never bank much: with stars left over, train anything useful, then research anything."""
        cap = self.p['bank_cap_base'] + v.turn // 3
        if v.stars <= cap:
            return None
        for c in v.my_cities:
            if c.tile in v.trains:
                row, ut = self.pick_unit(v, c, 'army', no_wait=True)
                if row is not None:
                    return self._do(v, row, f'spend: train {D.UNIT_NAME[ut]} at {c} (stars {v.stars} > {cap})')
        if self.researched < 6 and v.research:
            lit = D.T['philosophy'] in v.techs
            n = max(1, len(v.my_cities))
            sc = econ.research_scores(self, v)
            best = max(v.research, key=lambda t: sc.get(t, -9) - 0.2 * D.tech_cost(D.TECH_TIER.get(t, 1), n, lit))
            cost = D.tech_cost(D.TECH_TIER.get(best, 1), n, lit)
            if cost <= v.stars:
                self.researched += 1
                return self._do(v, v.research[best], f'spend: research {D.TECH_NAME[best]} cost {cost}')
        b = econ.choose_build(self, v, v.stars, 0.05)
        if b is not None:
            row, imp, tile, cost, val = b
            return self._do(v, row, f'spend: build {D.IMP_NAME[imp]} at {D.xy(tile)}')
        return None

    # ---- training
    def pick_unit(self, v, city, role='army', no_wait=False):
        opts = v.trains.get(city.tile, {})
        if not opts:
            return None, None
        stars = v.stars
        have = collections.Counter(u.type for u in v.my_units)
        n = max(1, len(v.my_units))
        if role == 'defend':
            if city.wall or city.capital or have[D.DEFENDER] < 2:
                order = (D.DEFENDER, D.SWORDSMAN, D.ARCHER, D.WARRIOR, D.RIDER)
            else:
                order = (D.SWORDSMAN, D.ARCHER, D.DEFENDER, D.WARRIOR, D.RIDER)
            for ut in order:
                if ut in opts and D.stats(ut).cost <= stars:
                    return opts[ut], ut
            return None, None
        if role == 'expand':
            for ut in (D.RIDER, D.WARRIOR):
                if ut in opts and D.stats(ut).cost <= stars:
                    return opts[ut], ut
            return None, None
        ranged = (have[D.ARCHER] + have[D.CATAPULT]) / n
        w = {}
        p = self.p
        if D.SWORDSMAN in opts:
            w[D.SWORDSMAN] = p['w_swordsman']
        if D.CATAPULT in opts:
            w[D.CATAPULT] = p['w_catapult'] if ranged < p['ranged_share'] else 0.5
        if D.ARCHER in opts:
            w[D.ARCHER] = p['w_archer'] if ranged < p['ranged_share'] + 0.05 else 0.6
        if D.KNIGHT in opts:
            w[D.KNIGHT] = p['w_knight'] if have[D.KNIGHT] < 0.3 * n else 0.8
        if D.RIDER in opts:
            w[D.RIDER] = p['w_rider'] if have[D.RIDER] < 3 else 0.9
        if D.DEFENDER in opts:
            w[D.DEFENDER] = p['w_defender']
        if D.WARRIOR in opts:
            w[D.WARRIOR] = 1.0 if v.turn < 12 else 0.3
        if D.MINDBENDER in opts:
            w[D.MINDBENDER] = 0.2
        for ut, wt in sorted(w.items(), key=lambda kv: -kv[1]):
            cost = D.stats(ut).cost
            if cost <= stars:
                return opts[ut], ut
            if not no_wait and wt >= 2.4 and cost - stars <= max(2, v.income // 2):
                return None, None  # save for it: next turn
        return None, None

    def ph_train(self, v):
        if not v.trains:
            return None
        deficit = self.army_target_value(v) - self.army_value(v)
        expand = self._unserved_villages(v)
        retrain = getattr(self, 'retrain', set())
        for c in sorted(v.my_cities, key=lambda c: (c.tile not in retrain, -self.threat.get(c.tile, 0),
                                                     self._front_dist(v, c))):
            if c.tile not in v.trains:
                continue
            th = self.threat.get(c.tile, 0)
            if c.tile in retrain:
                row, ut = self.pick_unit(v, c, 'defend')
                if row is not None:
                    return self._do(v, row, f'train {D.UNIT_NAME[ut]} as new garrison of {c}')
            if th > 0:
                row, ut = self.pick_unit(v, c, 'defend' if th >= 1.5 else 'army')
                if row is not None:
                    return self._do(v, row, f'train {D.UNIT_NAME[ut]} at threatened {c} ({th:.1f})')
            if expand and v.turn < 15:
                row, ut = self.pick_unit(v, c, 'expand')
                if row is not None:
                    expand -= 1
                    return self._do(v, row, f'train {D.UNIT_NAME[ut]} to expand at {c}')
            if deficit <= 0:
                continue
            row, ut = self.pick_unit(v, c, 'army')
            if row is not None:
                return self._do(v, row, f'train {D.UNIT_NAME[ut]} (army {deficit:.0f} short) at {c}')
        return None

    def _front_dist(self, v, c):
        tgt = self.mem.get('target')
        return D.dist(c.tile, tgt) if tgt is not None else 0

    def _unserved_villages(self, v):
        """Known villages no unit of ours is heading for or standing on."""
        goals = {g for (k, g) in self.ugoal.values() if k == 'village'}
        mine = {u.tile for u in v.my_units}
        return sum(1 for t in v.villages if t not in mine and t not in goals)

    # ---- boats
    def ph_upgrade(self, v):
        for t, ups in v.upgrades.items():
            for ut in (D.RAMMER, D.SCOUTSHIP, D.BOMBER):
                if ut in ups:
                    cost = 15 if ut == D.BOMBER else 5
                    if v.stars - cost >= 0:
                        return self._do(v, ups[ut], f'upgrade boat at {D.xy(t)} to {D.UNIT_NAME[ut]}')
        return None

    def ph_heal(self, v):
        for t, row in v.heal.items():
            if any(u.hp < u.maxhp for u in v.my_units if D.dist(u.tile, t) == 1):
                return self._do(v, row, 'heal others')
        return None

    def ph_recover(self, v):
        for u in v.my_units:
            if u.tile in v.recover and u.hp < u.maxhp:
                return self._do(v, v.recover[u.tile], f'recover {u}')
        return None

    # ------------------------------------------------------------------ movement
    def ph_moves(self, v):
        fresh = [u for u in v.my_units if u.tile in v.moves and not u.moved and u.tile not in self.hold]
        if not fresh:
            return None
        tasks = self.assign(v, fresh)
        order = sorted(fresh, key=lambda u: -tasks[u.tile][0])
        for u in order:
            prio, kind, goal = tasks[u.tile]
            old_goal = self.ugoal.get(u.tile)
            if not (kind == 'explore' and goal is None and old_goal and old_goal[0] == 'explore'):
                self.ugoal[u.tile] = (kind, goal)
            r = self.move_toward(v, u, kind, goal)
            if r is None:
                self.hold.add(u.tile)
                continue
            row, why = r
            self.hold.add(int(v.legal[row][2]))
            if kind == 'vacate' and goal in self.retrain:
                self.retrain_pending.add(goal)
            self.turn_kinds[kind] += 1
            self.turn_goals[(kind, goal)] += 1
            return self._do(v, row, f'{kind}: {u} -> {D.xy(int(v.legal[row][2]))} {why}')
        return None

    def assign(self, v, fresh):
        """Task per fresh unit: (priority, kind, goal tile or None)."""
        tasks = {}
        free = {u.tile: u for u in fresh if u.type != D.EXPLORER_UNIT}
        for u in fresh:
            if u.type == D.EXPLORER_UNIT:
                tasks[u.tile] = (0, 'explore', None)
        self.retrain = set()
        spare = v.stars - sum(self._defender_cost(v) for _ in self.retrain_pending)
        danger = self.danger(v)
        # 0. capital safety: a garrison the visible enemies could kill steps out when a unit that
        #    would survive there can step in (next decision assigns it as the garrison).
        cap = v.capital
        if cap is not None and self.threat.get(cap.tile, 0) > 0:
            g = v.unit_at.get(cap.tile)
            if g is not None and g.owner == 1 and g.tile in free and danger.kills(cap.tile, g.type, g.hp, g.maxhp):
                for u in free.values():
                    if u is g or u.naval or D.dist(u.tile, cap.tile) > max(1, u.st.mov):
                        continue
                    if not danger.kills(cap.tile, u.type, u.hp, u.maxhp) and \
                            self.garrison_quality(u, cap.wall) > 1.3 * self.garrison_quality(g, cap.wall):
                        tasks[g.tile] = (12, 'vacate', cap.tile)
                        del free[g.tile]
                        self.mem['swap_in'] = (v.turn, u.tile)
                        break
        # 1. garrisons for threatened cities. Rotation: the occupant steps out next to the city when
        #    the city can train a fresh unit this turn (it becomes the new garrison).
        for c in sorted(v.my_cities, key=lambda c: (not c.capital, -self.threat.get(c.tile, 0))):
            th = self.threat.get(c.tile, 0)
            if th <= 0:
                continue
            occ = v.unit_at.get(c.tile)
            if occ is not None and occ.owner == 1:
                if occ.tile in free:
                    unit_cost = self._defender_cost(v)
                    weak = occ.hp < 0.75 * occ.maxhp or 'fortify' not in occ.st.abil or occ.st.df < 20
                    rich = spare >= self.p['bank_cap_base'] + v.turn // 3 + unit_cost
                    if (weak or rich) and spare >= unit_cost and self._city_can_train(v, c) and \
                            self._has_step_out(v, occ):
                        self.retrain.add(c.tile)
                        spare -= unit_cost
                        tasks[occ.tile] = (12, 'vacate', c.tile)
                    else:
                        tasks[occ.tile] = (9, 'garrison-hold', c.tile)
                    del free[occ.tile]
            elif c.tile not in self.retrain_pending:
                cands = [u for u in free.values() if not u.naval and c.tile in v.moves.get(u.tile, {})]
                if cands:
                    cands.sort(key=lambda u: -self.garrison_quality(u, c.wall))
                    u = cands[0]
                    tasks[u.tile] = (10, 'garrison', c.tile)
                    del free[u.tile]
        # 1b. capital defence: recall the army when the enemy near the capital outweighs our units there
        if cap is not None:
            en_s = sum(D.unit_value(e.type) * e.hp / e.maxhp for e in v.enemy_units
                       if (e.st.atk > 0 or e.passenger >= 0) and D.dist(e.tile, cap.tile) <= 3)
            my_s = sum(tactics.my_value(u) * u.hp / u.maxhp for u in v.my_units
                       if u.st.atk > 0 and D.dist(u.tile, cap.tile) <= 2 and u.tile not in free)
            if en_s > 0 and en_s >= self.p['cap_defend_ratio'] * my_s:
                need = 1.4 * en_s - my_s
                dm = self.land_dist(v, (cap.tile,))
                for u in sorted(free.values(), key=lambda u: dm[u.tile]):
                    if need <= 0 or dm[u.tile] > self.p['cap_defend_radius']:
                        break
                    if u.naval or u.st.atk <= 0:
                        continue
                    tasks[u.tile] = (8.5, 'defend', cap.tile)
                    del free[u.tile]
                    if D.dist(u.tile, cap.tile) > 2:
                        need -= tactics.my_value(u) * u.hp / u.maxhp
                    else:
                        need -= 0.5 * tactics.my_value(u) * u.hp / u.maxhp
        # 2. hunt invaders: enemy units on our land or near our cities
        invaders = []
        for e in v.enemy_units:
            if e.st.atk <= 0 and not (e.naval and e.passenger >= 0):
                continue
            dc = min((D.dist(e.tile, c.tile) for c in v.my_cities), default=99)
            if dc <= 2 or v.owner[e.tile] == 1:
                invaders.append((dc, e))
        invaders.sort(key=lambda x: x[0])
        for dc, e in invaders:
            k = (1 if D.unit_value(e.type) <= 3 else 2) - self.turn_goals[('hunt', e.tile)]
            if k <= 0:
                continue
            dm = self.land_dist(v, (e.tile,))
            near = sorted((u for u in free.values() if not u.naval and u.st.atk > 0 and dm[u.tile] <= 5),
                          key=lambda u: dm[u.tile])
            for u in near[:k]:
                tasks[u.tile] = (8, 'hunt', e.tile)
                del free[u.tile]
        # 3. villages and ruins: keep goals from earlier turns when still valid
        occupied_by_me = {u.tile for u in v.my_units}
        open_targets = {t: 'village' for t in v.villages if t not in occupied_by_me}
        for t in v.ruins:
            if t not in occupied_by_me:
                open_targets[t] = 'ruin'
        taken = set()
        for ut, u in list(free.items()):
            g = self.ugoal.get(ut)
            if g and g[0] in ('village', 'ruin') and g[1] in open_targets and g[1] not in taken:
                tasks[ut] = (7 if g[0] == 'village' else 5, g[0], g[1])
                taken.add(g[1])
                del free[ut]
        pairs = []
        for t, kind in open_targets.items():
            if t in taken:
                continue
            dm = self.land_dist(v, (t,))
            w = 1.0 if kind == 'village' else 0.45
            for u in free.values():
                if u.naval:
                    continue
                d = dm[u.tile]
                if d >= geo.INF:
                    continue
                turns = d / max(1, u.st.mov)
                if turns > self.p['village_turns'] * (1 if kind == 'village' else 0.5):
                    continue
                pairs.append((turns / w, t, kind, u.tile))
        pairs.sort()
        for sc, t, kind, ut in pairs:
            if ut not in free or t in taken:
                continue
            taken.add(t)
            tasks[ut] = (7 if kind == 'village' else 5, kind, t)
            del free[ut]
        # 4. wounded units out of a fight go home to heal
        for ut, u in list(free.items()):
            if u.hp < 0.5 * u.maxhp and not u.naval and u.type != D.GIANT:
                home = min((c.tile for c in v.my_cities if self.threat.get(c.tile, 0) < 3 and
                            (c.tile not in v.unit_at or c.tile == ut)), key=lambda t: D.dist(t, ut), default=None)
                if home is not None:
                    tasks[ut] = (6, 'heal', home)
                    del free[ut]
        # 5. exploration (early, or while no enemy city is known) and the army
        target = self.mem['target']
        max_exp = self.p['explorers'] + (1 if target is None else 0)
        if self.mem.get('mode') == 'defend':
            max_exp = min(max_exp, 1)
        cap_known = any(ic for (_, ic) in self.mem['enemy_cities'].values())
        if v.turn > self.p['explore_until'] and target is not None:
            max_exp = 0 if cap_known else self.p['capital_scouts']
        explorers = self.turn_kinds['explore']
        for ut, u in sorted(free.items(), key=lambda kv: (0 if (self.ugoal.get(kv[0]) or ('',))[0] == 'explore' else 1,
                                                           -kv[1].st.mov)):
            if explorers < max_exp and not u.naval:
                tasks[ut] = (3, 'explore', None)
                explorers += 1
            elif self.mem.get('front') is not None and (target is not None or self.mem['mode'] == 'defend'):
                tasks[ut] = (4, 'army', self.mem['front'])
            elif explorers < max_exp + 1 and not u.naval:
                tasks[ut] = (2, 'explore', None)
                explorers += 1
            else:
                tasks[ut] = (1, 'army', self.mem.get('front') or (v.capital.tile if v.capital else ut))
        return tasks

    def _defender_cost(self, v):
        for ut in (D.DEFENDER, D.SWORDSMAN, D.ARCHER, D.WARRIOR):
            if ut == D.WARRIOR or D.UNIT_TECH.get(ut) in v.techs:
                return D.stats(ut).cost
        return 2

    def _has_step_out(self, v, u):
        return any(D.dist(d, u.tile) == 1 and v.land(d) for d in v.moves.get(u.tile, {}))

    def _city_can_train(self, v, c):
        homes = sum(1 for u in v.my_units if u.home == c.tile)
        return homes < c.level + 1

    def army_target(self, v):
        cities = list(self.mem['enemy_cities'].items())
        if not cities:
            return None
        ref = [c.tile for c in v.my_cities] or [u.tile for u in v.my_units]
        best = None
        for t, (seen, is_cap) in cities:
            dm = self.land_dist(v, (t,))
            d = min((dm[r] for r in ref), default=geo.INF)
            if d >= geo.INF:
                d = 40 + min((D.dist(t, r) for r in ref), default=0)
            sc = d - (self.p['capital_bias'] if is_cap else 0)
            if best is None or sc < best[0]:
                best = (sc, t)
        return best[1]

    def move_toward(self, v, u, kind, goal):
        """(row, why) for unit u's move this turn, or None to stay."""
        moves = v.moves.get(u.tile, {})
        if not moves:
            return None
        if kind == 'garrison':
            row = moves.get(goal)
            return (row, 'into threatened city') if row is not None else None
        if kind == 'garrison-hold':
            return None
        if kind == 'vacate':
            danger = self.danger(v)
            best = None
            for d, row in moves.items():
                if not v.land(d):
                    continue
                dmg, n = danger.damage(d, u.type, u.hp, u.maxhp)
                sc = -dmg / max(1, u.hp) * 5 - (5 if D.dist(d, goal) > 1 else 0)
                if (v.terrain[d] == D.FOREST and D.T['archery'] in v.techs) or \
                        (v.terrain[d] == D.MOUNTAIN and D.T['climbing'] in v.techs):
                    sc += 0.5
                if best is None or sc > best[0]:
                    best = (sc, row)
            return (best[1], 'make room for a stronger garrison') if best else None
        if kind == 'heal' and (u.tile == goal or v.owner[u.tile] == 1) and \
                not self.danger(v).kills(u.tile, u.type, u.hp, u.maxhp):
            return None  # safe in our land: stay and recover
        if u.naval:
            return self._move_boat(v, u, kind, goal)
        danger = self.danger(v)
        if kind == 'explore':
            goal = self.explore_goal(v, u)
            if goal is None:
                return None
            self.ugoal[u.tile] = ('explore', goal)
        dm = self.land_dist(v, (goal,))
        if kind == 'army' and self.p['naval_transport']:
            row = self._embark(v, u, goal, dm)
            if row is not None:
                return row, 'embark: sailing is shorter'
        mode = self.mem['mode'] if kind == 'army' else None
        dwt = {'assault': self.p['assault_danger'], 'advance': 0.7, 'defend': 0.5}.get(mode, 1.0) * self.p['danger_scale']
        tgt_city = self.mem.get('target')
        if kind == 'hunt' and v.capital is not None and D.dist(goal, v.capital.tile) <= 2:
            dwt = 0.3
        if kind == 'defend':
            dwt = 0.5
        ranged = u.st.rng >= 2
        best = None
        cur = u.tile
        for d, row in list(moves.items()) + [(cur, None)]:
            if d != cur and v.terrain[d] in (D.WATER, D.OCEAN):
                continue  # no embarking (v1)
            dd = dm[d] if dm[d] < geo.INF else 60.0
            if kind == 'army':
                sc = -max(0.0, dd - 1.5) * 1.5
            else:
                sc = -dd
            if kind == 'explore':
                sc += 0.25 * self._reveal(v, d)
            dmg, n = danger.damage(d, u.type, u.hp, u.maxhp)
            if n:
                if dmg >= u.hp:
                    pen = 4.0 + tactics.my_value(u)
                    if kind == 'village' and d == goal:
                        pen *= 0.4
                    sc -= pen * dwt
                else:
                    sc -= 3.0 * dmg / u.maxhp * dwt
            if ranged and any(D.dist(d, e.tile) == 1 for e in v.enemy_units if e.st.atk > 0):
                sc -= 2.0
            if kind == 'army' and tgt_city is not None and d == tgt_city and v.unit_at.get(d) is None:
                sc += 25 if mode == 'assault' else 12  # walk into an empty enemy city
            if kind == 'army' and d in v.city_at and v.city_at[d].owner == 2 and v.unit_at.get(d) is None:
                sc += 10  # any empty enemy city on the way
            if kind == 'hunt':
                ddg = D.dist(d, goal)
                if ddg <= u.st.rng:
                    sc += 1.5  # can strike it next turn from here
            if kind == 'defend':
                dc = D.dist(d, goal)
                sc = -max(0, dc - 1) * 2.0 - (dm[d] if dm[d] < geo.INF else 60) * 0.2 + sc * 0.0 - \
                    (0 if not n else (pen_for(dmg, u) * dwt))
                if dc == 0 and v.unit_at.get(goal) is None:
                    sc += 3.0  # empty capital: get in
            if (v.terrain[d] == D.FOREST and D.T['archery'] in v.techs) or \
                    (v.terrain[d] == D.MOUNTAIN and D.T['climbing'] in v.techs):
                sc += 0.3
            if d in v.city_at and v.city_at[d].owner == 1 and d != cur and kind != 'defend':
                sc -= 0.6  # don't clog our own city tiles (training)
            if d == cur and cur in v.city_at and v.city_at[cur].owner == 1 and kind in ('army', 'explore', 'village'):
                sc -= 0.6
            if best is None or sc > best[0]:
                best = (sc, d, row)
        if best is None or best[2] is None:
            return None
        return best[2], f'(score {best[0]:.1f})'

    def _unused(self):
        pass

    def explore_goal(self, v, u):
        """A far tile in the largest unexplored region, kept across turns until it's explored."""
        g = self.ugoal.get(u.tile)
        if g and g[0] == 'explore' and g[1] is not None:
            t = g[1]
            if not v.explored[t] or sum(1 for j in D.within(t, 2) if not v.explored[j]) >= 4:
                return t
        taken = {gg[1] for tt, gg in self.ugoal.items() if gg[0] == 'explore' and tt != u.tile and gg[1] is not None}
        cap_known = any(ic for (_, ic) in self.mem['enemy_cities'].values())
        dm = self.land_dist(v, (u.tile,))
        cap = v.capital.tile if v.capital else u.tile
        post = None
        if not cap_known:
            # where the enemy capital may still be: the map prior, minus what we've seen
            if self.mem.get('cap_prior') is None:
                self.mem['cap_prior'] = D.enemy_capital_prior(cap)
            post = self.mem['cap_prior'] * (~v.explored)
            if post.sum() > 0:
                post = post / post.sum()
        best = None
        for t in range(D.N):
            if v.explored[t] and v.terrain[t] not in (D.FIELD, D.FOREST, D.MOUNTAIN):
                continue
            if dm[t] >= geo.INF:
                continue
            n_un = sum(1 for j in D.within(t, 2) if not v.explored[j])
            if n_un < 4:
                continue
            if any(D.dist(t, x) <= 3 for x in taken):
                continue
            toward = 0.0
            if not cap_known:
                toward = self.p['capital_prior_w'] * sum(post[j] for j in D.within(t, 2)) + post[t]
                if any(v.owner[j] == 2 for j in D.within(t, 2)):
                    toward += 1.5  # enemy land: its capital is near
            sc = 0.5 * n_un - 0.45 * dm[t] + toward
            if best is None or sc > best[0]:
                best = (sc, t)
        return best[1] if best else None

    def _reveal(self, v, d):
        r = 2 if v.terrain[d] == D.MOUNTAIN else 1
        return sum(1 for j in D.within(d, r) if not v.explored[j])

    def water_dist(self, v, goal):
        """Boat cost-distance to the water next to `goal` (rafts use shallow water; ocean with Sailing)."""
        key = ('water', goal, int(v.explored.sum()), D.T['sailing'] in v.techs)
        d = self._dist_cache.get(key)
        if d is None:
            cost = geo.water_cost_array(v, D.T['sailing'] in v.techs)
            src = [j for j in D.within(goal, 1) if cost[j] < geo.INF and not v.land(j)] or \
                  [j for j in D.within(goal, 2) if cost[j] < geo.INF and not v.land(j)]
            d = geo.dijkstra(src, cost) if src else __import__('numpy').full(D.N, geo.INF)
            self._dist_cache[key] = d
        return d

    def _embark(self, v, u, goal, dm):
        """Row of a move onto one of our ports when sailing to `goal` beats walking, else None."""
        if goal is None or u.naval or dm[u.tile] < self.p['embark_min']:
            return None
        wd = self.water_dist(v, goal)
        best = None
        for d, row in v.moves.get(u.tile, {}).items():
            if v.imp[d] == D.PORT and v.owner[d] == 1 and wd[d] < geo.INF:
                route = wd[d] / 2.0 + 1.5  # rafts move 2; a turn to embark and one to land
                walk = dm[u.tile] / max(1, u.st.mov)
                if route + 1 < walk and (best is None or wd[d] < best[0]):
                    best = (wd[d], row)
        return best[1] if best else None

    def _move_boat(self, v, u, kind, goal):
        moves = v.moves.get(u.tile, {})
        target = self.mem.get('front') or self.mem.get('target') or goal
        if target is None:
            return None
        wd = self.water_dist(v, target)
        danger = self.danger(v)
        land_type = u.passenger if u.passenger >= 0 else D.WARRIOR
        best = None
        for d, row in list(moves.items()) + [(u.tile, None)]:
            if v.land(d):
                dt = D.dist(d, target)
                if dt > 2:
                    continue
                sc = 8.0 - 2.0 * dt
                dmg, n = danger.damage(d, land_type, u.hp, u.maxhp)
            else:
                sc = -(wd[d] if wd[d] < geo.INF else 40.0)
                dmg, n = danger.damage(d, u.type, u.hp, u.maxhp)
            if n:
                sc -= (6.0 if dmg >= u.hp else 3.0 * dmg / u.maxhp) * (0.5 if u.st.atk > 0 else 1.0)
            if best is None or sc > best[0]:
                best = (sc, row, d)
        if best is None or best[1] is None:
            return None
        return best[1], f'boat (score {best[0]:.1f})'
