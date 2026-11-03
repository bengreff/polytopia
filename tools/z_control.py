"""The controllability test of strategy conditioning (research log HP1, test T2): reads the facts files
that py/eval_vs_bot.py --facts_out wrote for each z set, and compares what the agent did with what its
z said.

    python3 tools/z_control.py runs/evals/hp1_ctrl/<step>/   (files base.jsonl, fast.jsonl, slow.jsonl, ...)

For each set: games, agent wins, and per fact the agent's mean against the z's mean over the same games:
techs by turn 20 (games that reached turn 20), catapults among units trained by turn 20, a naval unit by
turn 20 (games that reached turn 20), techs by turn 10. Then the B5 checks: the agent's gap between two
sets against the z's own gap ("follows z for at least half of the gap").
"""
import json
import os
import sys


def load(path):
    return [json.loads(l) for l in open(path) if l.strip()]


def summary(rows):
    if any(r.get('z') for r in rows):  # a z set: only the games that got a z (a tribe with no candidate gets none)
        rows = [r for r in rows if r.get('z')]
    s = dict(games=len(rows), wins=sum(r['result'] == 'win' for r in rows))
    r20 = [r for r in rows if r['turns'] >= 20]
    r10 = [r for r in rows if r['turns'] >= 10]
    s['reached_t20'] = len(r20)

    def mean(xs):
        return sum(xs) / len(xs) if xs else float('nan')

    s['techs_t20'] = mean([r['facts'].get('techs_t20', 0) for r in r20])
    s['techs_t10'] = mean([r['facts'].get('techs_t10', 0) for r in r10])
    units = sum(r['facts'].get('units_t20', 0) for r in rows)
    s['catapult_share'] = sum(r['facts'].get('catapult_t20', 0) for r in rows) / max(1, units)
    s['naval_t20'] = mean([r['facts'].get('has_naval', 0) and r['facts'].get('naval_turn', 99) <= 20 for r in r20])
    zs = [r for r in rows if r.get('z')]
    if zs:
        z20 = [r for r in zs if r['turns'] >= 20]
        s['z_techs_t20'] = mean([r['z']['facts']['techs_t20'] for r in z20])
        s['z_techs_t10'] = mean([r['z']['facts']['techs_t10'] for r in zs if r['turns'] >= 10])
        zu = sum(r['z']['facts']['units_t20'] for r in zs)
        s['z_catapult_share'] = sum(r['z']['facts']['catapult_t20'] for r in zs) / max(1, zu)
        s['z_naval_t20'] = mean([r['z']['facts']['has_naval'] and r['z']['facts']['naval_turn'] <= 20 for r in z20])
    return s


def main():
    d = sys.argv[1]
    sets = {f[:-6]: summary(load(os.path.join(d, f))) for f in sorted(os.listdir(d)) if f.endswith('.jsonl')}
    keys = ['games', 'wins', 'reached_t20', 'techs_t10', 'z_techs_t10', 'techs_t20', 'z_techs_t20', 'catapult_share',
            'z_catapult_share', 'naval_t20', 'z_naval_t20']
    print('set'.ljust(10) + ''.join(k.rjust(17) for k in keys))
    for name, s in sets.items():
        print(name.ljust(10) + ''.join((f'{s[k]:.3f}' if isinstance(s.get(k), float) else str(s.get(k, '-'))).rjust(17)
                                       for k in keys))

    def follow(a, b, k):
        if a not in sets or b not in sets or f'z_{k}' not in sets[a] or f'z_{k}' not in sets[b]:
            return
        agent = sets[a][k] - sets[b][k]
        z = sets[a][f'z_{k}'] - sets[b][f'z_{k}']
        print(f'{k}: {a} - {b}: agent {agent:+.3f}, z {z:+.3f}, followed {agent / z if z else float("nan"):.0%} of the gap')

    print()
    follow('fast', 'slow', 'techs_t20')
    follow('fast', 'slow', 'techs_t10')
    follow('manycat', 'nocat', 'catapult_share')
    follow('earlyport', 'nonavy', 'naval_t20')


if __name__ == '__main__':
    main()
