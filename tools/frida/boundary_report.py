"""Summarizes a boundary recording: client methods calling rules methods.

    boundary_report.py <calls.bin> [--all]
"""
import collections, struct, sys
rows = []
for line in open('game/dump/symbols.tsv'):
    off, asm, name = line.rstrip('\n').split('\t'); rows.append((int(off, 16), asm, name))
rows.sort()
data = open(sys.argv[1], 'rb').read()
pairs = struct.unpack(f'<{len(data) // 4}I', data)
calls = [(pairs[i], pairs[i + 1]) for i in range(0, len(pairs) - 1, 2)]
first, count = {}, collections.Counter()
for k, (fn, caller) in enumerate(calls):
    key = (rows[fn][2], rows[caller][2])
    count[key] += 1
    first.setdefault(key, k)
print(f'{len(calls)} calls, {len(first)} distinct (rules method, client caller) pairs\n')
for key, k in sorted(first.items(), key=lambda x: x[1]):
    print(f'{k:7d} x{count[key]:<6d} {key[0]:60s} <- {key[1]}')
