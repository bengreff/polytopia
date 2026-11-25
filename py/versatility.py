"""Versatility of the networks' play vs the human championship games (research log "Checkpoint 1"), with py/strategy_positions.py's
18 strategy detectors (unchanged). Model sets use only self-play games (both seats the same network,
so both players' events exist) that start from turn 0 (not strategy starts).

    .venv/bin/python py/versatility.py <out.json> human=runs/strategy/events.pkl gen000=<games dir> gen107=<dir> ...

The length-matched numbers in the log come from py/versatility_records.py and py/versatility_report.py.
"""
import collections
import json
import math
import multiprocessing as mp
import os
import pickle
import sys
import types

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import polyenv  # noqa: E402
import strategy_positions as sp  # noqa: E402

TRIBE_NAME = {v: k for k, v in polyenv.TRIBES.items()}


class SelfPlayGame:
    """A py/selfplay.py npz seen through the human-game keys strategy_positions.extract reads."""
    def __init__(self, path):
        self.z = np.load(path)
        self.extra = dict(game_id=os.path.basename(path)[:-4], game_version=0, resigned=0,
                          final_check='bytes', final_diff_fields=np.array([], dtype='<U1'))

    def __getitem__(self, k):
        if k == 'label':
            return self.z['played']
        if k in self.extra:
            return np.asarray(self.extra[k])
        return self.z[k]


_np_proxy = types.SimpleNamespace(**{k: getattr(np, k) for k in dir(np) if not k.startswith('__')})
_np_proxy.load = lambda p, *a, **kw: SelfPlayGame(p)


def extract_sp(path):
    z = np.load(path)
    if 'start_turn' in z.files and int(z['start_turn']) > 0:
        return None
    if set(int(x) for x in np.unique(z['player'])) != {1, 2}:  # both seats recorded = self-play
        return None
    ended, turns = bool(z['ended']), int(z['turns'])
    real = sp.np
    sp.np = _np_proxy
    try:
        g = sp.extract(path)
    finally:
        sp.np = real
    g['meta']['ended'] = ended
    return g


def episodes(g):
    return g['meta'], sp.game_episodes(g, min_ratio=0.4)


def summarize(name, games_eps):
    kinds = list(sp.DETECTORS)
    pg = []  # one entry per player-game
    for meta, eps in games_eps:
        for p in (1, 2):
            mine = [e for e in eps if e['player'] == p]
            pg.append(dict(tribe=meta['tribes'][p - 1], won=meta['winner'] == p, turns=meta['turns'],
                           att={e['kind'] for e in mine}, ok={e['kind'] for e in mine if e['success']}))
    n = len(pg)
    out = dict(name=name, games=len(games_eps), player_games=n,
               turns_median=float(np.median([x['turns'] for x in pg])) if pg else 0)
    out['attempt'] = {k: sum(k in x['att'] for x in pg) / n for k in kinds}
    out['success'] = {k: sum(k in x['ok'] for x in pg) / n for k in kinds}
    out['kinds_per_player_game'] = float(np.mean([len(x['ok']) for x in pg]))
    out['attempted_kinds_per_player_game'] = float(np.mean([len(x['att']) for x in pg]))
    # mix of successful kinds over all player-games: normalized entropy and effective number of kinds
    tot = collections.Counter(k for x in pg for k in x['ok'])
    s = sum(tot.values())
    ps = [c / s for c in tot.values() if c]
    h = -sum(q * math.log(q) for q in ps)
    out['mix_entropy_norm'] = h / math.log(len(kinds))
    out['effective_kinds'] = math.exp(h)
    out['kinds_used_1pct'] = sum(1 for k in kinds if out['success'][k] >= 0.01)
    # situation: how much the choice depends on the tribe. For each kind, mutual information between
    # tribe and "used it (successfully)", summed over kinds, in bits; and per-tribe rates for the
    # tribes with >= 40 player-games
    by_t = collections.defaultdict(list)
    for x in pg:
        by_t[x['tribe']].append(x)
    tribes = [t for t, xs in by_t.items() if len(xs) >= 40]
    m = sum(len(by_t[t]) for t in tribes)
    mi = 0.0
    for k in kinds:
        pk = sum(k in x['ok'] for t in tribes for x in by_t[t]) / m if m else 0
        for t in tribes:
            pt = len(by_t[t]) / m
            for used in (True, False):
                pkt = sum((k in x['ok']) == used for x in by_t[t]) / len(by_t[t])
                pu = pk if used else 1 - pk
                if pkt > 0 and pu > 0:
                    mi += pt * pkt * math.log2(pkt / pu)
    out['tribe_mi_bits'] = mi
    out['per_tribe'] = {TRIBE_NAME.get(t, str(t)): dict(n=len(by_t[t]), win=sum(x['won'] for x in by_t[t]) / len(by_t[t]),
                                                         top={k: round(sum(k in x['ok'] for x in by_t[t]) / len(by_t[t]), 3)
                                                              for k in kinds})
                        for t in sorted(tribes)}
    # does using a kind go with winning? win rate of player-games with a successful episode of the kind
    out['win_given'] = {k: (sum(x['won'] for x in pg if k in x['ok']) / max(1, sum(k in x['ok'] for x in pg)))
                        for k in kinds}
    return out


def main():
    out_path, sets = sys.argv[1], [a.split('=', 1) for a in sys.argv[2:]]
    procs = int(os.environ.get('PROCS', '2'))
    res = []
    with mp.Pool(procs) as pool:
        for name, src in sets:
            if src.endswith('.pkl'):
                d = pickle.load(open(src, 'rb'))
                games = [g for g in d.values() if g['turns'] is not None and g['meta']['exact'] and g['meta']['turns'] >= 5]
            else:
                files = sorted(os.path.join(src, f) for f in os.listdir(src) if f.endswith('.npz'))
                games = [g for g in pool.imap_unordered(extract_sp, files, chunksize=8) if g is not None]
            eps = list(pool.imap_unordered(episodes, games, chunksize=16))
            r = summarize(name, eps)
            res.append(r)
            print(f"{name}: {r['games']} games, {r['player_games']} player-games, median {r['turns_median']} turns, "
                  f"{r['kinds_per_player_game']:.2f} kinds/player-game, effective kinds {r['effective_kinds']:.1f}, "
                  f"tribe MI {r['tribe_mi_bits']:.3f} bits", flush=True)
    json.dump(res, open(out_path, 'w'), indent=1)


if __name__ == '__main__':
    main()
