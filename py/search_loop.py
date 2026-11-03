"""Search in the loop (research log S10, track s10x): py/turn_search.py's --reply planner, run inside
py/selfplay.py's training games (--search_share), through the shard's batched inference.

At the start of a share of the learner's turns, plan_turn() copies the real game into fog-safe samples
(polyenv.Env.determinize, the learner as viewer) on scratch slots of the same game process and makes
--search_plans lanes there: lane 0 takes the top move every time, the others sample at the actors'
--temperature from the same network. Each lane is made on sample 0 and replayed on the other samples
(a command that isn't legal there is skipped), then End Turn, then the opponent's whole reply played
greedily by the learner's network from the opponent's own view of the sample. A lane's score is the
fog critic's win estimate at the learner's next turn (1 or 0 if the game ended), averaged over the
samples.

The lane the real game plays is not the best-scoring one. It is drawn from the sampled lanes (1..K-1)
with probability softmax(score / tau) (sampling-importance-resampling). The sampled lanes are draws of
whole turns from the policy pi, so the drawn turn is approximately a draw from
pi'(turn) ~ pi(turn) exp(Q(turn) / tau): the KL-regularized improvement of pi under the planner's
scores (the form of Gumbel MuZero's completed-Q target and of MPO's E-step). tau -> 0 is copying the
max over noisy lanes (kevinz917's distillation, which lost about half the search gain). --search_tau 0
plays the best lane over all K, as turn_search.py does. py/train_rl.py --search_adv then moves pi toward
pi' on those decisions.

Nothing here sees what the learner can't: the lanes, the reply and the scores all run on samples
built from its own view.
"""
import time

import numpy as np

END_TURN = 15
COMMANDS_PER_TURN_CAP = 400
SLOT_STRIDE = 50  # scratch slots of thread group j: base + j * SLOT_STRIDE ...
INFO = ('turn', 'player', 'top', 'chosen', 'best', 'mean', 'kl', 'commands', 'broken', 'seconds', 'lane', 'spread', 'noise')


def key(row):
    return tuple(int(x) for x in row)


def find(st, k):
    """Row index of command k (type, a, b, arg) in st's legal list, or -1."""
    if not len(st.legal):
        return -1
    hit = np.nonzero((st.legal == np.asarray(k, st.legal.dtype)).all(1))[0]
    return int(hit[0]) if len(hit) else -1


def end_row(st):
    e = np.nonzero(st.legal[:, 0] == END_TURN)[0] if len(st.legal) else []
    return int(e[0]) if len(e) else -1


def ask(infer, reqs):
    """Several inference requests [(key, obs, statuses, temperature)] at once (they batch with the
    other threads' requests); returns [(rows, logp, value)]."""
    pending = [infer.submit(*r) for r in reqs]
    return [infer.collect(p) for p in pending]


def pick(seed, turn, player, share):
    """Whether this turn of this player is searched: a fixed draw per (game seed, turn, player)."""
    return share > 0 and np.random.default_rng([seed, 5, turn, player]).random() < share


def plan_turn(env, infer, src, a_id, base, plans, samples, temperature, tau, seed, rng):
    """The planner at the start of a_id's turn in slot src. Returns (commands, info): the drawn lane's
    commands, End Turn included when the lane ended its turn, and a dict with INFO's fields."""
    t0 = time.time()
    K, D = plans, samples
    slot = lambda d, k: base + d * K + k
    sts = {}
    for d in range(D):
        for k in range(K):
            sts[slot(d, k)] = env.determinize(src, slot(d, k), a_id, (seed * 64 + d) % (1 << 63))
    lanes = {k: [] for k in range(K)}
    ended_turn = {k: False for k in range(K)}
    # 1. Lanes on sample 0, in lockstep.
    active, n = set(range(K)), {k: 0 for k in range(K)}
    while active:
        ks = sorted(active)
        ids = [slot(0, k) for k in ks]
        obs = env.observe([(i, 0) for i in ids])
        greedy = [j for j, k in enumerate(ks) if k == 0]
        sampled = [j for j, k in enumerate(ks) if k != 0]
        reqs, parts = [], []
        for js, temp in ((greedy, 0.0), (sampled, temperature)):
            if js:
                reqs.append(('A', [obs[j] for j in js], [sts[ids[j]] for j in js], temp))
                parts.append(js)
        rows = {}
        for js, (r, _, _) in zip(parts, ask(infer, reqs)):
            for j, a in zip(js, r):
                rows[j] = int(a)
        acts = []
        for j, k in enumerate(ks):
            st = sts[ids[j]]
            a = rows[j]
            n[k] += 1
            if st.legal[a][0] != END_TURN and n[k] > COMMANDS_PER_TURN_CAP:
                e = end_row(st)
                a = e if e >= 0 else a
            if st.legal[a][0] == END_TURN:
                lanes[k].append(key(st.legal[a]))
                ended_turn[k] = True
                active.discard(k)
            else:
                lanes[k].append(key(st.legal[a]))
                acts.append((ids[j], a))
        for (i, _), (ok, st) in zip(acts, env.step(acts) if acts else []):
            sts[i] = st
            k = i - slot(0, 0)
            if not ok:  # the game rejected a listed command (and relisted the state): not part of the lane
                lanes[k].pop()
            if st.ended or st.current != a_id:  # the lane won (or the turn passed some other way)
                active.discard(k)
    # 2. The same lanes on the other samples, in lockstep (End Turn is played in 3).
    for d in range(1, D):
        ptr = {k: 0 for k in range(K)}
        body = {k: lanes[k][:-1] if ended_turn[k] else lanes[k] for k in range(K)}
        while True:
            acts = []
            for k in range(K):
                i = slot(d, k)
                st = sts[i]
                while ptr[k] < len(body[k]) and not st.ended and st.current == a_id:
                    r = find(st, body[k][ptr[k]])
                    ptr[k] += 1
                    if r >= 0:
                        acts.append((i, r))
                        break
            if not acts:
                break
            for (i, _), (ok, st) in zip(acts, env.step(acts)):
                sts[i] = st
    # 3. End Turn, then the opponent's reply (greedy, from its own view), until a_id is to move again.
    ends = [(i, end_row(st)) for i, st in sts.items() if not st.ended and st.current == a_id]
    ends = [(i, e) for i, e in ends if e >= 0]
    for (i, _), (ok, st) in zip(ends, env.step(ends) if ends else []):
        sts[i] = st
    replying = {i for i, st in sts.items() if not st.ended and st.current != a_id}
    m = {i: 0 for i in replying}
    while replying:
        ids = sorted(replying)
        obs = env.observe([(i, 0) for i in ids])
        (r, _, _), = ask(infer, [('A', obs, [sts[i] for i in ids], 0.0)])
        acts = []
        for i, a in zip(ids, r):
            a = int(a)
            m[i] += 1
            if m[i] > COMMANDS_PER_TURN_CAP:
                e = end_row(sts[i])
                a = e if e >= 0 else a
            acts.append((i, a))
        for (i, _), (ok, st) in zip(acts, env.step(acts)):
            sts[i] = st
            if st.ended or st.current == a_id:
                replying.discard(i)
    # 4. Scores: a_id's win estimate at its next turn (the fog critic, key 'C'), averaged over samples.
    live = [i for i, st in sts.items() if not st.ended and st.current == a_id]
    est = {}
    if live:
        (_, _, v), = ask(infer, [('C', env.observe([(i, a_id) for i in live]), [sts[i] for i in live], 0.0)])
        est = dict(zip(live, (float(x) for x in v)))
    per = np.zeros((K, D))
    for k in range(K):
        for d in range(D):
            st = sts[slot(d, k)]
            per[k, d] = (1.0 if st.winner == a_id else 0.0) if st.ended else est.get(slot(d, k), 0.5)
    score = per.mean(1)
    # 5. The drawn lane.
    if tau > 0 and K > 1:
        z = score[1:] / tau
        w = np.exp(z - z.max())
        w /= w.sum()
        k = 1 + int(rng.choice(K - 1, p=w))
        kl = float(np.sum(w * np.log(np.maximum(w * (K - 1), 1e-12))))
        mean = float(score[1:].mean())
    else:
        k = int(np.argmax(score))
        kl, mean = 0.0, float(score.mean())
    # spread: sd of the sampled lanes' scores; noise: a lane's sd over the samples, averaged over lanes
    info = dict(top=float(score[0]), chosen=float(score[k]), best=float(score.max()), mean=mean, kl=kl,
                commands=len(lanes[k]), broken=0, seconds=time.time() - t0, lane=k,
                spread=float(score[1:].std()) if K > 1 else 0.0, noise=float(per.std(1).mean()))
    return lanes[k], info
