"""DAgger data: a trained policy plays championship games, and the game's AI labels every position the
policy reaches with the move it would make there (the M1 teacher: a person's seat, GetStupidity 0).

The policy's own moves are played (greedy, or sampled with --temperature), so the data covers the
positions the policy actually gets into, which behaviour cloning on the AI's own games never shows
it. Opponents cycle by seed over --opponents: a bot difficulty (0-3) on the other seat, or `self`
for the policy on both seats (both seats recorded). The agent's seat alternates.

Recorded per agent decision: observation, legal list, player, `label` (the AI's choice; -1 when it
would play its StayCommand, which is outside the agent's list), `played` (the policy's row) and
`logp` (its log-probability under the sampling policy, 0 when greedy). So the same games serve
DAgger (train_bc.py skips label -1) and RL (py/train_rl.py uses played/logp and the outcome).
Files are g<seed>.npz in gen_data.py's layout plus those arrays, `opponent` (difficulty, or -1 for
self-play) and `agent_seats`; seeds start at 20,000,000, apart from the teacher data (0-19,999)
and the evaluation seeds (10,000,000+). Repeating an opponent in --opponents raises its share.

    POLYHOST=mac .venv/bin/python py/dagger.py <checkpoint.pt> <out_dir> --games 1000 \
        [--opponents 1,2,3,self] [--procs 6 --slots 6] [--temperature 0]
"""
import argparse
import contextlib
import concurrent.futures as cf
import os
import time

import numpy as np

import eval_vs_bot
import gen_data
import polyenv
import remote

TURN_CAP = 150
COMMANDS_PER_TURN_CAP = 400


class Recorder(gen_data.Recorder):
    def __init__(self, seed, spec):
        super().__init__(seed, spec)
        self.played, self.logp = [], []


def save(out_dir, rec, st, owner, capital, opponent, agent_seats):
    gen_data.save_game(out_dir, rec, st, owner, capital)
    path = os.path.join(out_dir, f'g{rec.seed:08d}.npz')
    z = dict(np.load(path))
    z.update(opponent=np.int8(opponent), agent_seats=np.asarray(agent_seats, np.uint8),
             played=np.asarray(rec.played, np.int16), logp=np.asarray(rec.logp, np.float32),
             source=np.str_('dagger'))
    np.savez_compressed(path + '.tmp.npz', **z)
    os.replace(path + '.tmp.npz', path)


def sample(net, device, obs, statuses, temperature, rng):
    """The policy's rows and their log-probabilities under the sampling distribution (softmax of
    logits / temperature; greedy with log-probability 0 when temperature is 0)."""
    inputs = eval_vs_bot.batch_inputs(obs, statuses)
    logits = eval_vs_bot.policy_logits(net, device, inputs)
    if temperature <= 0:
        return logits.argmax(1), np.zeros(len(logits))
    acts, logps = [], []
    for row, m in zip(logits, inputs['mask']):
        z = (row[m] - row[m].max()) / temperature
        lp = z - np.log(np.exp(z).sum())
        a = int(rng.choice(len(lp), p=np.exp(lp) / np.exp(lp).sum()))
        acts.append(a)
        logps.append(lp[a])
    return np.array(acts), np.array(logps)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('checkpoint')
    ap.add_argument('out_dir')
    ap.add_argument('--games', type=int, default=1000)
    ap.add_argument('--first_seed', type=int, default=20_000_000)
    ap.add_argument('--opponents', default='1,2,3,self')
    ap.add_argument('--procs', type=int, default=6)
    ap.add_argument('--slots', type=int, default=6)
    ap.add_argument('--port', type=int, default=8900)
    ap.add_argument('--temperature', type=float, default=0.0)
    ap.add_argument('--tribe_pairs', default=None,
                    help='draw each game\'s tribes and the agent\'s seat from this file of lines "agent_tribe '
                         'opponent_tribe agent_seat(1|2)" (e.g. weak matchups) instead of at random')
    ap.add_argument('--remote', action='store_true', help='run the policy on the PC\'s GPU (py/remote.py)')
    ap.add_argument('--device', default='mps' if eval_vs_bot.torch.backends.mps.is_available() else 'cpu')
    args = ap.parse_args()
    os.makedirs(args.out_dir, exist_ok=True)
    opponents = [-1 if o == 'self' else int(o) for o in args.opponents.split(',')]
    pairs = [tuple(int(x) for x in l.split()) for l in open(args.tribe_pairs) if l.strip()] if args.tribe_pairs else None
    if args.remote:
        net = remote.RemoteNet(args.checkpoint)
        step = net.step
    else:
        net, step = eval_vs_bot.load_policy(args.checkpoint, args.device)
    rng = np.random.default_rng(args.first_seed)
    have = set(os.listdir(args.out_dir))
    jobs = [s for s in range(args.first_seed, args.first_seed + args.games) if f'g{s:08d}.npz' not in have]
    jobs.reverse()
    log = open(os.path.join(args.out_dir, 'games.tsv'), 'a')
    done = []
    with contextlib.ExitStack() as stack:
        envs = [stack.enter_context(polyenv.launch(args.port + i)) for i in range(args.procs)]
        pool = cf.ThreadPoolExecutor(len(envs))
        live = [dict() for _ in envs]

        def start(e, slot):
            seed = jobs.pop()
            opp = opponents[seed % len(opponents)]
            seat = (seed // len(opponents)) % 2
            tribes = tuple(int(t) for t in np.random.default_rng(seed).choice(eval_vs_bot.REGULAR_TRIBES, 2))
            if pairs:
                ta, tb, s1 = pairs[(seed // len(opponents)) % len(pairs)]
                seat = s1 - 1
                tribes = (ta, tb) if seat == 0 else (tb, ta)
            if opp < 0:
                bots, agent_seats = (None, None), (1, 2)
            else:
                bots, agent_seats = ((None, opp) if seat == 0 else (opp, None)), (seat + 1,)
            spec = polyenv.Spec(tribes=tribes, bots=bots, seed=seed)
            st = envs[e].new(slot, spec)
            live[e][slot] = dict(rec=Recorder(seed, spec), st=st, opp=opp, agent_seats=agent_seats,
                                 turn=st.turn, n=0)

        for e in range(len(envs)):
            for s in range(args.slots):
                if jobs:
                    start(e, s)
        t0 = time.time()
        timing = dict(env=0.0, net=0.0)
        # Two groups of slots (even, odd) alternate: while the network chooses for one group on this
        # thread, the game processes step the other group and observe it again on `orch`. Each game
        # process only ever serves one request at a time, as before.
        orch = cf.ThreadPoolExecutor(1)

        def finish(group):
            for e in range(len(envs)):
                for s, g in list(live[e].items()):
                    st = g['st']
                    if s % 2 != group or not (st.ended or st.turn >= TURN_CAP):
                        continue
                    owner, capital = envs[e].owners(s)
                    save(args.out_dir, g['rec'], st, owner, capital, g['opp'], g['agent_seats'])
                    res = 'cap' if not st.ended else 'self' if g['opp'] < 0 else \
                        'win' if st.winner in g['agent_seats'] else 'loss'
                    done.append((g['opp'], res))
                    log.write(f"{g['rec'].seed}\t{g['opp']}\t{','.join(map(str, g['agent_seats']))}\t"
                              f"{g['rec'].spec.tribes[0]},{g['rec'].spec.tribes[1]}\t{res}\t{st.turn}\t"
                              f"{len(g['rec'].labels)}\t{g['rec'].unlabelled}\n")
                    log.flush()
                    del live[e][s]
                    if jobs:
                        start(e, s)
                    if len(done) % 50 == 0:
                        summary = ', '.join(
                            f"vs {'self' if o < 0 else o}: {sum(r == 'win' for oo, r in done if oo == o)}/"
                            f"{sum(oo == o for oo, _ in done)}" for o in sorted(set(opponents)))
                        print(f'{len(done)} games ({summary}), {time.time() - t0:.0f} s '
                              f'(game processes {timing["env"]:.0f} s, network {timing["net"]:.0f} s)', flush=True)

        def prep(group):
            """Finishes the group's games, then observes its live slots and asks the AI for its label."""
            finish(group)
            order = [sorted(s for s in l if s % 2 == group) for l in live]
            t1 = time.time()
            obs = list(pool.map(lambda e: envs[e].observe([(s, 0) for s in order[e]]) if order[e] else [], range(len(envs))))
            labels = list(pool.map(lambda e: envs[e].bot(order[e]) if order[e] else [], range(len(envs))))
            timing['env'] += time.time() - t1
            flat = [(e, s) for e in range(len(envs)) for s in order[e]]
            return flat, [o for e in range(len(envs)) for o in obs[e]], [b for e in range(len(envs)) for b in labels[e]]

        def act(data):
            """The policy's choice for each slot of a prepared group, recorded; per-env actions, or None."""
            flat, flat_obs, flat_labels = data
            if not flat:
                return None
            t2 = time.time()
            statuses = [live[e][s]['st'] for e, s in flat]
            acts, logps = sample(net, args.device, flat_obs, statuses, args.temperature, rng)
            timing['net'] += time.time() - t2
            per_env = [[] for _ in envs]
            for (e, s), o, label, a, lp in zip(flat, flat_obs, flat_labels, acts, logps):
                g = live[e][s]
                st = g['st']
                g['rec'].add(o, st.legal, label, st.current)
                g['rec'].played.append(int(a))
                g['rec'].logp.append(float(lp))
                if label < 0:
                    g['rec'].unlabelled += 1
                if st.turn != g['turn']:
                    g['turn'], g['n'] = st.turn, 0
                g['n'] += 1
                if g['n'] > COMMANDS_PER_TURN_CAP:  # a safety net against a stuck policy
                    a = int(np.nonzero(st.legal[:, 0] == 15)[0][0])
                per_env[e].append((s, int(a)))
            return per_env

        def step_then_prep(group, per_env):
            if per_env is not None:
                t3 = time.time()
                stepped = list(pool.map(lambda e: envs[e].step(per_env[e]) if per_env[e] else [], range(len(envs))))
                timing['env'] += time.time() - t3
                for e in range(len(envs)):
                    for (s, _), (ok, st) in zip(per_env[e], stepped[e]):
                        if not ok:
                            raise polyenv.EnvError(f'env {e} slot {s}: legal command rejected')
                        live[e][s]['st'] = st
            return prep(group)

        here, group = prep(0), 0
        nxt = orch.submit(prep, 1)
        while True:
            per_env = act(here)
            other = nxt.result()
            if per_env is None and not other[0]:
                break
            nxt = orch.submit(step_then_prep, group, per_env)
            here, group = other, 1 - group
    print(f'checkpoint step {step}: {len(done)} games in {time.time() - t0:.0f} s', flush=True)


if __name__ == '__main__':
    main()
