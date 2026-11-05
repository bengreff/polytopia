"""A tactical search for the side to move: a beam search over attack sequences, each step played by
the game's own rules on a copy of the position (save, load, step), so every outcome is the engine's.

Run it only on fog-safe samples (polyenv.Env.determinize) for a planning seat: a sample's hidden part
is drawn from what that seat may know, so reading both players' views of the sample leaks nothing.

A step ("macro") is one of:
- an Attack row in the legal list (this includes a persisting unit's next attack after a kill: a
  Knight that kills moves into the tile and the game lists its next attacks from there);
- a Move then an Attack from the new tile (units with `dash`), one destination per (unit, target):
  the one the danger estimate likes best (for a melee kill the attacker ends on the target's tile
  anyway);
- a Capture row.
The combat formula (rulebot/combat.py, the strategy book's verified one) only ranks and prunes the
steps worth simulating; the engine plays them. The objective is material in stars, from the side to
move: units valued as in crazyplus/evalfn.py (star value x (0.3 + 0.7 x health share), +1.5 for a
veteran), cities as there, mine minus the opponent's, change from the root. A kill therefore counts
the whole unit, damage 70% of its share, and our own losses (retaliation, units killed) count against.
"""
import time

import numpy as np

import polyenv
from crazyplus.evalfn import DEFAULT_W, Side
from crazyplus.tactics import Board
from rulebot import combat as C
from rulebot import data as D

ATTACK, MOVE, CAPTURE, END_TURN = 2, 6, 7, 15
F = {n: i for i, n in enumerate(polyenv.TILE_FEATURES)}
KEY_COLS = [F[n] for n in ('unit', 'unit_owner', 'unit_health', 'unit_moved', 'unit_attacked', 'unit_passenger', 'owner')]
WIN_BONUS = 1000.0

DEFAULTS = dict(
    depth=6,        # macros per sequence
    beam=6,         # nodes kept per depth
    branch=10,      # macros simulated per node (ranked by the estimate)
    min_est=-1.0,   # a macro whose estimate (stars) is below this is not simulated, unless it kills
    min_gain=0.5,   # a sequence must gain at least this many stars to be returned
    top=4,          # sequences returned
    max_sims=400,   # simulations per search (a time cap)
    captures=True,  # Capture rows are steps too
    lookahead=0.5,  # beam ranking: exact gain so far + lookahead x the estimated gain still available
)


def row_of(legal, k):
    return tuple(int(x) for x in legal[k])


def find(st, row):
    """Index of `row` (type, a, b, arg; None = any) in st's legal list, or -1."""
    if not len(st.legal):
        return -1
    m = np.ones(len(st.legal), bool)
    for j, v in enumerate(row):
        if v is not None:
            m &= st.legal[:, j] == v
    hit = np.nonzero(m)[0]
    return int(hit[0]) if len(hit) else -1


class Node:
    __slots__ = ('blob', 'seq', 'obj', 'st', 'board', 'key', 'kills', 'lost', 'gain', 'loss', 'won', 'obs', 'h')

    def __init__(self, blob, seq, obj, st, obs, key, kills=0, lost=0, gain=0.0, loss=0.0, won=False):
        self.blob, self.seq, self.obj, self.st, self.obs, self.key = blob, seq, obj, st, obs, key
        self.kills, self.lost, self.gain, self.loss, self.won = kills, lost, gain, loss, won
        self.board = None


def material(obs_me, obs_op):
    """(my stars, the opponent's stars, my unit count, theirs): units and cities as evalfn values them."""
    a, b = Side(obs_me, DEFAULT_W), Side(obs_op, DEFAULT_W)
    return (float(a.unit_vals.sum() + a.city_vals.sum()), float(b.unit_vals.sum() + b.city_vals.sum()),
            len(a.unit_tiles), len(b.unit_tiles), float(a.unit_vals.sum()), float(b.unit_vals.sum()))


def state_key(obs_me, obs_op):
    return hash((obs_me.tiles[..., KEY_COLS].tobytes(), obs_op.tiles[..., KEY_COLS].tobytes()))


def unit_key(obs_me, obs_op):
    """The units and city owners of both sides (what the tactical search depends on, terrain aside)."""
    return state_key(obs_me, obs_op)


class Tactician:
    """Beam search over attack sequences. `slots`: scratch slots it may overwrite (at least `branch`)."""

    def __init__(self, env, spec, slots, **params):
        self.env, self.spec, self.slots = env, spec, list(slots)
        self.p = dict(DEFAULTS, **params)
        assert len(self.slots) >= self.p['branch']
        self.stats = dict(searches=0, sims=0, seconds=0.0)

    # ---- estimates (pruning only)
    @staticmethod
    def estimate(board, u, frm, e):
        """(estimated stars gained, kill?) of my unit u attacking enemy e from tile frm."""
        if 'convert' in u.st.abil:
            return 0.8 * e.value, True
        if u.st.atk <= 0:
            return -9.0, False
        dmg, ret = board.battle(u, frm, e)
        kill = dmg >= e.hp
        gain = e.value * (1.0 if kill else 0.7 * dmg / e.maxhp)
        loss = u.value * 0.7 * min(ret, u.hp) / u.maxhp + (0.3 * u.value if ret >= u.hp else 0.0)
        return gain - loss, kill

    def potential(self, board):
        """Estimated stars my units can still gain this turn (ranking only): each unit's best hit, from
        where it stands or after a move; for a persisting unit (Knight) that kills, plus the greedy chain
        of further one-hit kills from the target's tile. Lets a softening hit (a catapult on the
        swordsman a knight can't kill yet) outrank a small kill elsewhere."""
        mine = [u for u in board.my_units if not u.attacked and (u.st.atk > 0 or 'convert' in u.st.abil)]
        if not mine or not board.en_units:
            return 0.0
        en = {e.tile: e for e in board.en_units}
        total = 0.0
        for u in mine:
            frms = [u.tile] if not u.moved and 'dash' in u.st.abil else []
            if not u.moved and 'dash' in u.st.abil:
                frms += [m for m in board.moves.get(u.tile, {}) if m not in board.unit_at]
            best = 0.0
            direct = board.attacks.get(u.tile, {})
            seen = set()
            for frm in frms or [u.tile]:
                for t in D.within(frm, u.st.rng):
                    e = en.get(t)
                    if e is None or (t, frm == u.tile) in seen:
                        continue
                    if frm == u.tile and t not in direct:
                        continue
                    seen.add((t, frm == u.tile))
                    est, kill = self.estimate(board, u, frm, e)
                    if kill and 'persist' in u.st.abil:
                        est += self._chain(board, u, t, en, {t})
                    best = max(best, est)
            total += best
        return total

    def _chain(self, board, u, tile, en, killed, depth=8):
        """Greedy chain of one-hit kills (formula) for a persisting unit standing on `tile`."""
        gain = 0.0
        for _ in range(depth):
            nxt = None
            for t in D.within(tile, u.st.rng):
                e = en.get(t)
                if e is None or t in killed:
                    continue
                dmg, _ = board.battle(u, tile, e)
                if dmg >= e.hp and (nxt is None or e.value > nxt[1]):
                    nxt = (t, e.value)
            if nxt is None:
                break
            gain += nxt[1]
            killed.add(nxt[0])
            tile = nxt[0] if u.st.rng == 1 else tile
        return gain

    def macros(self, node):
        """[(priority, rows)]: the steps worth simulating from this node, best first. A move + attack's
        second row is (ATTACK, m, target, None): its arg is matched after the move."""
        board, legal, p = node.board, node.st.legal, self.p
        mine = {u.tile: u for u in board.my_units}
        out = []
        for a, targets in board.attacks.items():
            u = mine.get(a)
            if u is None:
                continue
            for b, k in targets.items():
                e = board.unit_at.get(b)
                if e is None or e.mine:
                    continue
                est, kill = self.estimate(board, u, a, e)
                if kill or est > p['min_est']:
                    out.append((est + (1.5 if kill else 0.0), [row_of(legal, k)]))
        for u in board.my_units:
            if u.moved or u.attacked or 'dash' not in u.st.abil or (u.st.atk <= 0 and 'convert' not in u.st.abil):
                continue
            dests = board.moves.get(u.tile)
            if not dests:
                continue
            best = {}
            for m, km in dests.items():
                if m in board.unit_at:
                    continue
                for e in board.en_units:
                    if D.dist(m, e.tile) > u.st.rng or e.tile in board.attacks.get(u.tile, {}):
                        continue  # (a target it can hit from where it stands is a direct attack)
                    est, kill = self.estimate(board, u, m, e)
                    if not (kill or est > p['min_est']):
                        continue
                    pr = est + (1.5 if kill else 0.0) - 0.3 * board.danger(u, m)
                    if e.tile not in best or pr > best[e.tile][0]:
                        best[e.tile] = (pr, m, km)
            for et, (pr, m, km) in best.items():
                out.append((pr, [row_of(legal, km), (ATTACK, m, et, None)]))
        if p['captures']:
            for k in np.nonzero(legal[:, 0] == CAPTURE)[0] if len(legal) else []:
                t = int(legal[k][1])
                c = board.city.get(t)
                v = 8.0 + 3.0 * (c[1] if c else 1) + (12.0 if c and c[3] else 0.0)
                out.append((v, [row_of(legal, int(k))]))
        out.sort(key=lambda x: -x[0])
        return out[:p['branch']]

    # ---- the search
    def _evaluate(self, node, me):
        obs_me, obs_op = node.obs
        node.board = Board(obs_me, obs_op, node.st.legal)

    def search(self, src_slot, st, deadline=None):
        """Sequences for the side to move in src_slot (Status st). Returns [dict(seq, obj, kills, lost,
        gain, loss, won, blob)], best first (obj >= min_gain), and nothing if no attack is worth it."""
        t0 = time.time()
        env, p = self.env, self.p
        self.stats['searches'] += 1
        if st.ended or not len(st.legal):
            return []
        me = st.current
        opp = 3 - me
        legal = st.legal
        if not ((legal[:, 0] == ATTACK).any() or (legal[:, 0] == MOVE).any() or (legal[:, 0] == CAPTURE).any()):
            return []
        obs = env.observe([(src_slot, me), (src_slot, opp)])
        m0 = material(obs[0], obs[1])
        root = Node(env.save(src_slot), [], 0.0, st, obs, state_key(*obs))
        self._evaluate(root, me)
        if not root.board.en_units and not (p['captures'] and (legal[:, 0] == CAPTURE).any()):
            return []
        visited = {root.key: root}
        beam = [root]
        sims = 0
        for depth in range(p['depth']):
            children = []
            for node in beam:
                if node.st.ended or node.st.current != me:
                    continue
                ms = self.macros(node)
                if not ms:
                    continue
                if sims >= p['max_sims'] or (deadline is not None and time.time() > deadline):
                    break
                ms = ms[:max(0, min(len(ms), p['max_sims'] - sims))]
                sims += len(ms)
                slots = self.slots[:len(ms)]
                sts = {}
                for s in slots:
                    sts[s] = env.load(s, self.spec, node.blob)
                # first rows, batched
                acts, alive = [], {}
                for s, (_, rows) in zip(slots, ms):
                    k = find(sts[s], rows[0])
                    if k >= 0:
                        acts.append((s, k))
                        alive[s] = rows
                for (s, _), (ok, st2) in zip(acts, env.step(acts) if acts else []):
                    sts[s] = st2
                    if not ok:
                        alive.pop(s, None)
                # second rows (the attack after a move)
                acts = []
                for s, rows in list(alive.items()):
                    if len(rows) > 1:
                        st2 = sts[s]
                        k = find(st2, rows[1]) if not st2.ended and st2.current == me else -1
                        if k < 0:
                            alive.pop(s)  # the move worked but the attack isn't there: not a step
                            continue
                        acts.append((s, k))
                done_rows = {}
                for s, rows in alive.items():
                    done_rows[s] = [rows[0]]
                for (s, k), (ok, st2) in zip(acts, env.step(acts) if acts else []):
                    if not ok:
                        alive.pop(s, None)
                        continue
                    done_rows[s].append(row_of(sts[s].legal, k))
                    sts[s] = st2
                live = [s for s in slots if s in alive]
                if not live:
                    continue
                obs_all = env.observe([r for s in live for r in ((s, me), (s, opp))])
                for j, s in enumerate(live):
                    o = (obs_all[2 * j], obs_all[2 * j + 1])
                    key = state_key(*o)
                    if key in visited:
                        continue
                    st2 = sts[s]
                    m1 = material(*o)
                    won = st2.ended and st2.winner == me
                    obj = (m1[0] - m0[0]) - (m1[1] - m0[1]) + (WIN_BONUS if won else 0.0)
                    if st2.ended and not won:
                        obj -= WIN_BONUS
                    child = Node(env.save(s), node.seq + done_rows[s], obj, st2, o, key,
                                 kills=m0[3] - m1[3], lost=m0[2] - m1[2], gain=m0[5] - m1[5], loss=m0[4] - m1[4], won=won)
                    visited[key] = child
                    children.append(child)
            if not children:
                break
            lam = p['lookahead']
            for n in children:
                if not n.st.ended:
                    self._evaluate(n, me)
                n.h = n.obj + (lam * self.potential(n.board) if lam and n.board is not None else 0.0)
            children.sort(key=lambda n: -n.h)
            beam = children[:p['beam']]
        self.stats['sims'] += sims
        self.stats['seconds'] += time.time() - t0
        best = sorted((n for n in visited.values() if n.seq and n.obj >= p['min_gain']), key=lambda n: -n.obj)
        return [dict(seq=n.seq, obj=round(n.obj, 2), kills=int(n.kills), lost=int(n.lost), gain=round(n.gain, 2),
                     loss=round(n.loss, 2), won=n.won, blob=n.blob) for n in best[:p['top']]]


def seq_info(seq):
    """The tiles a sequence uses: (entry tiles: where attackers moved to; attack origins; targets)."""
    entries, origins, targets = [], [], []
    for r in seq:
        if r[0] == MOVE:
            entries.append(int(r[2]))
        elif r[0] == ATTACK:
            origins.append(int(r[1]))
            targets.append(int(r[2]))
    return entries, origins, targets
