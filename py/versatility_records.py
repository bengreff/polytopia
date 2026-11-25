"""Per player-game records for the versatility analysis (py/versatility_report.py reads them): tribe, result, length, successful strategy
episodes (kind, t0), and the water share of the player's explored tiles at its turn 8 (the situation)."""
import os, pickle, sys, random
import multiprocessing as mp
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import versatility as V
sp = V.sp
TI = {n: i for i, n in enumerate(V.polyenv.TILE_FEATURES)}
GI = {n: i for i, n in enumerate(V.polyenv.GLOBAL_FEATURES)}
HUMAN = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'data', 'human1')

# .venv/bin/python py/versatility_records.py recs.pkl human=runs/strategy/events.pkl gen000=<dir> gen107=<dir> ...

def water8(path):
    """{player: water share of its explored tiles at its first decision of turn 8 (or its last turn before)}"""
    z = np.load(path)
    turn, player = z['globals'][:, GI['turn']], z['player']
    out = {}
    for p in (1, 2):
        idx = np.nonzero((player == p) & (turn <= 8))[0]
        if not len(idx):
            continue
        t = turn[idx].max()
        d = idx[turn[idx] == t][0]
        tl = z['tiles'][d]
        ex = tl[:, TI['explored']] > 0
        if ex.sum():
            out[p] = float(np.isin(tl[ex, TI['terrain']], sp.WATER_TERRAIN).mean())
    return out

def records(args):
    name, g, path = args
    meta, eps = V.episodes(g)
    w = water8(path) if path else {}
    out = []
    for p in (1, 2):
        out.append(dict(set=name, game=meta['game'], p=p, tribe=meta['tribes'][p - 1], opp=meta['tribes'][2 - p],
                        won=meta['winner'] == p, turns=meta['turns'], water=w.get(p),
                        ok=[(e['kind'], e['t0']) for e in eps if e['player'] == p and e['success']]))
    return out

def model_job(path):
    g = V.extract_sp(path)
    return None if g is None else (g, path)

def main():
    out, sets = sys.argv[1], [a.split('=', 1) for a in sys.argv[2:]]
    recs = []
    with mp.Pool(int(os.environ.get('PROCS', '3'))) as pool:
        for name, src in sets:
            if src.endswith('.pkl'):
                d = pickle.load(open(src, 'rb'))
                gs = [g for g in d.values() if g['turns'] is not None and g['meta']['exact'] and g['meta']['turns'] >= 5]
                random.Random(0).shuffle(gs)
                gs = gs[:int(os.environ.get('HUMAN_MAX', '2500'))]
                jobs = [(name, g, os.path.join(HUMAN, g['meta']['game'] + '.npz')) for g in gs]
            else:
                files = sorted(os.path.join(src, f) for f in os.listdir(src) if f.endswith('.npz'))
                jobs = [(name, g, p) for x in pool.imap_unordered(model_job, files, chunksize=8) if x for g, p in [x]]
            for r in pool.imap_unordered(records, jobs, chunksize=8):
                recs += r
            print(name, len(jobs), 'games', flush=True)
    pickle.dump(recs, open(out, 'wb'))

if __name__ == '__main__':
    main()
