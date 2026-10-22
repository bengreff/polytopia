"""Cross-host parity: the same seeded games on two hosts must be identical. Plays championship games
with a fixed pseudo-random policy (half of them against Crazy, so the built-in AI is covered too) and
prints, per game, a hash of every legal list along the way and of the final state's bytes. Run it
on each host (e.g. POLYHOST=mac on the Mac, POLYHOST=qemu on the PC) and diff the outputs.

    POLYHOST=qemu python3 py/parity_hosts.py --games 12 --turns 25 > parity_pc.txt
"""
import argparse
import hashlib

import numpy as np

import polyenv

REGULAR_TRIBES = sorted(polyenv.TRIBES.values())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--games', type=int, default=12)
    ap.add_argument('--turns', type=int, default=25)
    ap.add_argument('--first_seed', type=int, default=40_000_000)
    ap.add_argument('--port', type=int, default=9190)
    args = ap.parse_args()
    with polyenv.launch(args.port) as env:
        print(env.info())
        for k in range(args.games):
            seed = args.first_seed + k
            tribes = tuple(int(t) for t in np.random.default_rng(seed).choice(REGULAR_TRIBES, 2))
            bots = (None, 3) if k % 2 else (None, None)
            st = env.new(0, polyenv.Spec(tribes=tribes, bots=bots, seed=seed))
            h, rng, n = hashlib.sha256(), np.random.default_rng(seed), 0
            while not st.ended and st.turn < args.turns:
                h.update(np.ascontiguousarray(st.legal).tobytes())
                obs = env.observe([(0, 0)])[0]
                h.update(obs.tiles.tobytes() + obs.globals.tobytes() + obs.techs.tobytes())
                row = int(rng.integers(len(st.legal)))
                if st.legal[row, 0] == 15 and rng.random() < 0.7:  # end turns less often than uniform
                    row = int(rng.integers(len(st.legal)))
                ok, st = env.step([(0, row)])[0]
                assert ok
                n += 1
            final = env.save(0)
            print(f'seed {seed} tribes {tribes} bots {bots}: {n} decisions, turn {st.turn}, ended {st.ended}, '
                  f'winner {st.winner}, path {h.hexdigest()[:16]}, final {hashlib.sha256(final).hexdigest()[:16]} '
                  f'({len(final)} bytes)', flush=True)


if __name__ == '__main__':
    main()
