"""Smoke test of the bot interface from Python: games end, the wire round-trips, and speed.

    python3 py/smoke.py [games]
"""
import sys
import time

import numpy as np

import polyenv

games = int(sys.argv[1]) if len(sys.argv) > 1 else 4
rng = np.random.default_rng(0)
with polyenv.launch(8200) as env:
    print('server:', env.info())
    # The agent seat plays what the built-in AI would choose (Normal brain, handicap 1) against a
    # Crazy bot, observing every decision as a real agent would.
    t0, decisions, wins = time.time(), 0, 0
    for k in range(games):
        tribes = rng.choice(polyenv.FREE_TRIBES, 2)
        spec = polyenv.Spec(tribes=tuple(int(t) for t in tribes), bots=(None, 3), seed=100 + k)
        st = env.new(0, spec)
        while not st.ended and st.turn < 100:
            obs = env.observe([(0, 0)])[0]
            assert obs.get('me') == st.current and obs.tiles.shape == (14, 14, len(polyenv.TILE_FEATURES))
            i = env.bot([0])[0]
            ok, st = env.step([(0, i if i >= 0 else polyenv.STEP_BOT_OWN)])[0]
            assert ok
            decisions += 1
        wins += st.winner == 1
        print(f'game {k}: tribes {spec.tribes}, {"ended" if st.ended else "turn cap"} at turn {st.turn}, winner {st.winner}')
    dt = time.time() - t0
    print(f'{decisions} agent decisions in {dt:.1f} s ({decisions / dt:.0f}/s incl. the Crazy bot\'s turns), agent won {wins}/{games}')

    # Save/restore over the wire: a restored copy gives the same legal list and observation.
    st = env.new(1, polyenv.Spec(seed=7))
    for _ in range(30):
        st = env.step([(1, env.bot([1])[0] if env.bot([1])[0] >= 0 else polyenv.STEP_BOT_OWN)])[0][1]
    blob = env.save(1)
    st2 = env.load(2, polyenv.Spec(seed=7), blob)
    assert np.array_equal(st.legal, st2.legal)
    a, b = env.observe([(1, 0), (2, 0)])
    assert np.array_equal(a.tiles, b.tiles) and np.array_equal(a.globals, b.globals)
    print(f'save/load: {len(blob)} bytes, legal list and observation identical')

    # Batched random play on 16 slots, to time the wire with many games in one process.
    slots = list(range(10, 26))
    sts = {s: env.new(s, polyenv.Spec(seed=s)) for s in slots}
    t0, n = time.time(), 0
    while time.time() - t0 < 10:
        live = [s for s in slots if not sts[s].ended]
        acts = [(s, int(rng.integers(len(sts[s].legal)))) for s in live]
        env.observe([(s, 0) for s in live])
        for (s, _), (ok, st) in zip(acts, env.step(acts)):
            sts[s] = st
        n += len(live)
    print(f'random play, 16 slots batched: {n / (time.time() - t0):.0f} decisions/s with observations')
    print(env.dump(1)['CurrentTurn'], 'turns in slot 1 (dump works)')
