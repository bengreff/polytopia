"""Compares game content of dumped states, ignoring what the client's UI changes as a side effect.

    statecompare.py <dir>     (pairs ours_<n>.json / real_<n>.json written by `polyhost replay`)

Ignored: dictionary layout (compared as key/value maps), default-valued entries created on
lookup, AI scratch state (aiState, opinions) and caches, which the advisor and UI fill in.
"""
import glob, json, os, re, sys

IGNORE = {'aiState', 'opinions', 'unlockedTechCache', 'capitalCountCache', 'IsDirty'}
DEFAULT_RELATION = {'State': 0, 'LastAttackTurn': -100, 'EmbassyLevel': 0, 'LastPeaceBrokenTurn': -100,
                    'FirstMeet': -100, 'EmbassyBuildTurn': -100, 'PreviousAttackTurn': -100}

def canon(v, key=None):
    if isinstance(v, dict):
        if '_entries' in v and '_count' in v:  # System.Collections.Generic.Dictionary
            entries = (v['_entries'] or [])[:v['_count']]
            out = {json.dumps(e['key'], sort_keys=True): canon(e['value']) for e in entries if e.get('hashCode', 0) >= 0}
            if key == 'relations':
                out = {k: x for k, x in out.items() if x != DEFAULT_RELATION}
            return out
        return {k: canon(x, k) for k, x in v.items() if k not in IGNORE}
    if isinstance(v, list):
        return [canon(x) for x in v]
    return v

def diff(a, b, path, out):
    if type(a) != type(b): out.append((path, a, b)); return
    if isinstance(a, dict):
        for k in sorted(set(a) | set(b)):
            if k not in a or k not in b: out.append((f'{path}.{k}', a.get(k, '<missing>'), b.get(k, '<missing>')))
            else: diff(a[k], b[k], f'{path}.{k}', out)
    elif isinstance(a, list):
        if len(a) != len(b): out.append((f'{path}[len]', len(a), len(b)))
        for i, (x, y) in enumerate(zip(a, b)): diff(x, y, f'{path}[{i}]', out)
    elif a != b: out.append((path, a, b))

if __name__ == '__main__':  # importable for canon() and diff()
    d = sys.argv[1]
    steps = sorted(int(re.search(r'_(\d+)\.json', f).group(1)) for f in glob.glob(os.path.join(d, 'real_*.json')))
    bad = 0
    for n in steps:
        a = canon(json.load(open(os.path.join(d, f'real_{n}.json'))))
        b = canon(json.load(open(os.path.join(d, f'ours_{n}.json'))))
        out = []
        diff(a, b, '', out)
        if out:
            bad += 1
            if bad <= 3:
                print(f'command {n}: {len(out)} content differences')
                for p, x, y in out[:12]: print(f'   {p}: real {json.dumps(x)[:100]}  ours {json.dumps(y)[:100]}')
    print(f'{len(steps)} byte-different states: {len(steps) - bad} identical in content, {bad} differ')

    if '--summary' in sys.argv:
        import collections
        kinds = collections.Counter(); first = {}
        for n in steps:
            a = canon(json.load(open(os.path.join(d, f'real_{n}.json'))))
            b = canon(json.load(open(os.path.join(d, f'ours_{n}.json'))))
            out = []; diff(a, b, '', out)
            for p, x, y in out:
                k = re.sub(r'\[\d+\]', '[]', p)
                kinds[k] += 1; first.setdefault(k, (n, p, x, y))
        print('\ndifference kinds (path, count, first command):')
        for k, c in kinds.most_common(40):
            n, p, x, y = first[k]
            print(f'  {c:6d}  {k}   first at command {n}: real {json.dumps(x)[:60]} ours {json.dumps(y)[:60]}')
