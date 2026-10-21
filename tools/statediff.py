"""Diffs two dumped GameStates (JSON from `polyhost dump`) and prints differing paths.

    statediff.py <a.json> <b.json> [max]
"""
import json, sys

def walk(a, b, path, out):
    if type(a) != type(b):
        out.append((path, a, b)); return
    if isinstance(a, dict):
        for k in sorted(set(a) | set(b)):
            if k not in a or k not in b: out.append((f'{path}.{k}', a.get(k, '<missing>'), b.get(k, '<missing>')))
            else: walk(a[k], b[k], f'{path}.{k}', out)
    elif isinstance(a, list):
        if len(a) != len(b): out.append((f'{path}[len]', len(a), len(b)))
        for i, (x, y) in enumerate(zip(a, b)): walk(x, y, f'{path}[{i}]', out)
    elif a != b:
        out.append((path, a, b))

def short(v):
    s = json.dumps(v)
    return s if len(s) < 140 else s[:137] + '...'

a, b = (json.load(open(f)) for f in sys.argv[1:3])
out = []
walk(a, b, '', out)
limit = int(sys.argv[3]) if len(sys.argv) > 3 else 60
print(f'{len(out)} differences')
for p, x, y in out[:limit]:
    print(f'  {p}: {short(x)}  vs  {short(y)}')
