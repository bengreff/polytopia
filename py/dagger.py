"""DAgger data: a trained policy plays championship games, and the game's AI labels every position the
policy reaches with the move it would make there (the M1 teacher: a person's seat, GetStupidity 0).

The policy's own moves are played (greedy, or sampled with --temperature), so the data covers the
positions the policy actually gets into, which behaviour cloning on the AI's own games never shows
it. Opponents cycle by seed over --opponents: a bot difficulty (0-3) on the other seat, or `self`
for the policy on both seats (both seats recorded). The agent's seat alternates.

Recorded per agent decision, as gen_data.py does: observation, legal list, the AI's choice (its
StayCommand, outside the agent's list, is not recorded) and the player. Outcomes give value targets
for the policy's own games. Files are g<seed>.npz in gen_data.py's layout, plus `opponent`
(difficulty, or -1 for self-play) and `agent_seats`; seeds start at 20,000,000, apart from the
teacher data (0-19,999) and the evaluation seeds (10,000,000+).

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


def save(out_dir, rec, st, owner, capital, opponent, agent_seats):
    gen_data.save_game(out_dir, rec, st, owner, capital)
    path = os.path.join(out_dir, f'g{rec.seed:08d}.npz')
    z = dict(np.load(path))
    z.update(opponent=np.int8(opponent), agent_seats=np.asarray(agent_seats, np.uint8),
             source=np.str_('dagger'))
    np.savez_compressed(path + '.tmp.npz', **z)
    os.replace(path + '.tmp.npz', path)


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
    ap.add_argument('--remote', action='store_true', help='run the policy on the PC\'s GPU (py/remote.py)')
    ap.add_argument('--device', default='mps' if eval_vs_bot.torch.backends.mps.is_available() else 'cpu')
    args = ap.parse_args()
    os.makedirs(args.out_dir, exist_ok=True)
    opponents = [-1 if o == 'self' else int(o) for o in args.opponents.split(',')]
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
            if opp < 0:
                bots, agent_seats = (None, None), (1, 2)
            else:
                bots, agent_seats = ((None, opp) if seat == 0 else (opp, None)), (seat + 1,)
            spec = polyenv.Spec(tribes=tribes, bots=bots, seed=seed)
            st = envs[e].new(slot, spec)
            live[e][slot] = dict(rec=gen_data.Recorder(seed, spec), st=st, opp=opp, agent_seats=agent_seats,
                                 turn=st.turn, n=0)

        for e in range(len(envs)):
            for s in range(args.slots):
                if jobs:
                    start(e, s)
        t0 = time.time()
        while any(live):
            for e in range(len(envs)):
                for s, g in list(live[e].items()):
                    st = g['st']
                    if st.ended or st.turn >= TURN_CAP:
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
                                f"{sum(oo == o for oo, _ in done)}" for o in opponents)
                            print(f'{len(done)} games ({summary}), {time.time() - t0:.0f} s', flush=True)
            order = [sorted(l) for l in live]
            if not any(order):
                break
            obs = list(pool.map(lambda e: envs[e].observe([(s, 0) for s in order[e]]) if order[e] else [], range(len(envs))))
            labels = list(pool.map(lambda e: envs[e].bot(order[e]) if order[e] else [], range(len(envs))))
            flat = [(e, s) for e in range(len(envs)) for s in order[e]]
            flat_obs = [o for e in range(len(envs)) for o in obs[e]]
            flat_labels = [b for e in range(len(envs)) for b in labels[e]]
            statuses = [live[e][s]['st'] for e, s in flat]
            acts = eval_vs_bot.choose(net, args.device, flat_obs, statuses, args.temperature, rng)
            per_env = [[] for _ in envs]
            for (e, s), o, label, a in zip(flat, flat_obs, flat_labels, acts):
                g = live[e][s]
                st = g['st']
                if label >= 0:
                    g['rec'].add(o, st.legal, label, st.current)
                else:
                    g['rec'].unlabelled += 1
                if st.turn != g['turn']:
                    g['turn'], g['n'] = st.turn, 0
                g['n'] += 1
                if g['n'] > COMMANDS_PER_TURN_CAP:  # a safety net against a stuck policy
                    a = int(np.nonzero(st.legal[:, 0] == 15)[0][0])
                per_env[e].append((s, int(a)))
            stepped = list(pool.map(lambda e: envs[e].step(per_env[e]) if per_env[e] else [], range(len(envs))))
            for e in range(len(envs)):
                for (s, _), (ok, st) in zip(per_env[e], stepped[e]):
                    if not ok:
                        raise polyenv.EnvError(f'env {e} slot {s}: legal command rejected')
                    live[e][s]['st'] = st
    print(f'checkpoint step {step}: {len(done)} games in {time.time() - t0:.0f} s', flush=True)


if __name__ == '__main__':
    main()
