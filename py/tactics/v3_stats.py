"""Statistics for planner v3's test run (research log "Planner v3"): reads every results file in the run folder and
writes stats.json (everything) and stats.md (the readable table) next to them. Standard library only, so the driver
can run it with any python3: python3 v3_stats.py ~/runs/v3

Each results line is one game (py/tactics/match.py): seed, seat (side A = planner v3 plays player seat+1, tribe
tribes[seat]), res (win / loss / cap / error), turns, wall seconds, and per-side stats (seconds, commands, lanes...).
"""
import glob
import json
import math
import os
import sys
import time

# The pre-registered plan (research log "Planner v3", 2026-11-24): games planned per file, and the bars.
PLAN = [('screen_bot', 100, 'v2', 'screen: v3 with gen 107 as reply model'),
        ('screen_r7', 100, 'v2', 'screen: v3 with R7 as reply model'),
        ('gate_bot', 400, 'v2', 'gate: v3 (gen 107 reply)'),
        ('gate_r7', 400, 'v2', 'gate: v3 (R7 reply)'),
        ('vs_gen0_bot', 100, 'raw gen 0 (R7)', 'no-collapse check'),
        ('vs_gen0_r7', 100, 'raw gen 0 (R7)', 'no-collapse check'),
        ('vs_crazyplus_bot', 100, 'CrazyPlus', 'no-collapse check'),
        ('vs_crazyplus_r7', 100, 'CrazyPlus', 'no-collapse check')]


def wilson(w, n, z=1.96):
    if n == 0:
        return 0.0, 0.0
    p = w / n
    d = 1 + z * z / n
    c = p + z * z / (2 * n)
    r = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n))
    return (c - r) / d, (c + r) / d


def pct(xs, q):
    xs = sorted(xs)
    if not xs:
        return None
    k = (len(xs) - 1) * q / 100
    lo, hi = math.floor(k), math.ceil(k)
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)


def side(games, key):
    st = [g[key] for g in games if g.get(key)]
    turns = sum(s['turns'] for s in st)
    secs = sum(s['seconds'] for s in st)
    cmds = sum(s.get('commands', 0) for s in st)
    kinds = {}
    for s in st:
        for k, v in s.get('kinds', {}).items():
            kinds[k] = kinds.get(k, 0) + v
    return dict(turns=turns, seconds=round(secs), s_per_turn=round(secs / max(turns, 1), 2),
                s_per_command=round(secs / cmds, 3) if cmds else None,
                max_turn_s=round(max((s['max_seconds'] for s in st), default=0), 1),
                replans_per_game=round(sum(s.get('replans', 0) for s in st) / max(len(st), 1), 1),
                lanes=dict(sorted(kinds.items(), key=lambda kv: -kv[1])))


def summarize(path, planned):
    lines = [json.loads(l) for l in open(path) if l.strip()]
    good = {}
    for r in lines:
        if r.get('res') != 'error':
            good[(r['seed'], r['seat'])] = r
    errors = [r for r in lines if r.get('res') == 'error' and (r['seed'], r['seat']) not in good]
    g = list(good.values())
    n = len(g)
    w = sum(r['res'] == 'win' for r in g)
    lo, hi = wilson(w, n)
    by_seat = {}
    for s in (0, 1):
        gs = [r for r in g if r['seat'] == s]
        by_seat[f'seat{s}'] = dict(games=len(gs), wins=sum(r['res'] == 'win' for r in gs))
    seeds = {}
    for r in g:
        seeds.setdefault(r['seed'], []).append(r['res'] == 'win')
    pairs = [v for v in seeds.values() if len(v) == 2]
    by_tribe = {}
    for r in g:
        t = str(r['tribes'][r['seat']])
        d = by_tribe.setdefault(t, dict(games=0, wins=0))
        d['games'] += 1
        d['wins'] += r['res'] == 'win'
    secs = [r['seconds'] for r in g]
    return dict(file=os.path.basename(path), planned=planned, games=n, complete=n >= planned, wins=w,
                losses=sum(r['res'] == 'loss' for r in g), capped=sum(r['res'] == 'cap' for r in g),
                errors_unretried=len(errors), win_rate=round(w / n, 4) if n else None,
                ci95=[round(lo, 4), round(hi, 4)], by_seat=by_seat,
                seeds_paired=dict(both_won=sum(all(v) for v in pairs), split=sum(any(v) and not all(v) for v in pairs),
                                  both_lost=sum(not any(v) for v in pairs)),
                game_turns=dict(median=pct([r['turns'] for r in g], 50), mean=round(sum(r['turns'] for r in g) / max(n, 1), 1)),
                game_seconds=dict(median=pct(secs, 50), p90=pct(secs, 90), max=max(secs, default=None),
                                  total_hours=round(sum(secs) / 3600, 1)),
                v3=side(g, 'a_stats'), opponent=side(g, 'b_stats'),
                v3_by_tribe=dict(sorted(by_tribe.items(), key=lambda kv: int(kv[0]))))


def verdicts(res):
    out = []
    for name, s in res.items():
        if s['games'] == 0:
            continue
        tag = '' if s['complete'] else f" (partial: {s['games']} of {s['planned']} games)"
        rate, lo = s['win_rate'], s['ci95'][0]
        if name.startswith('gate_'):
            ok = rate >= 0.55 and lo >= 0.50
            out.append(f"{name}: {rate:.1%}, 95% CI {lo:.1%}-{s['ci95'][1]:.1%}: "
                       f"{'meets' if ok else 'misses'} Ben's bar (>= 55%, CI low >= 50%); "
                       f"prediction (60-70%) {'falsified' if rate < 0.55 else 'not falsified'}{tag}")
        elif name.startswith('vs_gen0_'):
            out.append(f"{name}: {rate:.1%}: {'passes' if rate >= 0.90 else 'fails'} the >= 90% check{tag}")
        elif name.startswith('vs_crazyplus_'):
            out.append(f"{name}: {rate:.1%}: {'passes' if rate >= 0.95 else 'fails'} the >= 95% check{tag}")
    return out


def main():
    d = os.path.expanduser(sys.argv[1] if len(sys.argv) > 1 else '~/runs/v3')
    res = {}
    for name, planned, opp, what in PLAN:
        p = os.path.join(d, name + '.jsonl')
        if os.path.exists(p):
            s = summarize(p, planned)
            s.update(opponent_name=opp, what=what)
            res[name] = s
    for p in sorted(glob.glob(os.path.join(d, '*.jsonl'))):  # anything else in the folder
        name = os.path.basename(p)[:-6]
        if name not in res:
            res[name] = summarize(p, 0)
    done = sum(s['games'] for s in res.values())
    planned = 200 + 400 + 200
    out = dict(written=time.strftime('%Y-%m-%d %H:%M:%S %Z'), games_done=done, games_planned=planned,
               finished=os.path.exists(os.path.join(d, 'DONE')), verdicts=verdicts(res), results=res)
    json.dump(out, open(os.path.join(d, 'stats.json.tmp'), 'w'), indent=1)
    os.replace(os.path.join(d, 'stats.json.tmp'), os.path.join(d, 'stats.json'))
    md = [f"# Planner v3 test: statistics ({out['written']})", '',
          f"{'Finished' if out['finished'] else 'Running or stopped'}; {done} of {planned} planned games recorded "
          '(screens 200, gate 400, checks 200). All numbers measured from the results files.', '']
    if out['verdicts']:
        md += ['## Verdicts', ''] + [f'- {v}' for v in out['verdicts']] + ['']
    md += ['## Results', '',
           '| file | vs | games | v3 wins | rate | 95% CI | seat 0 | seat 1 | seeds won both / split / lost both | v3 s per turn | v3 s per command | median game, s |',
           '|---|---|---|---|---|---|---|---|---|---|---|---|']
    for name, s in res.items():
        if s['games'] == 0:
            continue
        b = s['by_seat']
        sp = s['seeds_paired']
        md.append(f"| {name} | {s.get('opponent_name', '?')} | {s['games']}/{s['planned']} | {s['wins']} | {s['win_rate']:.1%} | "
                  f"{s['ci95'][0]:.1%}-{s['ci95'][1]:.1%} | {b['seat0']['wins']}/{b['seat0']['games']} | "
                  f"{b['seat1']['wins']}/{b['seat1']['games']} | {sp['both_won']} / {sp['split']} / {sp['both_lost']} | "
                  f"{s['v3']['s_per_turn']} | {s['v3']['s_per_command']} | {s['game_seconds']['median']} |")
    md += ['', 'Lanes v3 played (turns), per file:', '']
    for name, s in res.items():
        if s['games']:
            md.append(f"- {name}: {s['v3']['lanes']}; re-plans per game {s['v3']['replans_per_game']}; "
                      f"capped games {s['capped']}; unretried errors {s['errors_unretried']}")
    open(os.path.join(d, 'stats.md'), 'w').write('\n'.join(md) + '\n')
    print('\n'.join(out['verdicts']) or f'{done} games, no verdicts yet')


if __name__ == '__main__':
    main()
