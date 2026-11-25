"""The length-matched versatility report (research log "Checkpoint 1"): player-games in games of at least 20
turns, strategy episodes started by turn 20. Diversity (effective number of kinds overall and within a tribe),
tribe dependence (mutual information, with a shuffled-tribe baseline for the small-sample bias), and naval play by
the water share of the explored map at turn 8 (cut at the human terciles).

    .venv/bin/python py/versatility_report.py recs.pkl [out.json]
"""
import pickle, math, random, collections, json, sys
import numpy as np
recs = pickle.load(open(sys.argv[1] if len(sys.argv) > 1 else 'recs.pkl', 'rb'))
KINDS = sorted({k for r in recs for k, _ in r['ok']})
NAVAL = {'naval_landing', 'naval_expansion', 'naval_battle', 'naval_bombard'}
sets = ['human', 'gen000', 'gen107', 'gen110']
W = 20
def kinds(r): return {k for k, t0 in r['ok'] if t0 <= W}
def mi(xs, labels):
    n = len(xs); tot = 0.0
    by = collections.defaultdict(list)
    for x, l in zip(xs, labels): by[l].append(x)
    for k in KINDS:
        pk = sum(k in x for x in xs) / n
        for l, g in by.items():
            pl = len(g) / n
            for used in (True, False):
                pkl = sum((k in x) == used for x in g) / len(g); pu = pk if used else 1 - pk
                if pkl > 0 and pu > 0: tot += pl * pkl * math.log2(pkl / pu)
    return tot
def eff(xs):
    c = collections.Counter(k for x in xs for k in x); s = sum(c.values())
    return math.exp(-sum(v / s * math.log(v / s) for v in c.values())) if s else 0
res = {}
hw = sorted(r['water'] for r in recs if r['set'] == 'human' and r['water'] is not None)
cut = (hw[len(hw) // 3], hw[2 * len(hw) // 3])
for s in sets:
    rs = [r for r in recs if r['set'] == s and r['turns'] >= W]
    xs = [kinds(r) for r in rs]
    tr = [r['tribe'] for r in rs]
    big = [t for t, c in collections.Counter(tr).items() if c >= (30 if s != 'gen110' else 1)]
    sel = [i for i, t in enumerate(tr) if t in big]
    xs_t, tr_t = [xs[i] for i in sel], [tr[i] for i in sel]
    m = mi(xs_t, tr_t)
    rng = random.Random(0); base = []
    for _ in range(5):
        sh = tr_t[:]; rng.shuffle(sh); base.append(mi(xs_t, sh))
    within = np.average([eff([x for x, t in zip(xs_t, tr_t) if t == T]) for T in big],
                        weights=[tr_t.count(T) for T in big])
    top = np.average([collections.Counter(frozenset(x) for x, t in zip(xs_t, tr_t) if t == T).most_common(1)[0][1] / tr_t.count(T) for T in big],
                     weights=[tr_t.count(T) for T in big])
    bins = [[], [], []]
    for r, x in zip(rs, xs):
        if r['water'] is None: continue
        b = 0 if r['water'] < cut[0] else 1 if r['water'] < cut[1] else 2
        bins[b].append(bool(x & NAVAL))
    res[s] = dict(player_games=len(rs), tribes=len(big), kinds_per=float(np.mean([len(x) for x in xs])),
                  none=float(np.mean([len(x) == 0 for x in xs])), effective=eff(xs), within_tribe_effective=float(within),
                  top_plan_share=float(top), tribe_mi=m, tribe_mi_shuffled=float(np.mean(base)),
                  naval_by_water=[(round(float(np.mean(b)), 3), len(b)) for b in bins],
                  share={k: float(np.mean([k in x for x in xs])) for k in KINDS})
print('water terciles (human, explored tiles at turn 8):', [round(c, 3) for c in cut])
for s in sets:
    r = res[s]
    print(f"{s:7} n={r['player_games']:5} kinds/pg {r['kinds_per']:.2f} none {r['none']:.2f} eff {r['effective']:.1f} "
          f"within-tribe eff {r['within_tribe_effective']:.1f} top-plan {r['top_plan_share']:.2f} "
          f"tribe MI {r['tribe_mi']:.3f} (shuffled {r['tribe_mi_shuffled']:.3f}) naval by water {r['naval_by_water']}")
print('kind'.ljust(16) + ''.join(s.rjust(8) for s in sets))
for k in KINDS:
    print(k.ljust(16) + ''.join(f"{res[s]['share'][k]*100:7.1f}%" for s in sets))
json.dump(res, open(sys.argv[2] if len(sys.argv) > 2 else 'versatility.json', 'w'), indent=1)
