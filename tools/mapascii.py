"""Prints a dumped GameState's map as text: . ocean, ~ shallow water, letters for land.

    mapascii.py <state.json>
"""
import json, sys
TERRAIN = {0: '?', 1: '~', 2: '.', 3: 'f', 4: 'M', 5: 'T', 6: 'I', 7: 'w', 8: 'g'}
d = json.load(open(sys.argv[1]))
m = d['Map']
w, h = m['width'], m['height']
grid = [[' '] * w for _ in range(h)]
for t in m['tiles']:
    x, y = t['coordinates']['x'], t['coordinates']['y']
    c = TERRAIN.get(t['terrain'], '?')
    if t.get('capitalOf'): c = '@'
    elif t.get('improvement') and t['improvement']['type'] == 1: c = 'v'
    grid[y][x] = c
water = sum(1 for t in m['tiles'] if t['terrain'] in (1, 2))
print(f"{w}x{h}  water {water / len(m['tiles']):.0%}  seed {d['Seed']}  preset {d['Settings']['mapPreset']}")
for row in grid: print(' '.join(row))
