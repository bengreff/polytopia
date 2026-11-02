"""Turn planning with the engine (research log S6): does choosing among whole candidate turns beat the
raw network?

The planning side (A) starts each of its turns by copying the game into N scratch slots (the engine's
own save and load). In each slot it plays out a complete turn with its policy: plan 1 takes the top
move every time, the others sample at --temperature. A plan ends when the policy chooses End Turn
(not yet played) or the game ends. Each plan is scored by A's own win estimate (the value head) at
that moment: the position after the plan, from A's view, before handing over the turn. The best
plan's position is loaded into the real game and its End Turn played.

Side B is a network playing greedily without planning (by default the same checkpoint), or the
game's AI (bot:<difficulty>). Each seed is played twice with the seats swapped; tribes are drawn
from the seed as in py/selfplay.py. Games run in --procs worker processes, each with its own engine
process and networks.

    POLYHOST=qemu python turn_search.py gen047.pt [--b gen047.pt | --b bot:3] [--plans 8] [--seeds 50]

**--reply (radical-ideas.md A1; the S9 read's planner re-test):** fixes S6's three flaws.
- **Fog-safe:** plans run on fog-safe samples of the game (polyenv.Env.determinize, viewer A), not
  the true state.
- **Lanes:** the top-move plan plus --plans - 1 plans sampled at --temperature (0.5).
- **Several samples:** each plan is made on sample 0 and replayed command by command on --samples - 1
  more samples (a command that isn't legal there is skipped).
- **The reply:** after the plan's End Turn, the opponent's whole turn is played greedily by --model
  (default: A's own network) from the opponent's own view of the sample.
- **The score:** A's win estimate at the start of its next turn, from --scorer (a checkpoint whose
  value head is used: A's own network by default, or a separate critic), averaged over the samples.
  A plan that wins the game scores 1.
- **Playing it:** the best plan's commands are played in the real game in order. If one isn't legal
  there (the real hidden part differed), the rest of the turn is played by the raw policy.

    POLYHOST=linux python turn_search.py s9v/gen083.pt --reply --scorer s9v/critic_g083.pt --plans 16 --samples 2 --seeds 100
"""
import argparse
import json
import multiprocessing as mp
import time

import numpy as np

TURN_CAP = 150
COMMANDS_PER_TURN_CAP = 400


def worker(args, jobs, port, out_q):
    import torch

    import eval_vs_bot
    import fast_infer
    import polyenv
    from selfplay import REGULAR_TRIBES, load_net
    end_turn = {v: k for k, v in polyenv.COMMAND_TYPES.items()}['EndTurn']
    dev = 'cuda' if torch.cuda.is_available() else 'cpu'
    net_a = fast_infer.FastPolicy(load_net(args.net, dev), dev, bf16=False)
    model = net_a if not args.model else fast_infer.FastPolicy(load_net(args.model, dev), dev, bf16=False)
    scorer = net_a if not args.scorer else fast_infer.FastPolicy(load_net(args.scorer, dev), dev, bf16=False)
    bot = int(args.b.split(':')[1]) if args.b.startswith('bot:') else None
    net_b = net_a if args.b == args.net else (None if bot is not None else fast_infer.FastPolicy(load_net(args.b, dev), dev, bf16=False))
    rng = np.random.default_rng(port)

    def infer(net, slots, sts, obs):
        logits, v = net(eval_vs_bot.batch_inputs(obs, [sts[s] for s in slots]))
        return logits.float().cpu().numpy(), torch.sigmoid(v.float()).cpu().numpy()

    def key(row):
        return tuple(int(x) for x in row)

    def find(st, k):
        """Row index of command k (type, a, b, arg) in st's legal list, or -1."""
        hit = np.nonzero((st.legal == np.asarray(k, st.legal.dtype)).all(1))[0] if len(st.legal) else []
        return int(hit[0]) if len(hit) else -1

    def search_reply(env, spec, a_id, stats, seed):
        """The --reply planner: returns the real game's status after A's turn (End Turn played)."""
        K, D = args.plans, args.samples
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
            logits, _ = infer(net_a, ids, sts, env.observe([(i, 0) for i in ids]))
            acts = []
            for j, k in enumerate(ks):
                st = sts[ids[j]]
                row = logits[j][:len(st.legal)]
                if k == 0:
                    a = int(np.argmax(row))
                else:
                    z = (row - row.max()) / args.temperature
                    p = np.exp(z) / np.exp(z).sum()
                    a = int(rng.choice(len(p), p=p))
                n[k] += 1
                if st.legal[a][0] == end_turn or n[k] > COMMANDS_PER_TURN_CAP:
                    active.discard(k)
                else:
                    plans[k].append(key(st.legal[a]))
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
                    r = find(st, c)
                    if r >= 0:
                        sts[slot(d, k)] = env.step([(slot(d, k), r)])[0][1]
        # 3. End Turn, then the opponent's reply (greedy, from its own view), until A is to move again.
        ends = []
        for i, st in sts.items():
            if not st.ended and st.current == a_id:
                e = np.nonzero(st.legal[:, 0] == end_turn)[0]
                if len(e):
                    ends.append((i, int(e[0])))
        for (i, _), (ok, st) in zip(ends, env.step(ends) if ends else []):
            sts[i] = st
        replying = {i for i, st in sts.items() if not st.ended and st.current != a_id}
        m = {i: 0 for i in replying}
        while replying:
            ids = sorted(replying)
            logits, _ = infer(model, ids, sts, env.observe([(i, 0) for i in ids]))
            acts = []
            for j, i in enumerate(ids):
                st = sts[i]
                a = int(np.argmax(logits[j][:len(st.legal)]))
                m[i] += 1
                if m[i] > COMMANDS_PER_TURN_CAP:
                    e = np.nonzero(st.legal[:, 0] == end_turn)[0]
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
            _, v = infer(scorer, live, sts, env.observe([(i, a_id) for i in live]))
            est = dict(zip(live, v))
        score = {}
        for k in range(K):
            vals = []
            for d in range(D):
                st = sts[slot(d, k)]
                vals.append((1.0 if st.winner == a_id else 0.0) if st.ended else float(est[slot(d, k)]))
            score[k] = float(np.mean(vals))
        best = max(score, key=lambda k: score[k])
        stats['planned'] += 1
        stats['greedy_best'] += best == 0
        stats['gain'] += score[best] - score[0]
        stats['spread'] += max(score.values()) - min(score.values())
        # 5. The best plan in the real game; the raw policy finishes the turn if a command doesn't apply.
        real = env.load(0, spec, env.save(0))
        for c in plans[best]:
            r = find(real, c)
            if r < 0:
                stats['broken'] += 1
                break
            real = env.step([(0, r)])[0][1]
            if real.ended:
                return real
        k = 0
        while not real.ended and real.current == a_id and k < COMMANDS_PER_TURN_CAP:
            logits, _ = infer(net_a, [0], {0: real}, env.observe([(0, 0)]))
            a = int(np.argmax(logits[0][:len(real.legal)]))
            if real.legal[a][0] == end_turn:
                return env.step([(0, a)])[0][1]
            real = env.step([(0, a)])[0][1]
            k += 1
        if not real.ended and real.current == a_id:
            e = np.nonzero(real.legal[:, 0] == end_turn)[0]
            real = env.step([(0, int(e[0]))])[0][1] if len(e) else real
        return real

    def search_turn(env, spec, a_id, stats):
        blob = env.save(0)
        sts = {i: env.load(i, spec, blob) for i in range(1, args.plans + 1)}
        active, value, n = set(sts), {}, {i: 0 for i in sts}
        while active:
            ids = sorted(active)
            obs = env.observe([(i, 0) for i in ids])
            logits, v = infer(net_a, ids, sts, obs)
            acts = []
            for k, i in enumerate(ids):
                row = logits[k][:len(sts[i].legal)]
                if i == 1:
                    a = int(np.argmax(row))
                else:
                    z = (row - row.max()) / args.temperature
                    p = np.exp(z) / np.exp(z).sum()
                    a = int(rng.choice(len(p), p=p))
                n[i] += 1
                if sts[i].legal[a][0] == end_turn or n[i] > COMMANDS_PER_TURN_CAP:
                    value[i] = float(v[k])
                    active.discard(i)
                else:
                    acts.append((i, a))
            for (i, _), (ok, st) in zip(acts, env.step(acts) if acts else []):
                sts[i] = st
                if st.ended:  # the plan ended the game
                    value[i] = 1.0 if st.winner == a_id else 0.0
                    active.discard(i)
        best = max(value, key=lambda i: value[i])
        stats['planned'] += 1
        stats['greedy_best'] += best == 1
        stats['gain'] += value[best] - value[1]
        stats['spread'] += max(value.values()) - min(value.values())
        st = env.load(0, spec, env.save(best))
        if not st.ended:
            ends = np.nonzero(st.legal[:, 0] == end_turn)[0]
            st = env.step([(0, int(ends[0]))])[0][1] if len(ends) else st
        return st

    with polyenv.launch(port) as env:
        for seed, seat in jobs:
            tribes = tuple(int(t) for t in np.random.default_rng(seed).choice(REGULAR_TRIBES, 2))
            a_id = seat + 1
            bots = (None, None) if bot is None else ((None, bot) if seat == 0 else (bot, None))
            spec = polyenv.Spec(tribes=tribes, bots=bots, seed=seed)
            st = env.new(0, spec)
            stats = dict(planned=0, greedy_best=0, gain=0.0, spread=0.0, broken=0)
            t0, b_turn, n_b = time.time(), -1, 0
            while not st.ended and st.turn < TURN_CAP:
                if st.current == a_id:
                    st = search_reply(env, spec, a_id, stats, seed * 1000 + st.turn) if args.reply else search_turn(env, spec, a_id, stats)
                else:  # B: one greedy move at a time (a bot seat plays inside the engine)
                    if st.turn != b_turn:
                        b_turn, n_b = st.turn, 0
                    obs = env.observe([(0, 0)])
                    logits, _ = infer(net_b, [0], {0: st}, obs)
                    a = int(np.argmax(logits[0][:len(st.legal)]))
                    n_b += 1
                    ends = np.nonzero(st.legal[:, 0] == end_turn)[0]
                    if n_b > COMMANDS_PER_TURN_CAP and len(ends):  # a safety net, as in py/selfplay.py
                        a = int(ends[0])
                    st = env.step([(0, a)])[0][1]
            res = 'win' if st.ended and st.winner == a_id else 'loss' if st.ended else 'cap'
            out_q.put(dict(seed=seed, seat=seat, res=res, turns=st.turn, seconds=round(time.time() - t0), **stats))


def main():
    import eval_vs_bot
    ap = argparse.ArgumentParser()
    ap.add_argument('net')
    ap.add_argument('--b', default=None, help='side B: a checkpoint (default: the same) or bot:<difficulty>')
    ap.add_argument('--plans', type=int, default=8)
    ap.add_argument('--temperature', type=float, default=1.0)
    ap.add_argument('--seeds', type=int, default=50)
    ap.add_argument('--first_seed', type=int, default=16_000_000)
    ap.add_argument('--procs', type=int, default=6)
    ap.add_argument('--port', type=int, default=9700)
    ap.add_argument('--out', default=None, help='results, one JSON line per game')
    ap.add_argument('--reply', action='store_true', help='fog-safe samples, plans scored after the opponent\'s reply (docstring)')
    ap.add_argument('--samples', type=int, default=2, help='--reply: determinized samples each plan is scored on')
    ap.add_argument('--scorer', default=None, help='--reply: checkpoint whose value head scores plans (default: the planner\'s own)')
    ap.add_argument('--model', default=None, help='--reply: the network that plays the opponent\'s reply (default: the planner\'s own)')
    args = ap.parse_args()
    args.b = args.b or args.net
    jobs = [(args.first_seed + k, seat) for k in range(args.seeds) for seat in (0, 1)]
    ctx = mp.get_context('spawn')
    q = ctx.Queue()
    ps = [ctx.Process(target=worker, args=(args, jobs[i::args.procs], args.port + i, q)) for i in range(args.procs)]
    for p in ps:
        p.start()
    results, t0 = [], time.time()
    out = open(args.out, 'a') if args.out else None
    while len(results) < len(jobs):
        if not any(p.is_alive() for p in ps) and q.empty():
            break
        try:
            r = q.get(timeout=30)
        except Exception:
            continue
        results.append(r)
        if out:
            out.write(json.dumps(r) + '\n')
            out.flush()
        w = sum(x['res'] == 'win' for x in results)
        print(f"{len(results)}/{len(jobs)} games: planner won {w} ({w / len(results):.1%}); "
              f"best plan = top-move plan on {sum(x['greedy_best'] for x in results) / max(1, sum(x['planned'] for x in results)):.0%} of turns, "
              f"mean estimate gain {sum(x['gain'] for x in results) / max(1, sum(x['planned'] for x in results)):.3f}, "
              f"{time.time() - t0:.0f} s", flush=True)
    for p in ps:
        p.join()
    n = len(results)
    w = sum(r['res'] == 'win' for r in results)
    lo, hi = eval_vs_bot.wilson(w, n)
    print('SUMMARY ' + json.dumps(dict(a=args.net, b=args.b, plans=args.plans, temperature=args.temperature, reply=args.reply,
                                       samples=args.samples, scorer=args.scorer, games=n, a_wins=w,
                                       a_rate=w / max(n, 1), ci=[lo, hi], capped=sum(r['res'] == 'cap' for r in results),
                                       seconds=round(time.time() - t0))), flush=True)


if __name__ == '__main__':
    main()
