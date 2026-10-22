"""Ranks recorded games by how back-and-forth they were, for the web app's watch list.

Every game (runs/games/<tag>/<seed>_s<seat>.start/.final, from eval_vs_bot.py or selfplay.py
--save_games) is replayed through the game's code. At each turn the two sides' territory (tiles
each owns, the true state) gives the lead, (network - opponent) / (both). Added to the game's line
in games.jsonl:
- lead_changes: how often the lead flipped (it has to pass 5% each way to count);
- comeback: the deepest deficit the eventual winner climbed out of (0 if it was never behind);
- swing = lead_changes + 4 * comeback; the watch list can sort by it;
- leads: the lead per turn, for a sparkline.
Duplicate lines of the same game (restarted evaluations) are merged, the last kept.

    POLYHOST=mac .venv/bin/python py/rank_games.py runs/games/sp_s2_crazy_g000 runs/games/r4_20k_watch_d3 \
        [--rename "network (gen000.pt)=network (R7)"]
"""
import argparse
import concurrent.futures as cf
import json
import os
import queue

import numpy as np

import polyenv


def lead_stats(leads, winner_is_agent):
    """leads: per turn, (agent - opponent) / both. The winner's comeback is read from its side."""
    flips, side = 0, 0
    for x in leads:
        s = 1 if x > 0.05 else -1 if x < -0.05 else 0
        if s and side and s != side:
            flips += 1
        if s:
            side = s
    w = np.array(leads) if winner_is_agent else -np.array(leads)
    comeback = float(max(0.0, -w.min())) if len(w) else 0.0
    return flips, comeback


def analyse(env, d, g):
    name = g['path'].split('/', 1)[1]
    start = open(os.path.join(d, name + '.start'), 'rb').read()
    final = open(os.path.join(d, name + '.final'), 'rb').read()
    agent = int(g.get('agent', 1))
    spec = polyenv.Spec(tribes=tuple(g['tribes']), bots=(None, None), seed=int(g['seed']))
    left, st = env.replay(0, spec, start, final)
    leads, turn = [], st.turn
    while left > 0:
        _, _, _, ok, left, st = env.replay_step(0)
        if st.turn != turn:
            turn = st.turn
            owner, _ = env.owners(0)
            a, b = int((owner == agent).sum()), int((owner == 3 - agent).sum())
            leads.append(round((a - b) / max(a + b, 1), 3))
    leads = leads[2:]  # the first turns are all equal starts
    flips, comeback = lead_stats(leads, g.get('result') == 'win')
    return dict(lead_changes=flips, comeback=round(comeback, 3), swing=round(flips + 4 * comeback, 2), leads=leads)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dirs', nargs='+')
    ap.add_argument('--procs', type=int, default=4)
    ap.add_argument('--port', type=int, default=9220)
    ap.add_argument('--rename', action='append', default=[], help='"old player label=new label"')
    args = ap.parse_args()
    renames = dict(r.split('=', 1) for r in args.rename)
    work = []
    for d in args.dirs:
        lines = {}
        for l in open(os.path.join(d, 'games.jsonl')):
            if l.strip():
                g = json.loads(l)
                lines[g['path']] = g
        for g in lines.values():
            g['players'] = {k: renames.get(v, v) for k, v in g.get('players', {}).items()}
        work.append((d, lines))
    jobs = queue.Queue()
    for d, lines in work:
        for g in lines.values():
            jobs.put((d, g))
    total = jobs.qsize()
    done = [0]

    def worker(i):
        with polyenv.launch(args.port + i) as env:
            while True:
                try:
                    d, g = jobs.get_nowait()
                except queue.Empty:
                    return
                try:
                    g.update(analyse(env, d, g))
                except Exception as e:  # a game that doesn't replay is listed without a rank
                    print(f"{g['path']}: {e}", flush=True)
                done[0] += 1
                if done[0] % 50 == 0:
                    print(f'{done[0]}/{total} games', flush=True)

    with cf.ThreadPoolExecutor(args.procs) as pool:
        list(pool.map(worker, range(args.procs)))
    for d, lines in work:
        path = os.path.join(d, 'games.jsonl')
        with open(path + '.tmp', 'w') as f:
            for g in sorted(lines.values(), key=lambda g: g.get('time', '')):
                f.write(json.dumps(g) + '\n')
        os.replace(path + '.tmp', path)
        wins = sorted((g for g in lines.values() if g.get('result') == 'win' and 'swing' in g), key=lambda g: -g['swing'])
        print(f'\n{d}: {len(lines)} games; most back-and-forth wins:')
        for g in wins[:8]:
            print(f"  {g['path']}: swing {g['swing']}, {g['lead_changes']} lead changes, comeback {g['comeback']:.0%}, "
                  f"{g['turns']} turns")


if __name__ == '__main__':
    main()
