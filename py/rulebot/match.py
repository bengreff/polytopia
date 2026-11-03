"""Plays RuleBot against the game's built-in AI (or a network checkpoint) and prints the win rate
with a 95% Wilson interval.

Seeds and tribes follow py/eval_vs_bot.py: each seed is played twice on the same map and tribes,
seats swapped; tribes are np.random.default_rng(seed).choice(regular tribes, 2). The bot plays a
person's seat (handicap 1), so Crazy has +3 stars per turn over it.

    POLYHOST=mac POLYHOST_BIN=build/polyhost-mac-det .venv/bin/python -m rulebot.match \
        --vs bot:3 --seeds 50 --procs 4 --port 8800 [--first_seed 20000000] [--record runs/rulebot/x]
"""
import argparse
import json
import math
import multiprocessing as mp
import os
import pickle
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import polyenv  # noqa: E402
from rulebot.bot import RuleBot  # noqa: E402

REGULAR_TRIBES = sorted(polyenv.TRIBES.values())
TURN_CAP = 100


def wilson(wins, n, z=1.96):
    if n == 0:
        return 0.0, 0.0
    p = wins / n
    d = 1 + z * z / n
    c = p + z * z / (2 * n)
    r = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n))
    return (c - r) / d, (c + r) / d


def tribes_for(seed):
    return tuple(int(t) for t in np.random.default_rng(seed).choice(REGULAR_TRIBES, 2))


def play_game(env, slot, seed, seat, difficulty, params=None, record=False, turn_cap=TURN_CAP, opponent=None):
    """seat: 0 or 1, the bot's seat. Returns a result dict (and the review record if asked)."""
    tribes = tribes_for(seed)
    if opponent is not None:  # another RuleBot (params dict) or a network ('net', net, device) in the other seat
        bots = (None, None)
    else:
        bots = (None, difficulty) if seat == 0 else (difficulty, None)
    spec = polyenv.Spec(tribes=tribes, bots=bots, seed=seed)
    st = env.new(slot, spec)
    me = seat + 1
    bot = RuleBot(params, log=record)
    net = None
    if isinstance(opponent, tuple) and opponent[0] == 'net':
        net, other = opponent[1:], None
    else:
        other = RuleBot(opponent, log=False) if opponent is not None else None
    t0 = time.time()
    n_dec, think = 0, 0.0
    rec = dict(turns=[]) if record else None
    last_turn = -1
    while not st.ended and st.turn < turn_cap:
        if st.current == me:
            obs = env.observe([(slot, me)])[0]
            if record and st.turn != last_turn:
                last_turn = st.turn
                # review only, never shown to the bot: the opponent's own view of its economy
                oo = env.observe([(slot, 3 - me)])[0]
                opp = dict(cities=oo.get('cities'), income=oo.get('income'), stars=oo.get('stars'),
                           units=int((oo.tiles[..., polyenv.TILE_FEATURES.index('unit_owner')] == 1).sum()),
                           techs=int(oo.techs.sum()))
                rec['turns'].append(dict(turn=st.turn, tiles=obs.tiles.copy(), globals=obs.globals.copy(),
                                         techs=obs.techs.copy(), log_at=len(bot.log), opp=opp))
            t1 = time.time()
            a = bot.act(obs, st)
            think += time.time() - t1
            n_dec += 1
        else:
            obs = env.observe([(slot, st.current)])[0]
            if net is not None:  # the network plays greedily from its own observation (py/eval_vs_bot.py)
                import eval_vs_bot as evb
                a = int(evb.choose(net[0], net[1], [obs], [st], 0.0, None)[0])
            else:
                a = other.act(obs, st)
        ok, st = env.step([(slot, a)])[0]
        if not ok:
            raise polyenv.EnvError(f'seed {seed} seat {seat}: legal command rejected')
    res = 'win' if st.ended and st.winner == me else 'loss' if st.ended else 'cap'
    out = dict(seed=seed, seat=seat, tribes=tribes, my_tribe=tribes[seat], opp_tribe=tribes[1 - seat], result=res,
               turns=st.turn, decisions=n_dec, think_ms=1000 * think / max(1, n_dec), secs=time.time() - t0,
               stats=dict(bot.stats), checks=bot.checks)
    if record:
        rec['log'] = bot.log
        rec['meta'] = out
        rec['attack_log'] = bot.mem.get('attack_log', [])
        rec['checks'] = bot.checks
        rec['final'] = env.save(slot)  # the finished game, for review only
        rec['spec'] = (tribes, bots, seed)
        return out, rec
    return out, None


def worker(args):
    port, jobs, difficulty, params, record_dir, opponent = args
    out = []
    if isinstance(opponent, str) and opponent.endswith('.pt'):  # load the network once per worker
        import torch
        import eval_vs_bot as evb
        device = 'cuda' if torch.cuda.is_available() else 'mps' if torch.backends.mps.is_available() else 'cpu'
        net, _ = evb.load_policy(opponent, device)
        opponent = ('net', net, device)
    with polyenv.launch(port) as env:
        for seed, seat in jobs:
            try:
                r, rec = play_game(env, 0, seed, seat, difficulty, params, record=bool(record_dir), opponent=opponent)
            except Exception as ex:  # keep the batch going; report the failure
                r, rec = dict(seed=seed, seat=seat, result='error', error=repr(ex), turns=0, tribes=tribes_for(seed),
                              my_tribe=tribes_for(seed)[seat], opp_tribe=tribes_for(seed)[1 - seat], think_ms=0,
                              decisions=0), None
            if rec is not None:
                with open(os.path.join(record_dir, f"{seed}_s{seat + 1}.pkl"), 'wb') as f:
                    pickle.dump(rec, f)
            out.append(r)
            print(f"  {seed} seat {seat + 1} {polyenv_name(r['my_tribe'])} vs {polyenv_name(r['opp_tribe'])}: "
                  f"{r['result']} t{r['turns']} ({r.get('think_ms', 0):.1f} ms/dec)", flush=True)
    return out


def polyenv_name(t):
    return {v: k for k, v in polyenv.TRIBES.items()}.get(t, str(t))


def run(vs, seeds, first_seed, procs, port, params=None, record_dir=None, opponent=None, quiet=False):
    difficulty = int(vs.split(':')[1]) if vs.startswith('bot:') else None
    jobs = [(first_seed + k, s) for k in range(seeds) for s in (0, 1)]
    if record_dir:
        os.makedirs(record_dir, exist_ok=True)
    chunks = [jobs[i::procs] for i in range(procs)]
    t0 = time.time()
    with mp.Pool(procs) as pool:
        results = [r for rs in pool.map(worker, [(port + i, c, difficulty, params, record_dir, opponent)
                                                 for i, c in enumerate(chunks)]) for r in rs]
    n = len([r for r in results if r['result'] != 'error'])
    w = sum(r['result'] == 'win' for r in results)
    lo, hi = wilson(w, n)
    summary = dict(vs=vs, games=n, wins=w, rate=w / max(1, n), ci=(lo, hi),
                   caps=sum(r['result'] == 'cap' for r in results), errors=sum(r['result'] == 'error' for r in results),
                   mean_turns_win=float(np.mean([r['turns'] for r in results if r['result'] == 'win'] or [0])),
                   think_ms=float(np.mean([r['think_ms'] for r in results if r['result'] != 'error'] or [0])),
                   secs=time.time() - t0)
    if not quiet:
        print(f"\nRuleBot vs {vs}: {n} games, won {w} ({w / max(1, n):.1%}, 95% Wilson CI {lo:.1%}-{hi:.1%}), "
              f"{summary['caps']} capped at turn {TURN_CAP}, {summary['errors']} errors, "
              f"mean win turn {summary['mean_turns_win']:.1f}, {summary['think_ms']:.1f} ms/decision, "
              f"{summary['secs']:.0f} s")
        for seat in (0, 1):
            rs = [r for r in results if r['seat'] == seat]
            print(f"  seat {seat + 1}: won {sum(r['result'] == 'win' for r in rs)}/{len(rs)}")
    return summary, results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--vs', default='bot:0', help="bot:<difficulty 0-3> | rulebot (self-play) | <checkpoint.pt> (network, "
                                                  "greedy, torch on MPS: keep --procs small)")
    ap.add_argument('--seeds', type=int, default=20, help='games = 2 x seeds (seats swapped)')
    ap.add_argument('--first_seed', type=int, default=20_000_000)
    ap.add_argument('--procs', type=int, default=4)
    ap.add_argument('--port', type=int, default=8800)
    ap.add_argument('--params', default=None, help='JSON dict of RuleBot parameter overrides')
    ap.add_argument('--record', default=None, help='directory: keep a review record per game (rulebot.review)')
    ap.add_argument('--out', default=None, help='append results as JSON lines')
    args = ap.parse_args()
    params = json.loads(args.params) if args.params else None
    opponent = {} if args.vs == 'rulebot' else args.vs if args.vs.endswith('.pt') else None
    summary, results = run(args.vs, args.seeds, args.first_seed, args.procs, args.port, params, args.record, opponent)
    if args.out:
        with open(args.out, 'a') as f:
            f.write(json.dumps(dict(summary=summary, params=params, results=results)) + '\n')


if __name__ == '__main__':
    main()
