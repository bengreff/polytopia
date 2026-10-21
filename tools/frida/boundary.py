"""Records the client's calls into the rules code while the real app runs.

    boundary.py <out.bin> [--attach]

Writes out.bin as little-endian uint32 pairs (rules method index, caller method index) into
game/dump/symbols.tsv sorted by offset, and prints a running count. Hooks are installed at
launch, before any game exists; the game is driven by hand or by adb taps meanwhile.
"""
import json, os, struct, sys, time
import frida

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PACKAGE = 'air.com.midjiwan.polytopia'

def tables():
    rows = []
    for line in open(os.path.join(ROOT, 'game/dump/symbols.tsv')):
        off, asm, name = line.rstrip('\n').split('\t')
        rows.append((int(off, 16), asm, name))
    rows.sort()
    asm_ids = {a: i for i, a in enumerate(sorted({r[1] for r in rows}))}
    starts = [r[0] for r in rows]
    owner = [asm_ids[r[1]] for r in rows]
    seen = set()
    hooks = []
    for i, (off, asm, name) in enumerate(rows):
        if asm == 'GameLogicAssembly.dll' and off not in seen:
            seen.add(off)
            hooks.append([i, off])
    return rows, {'starts': starts, 'owner': owner, 'hooks': hooks, 'client': asm_ids['PolytopiaAssembly.dll']}

def main():
    out = open(sys.argv[1], 'ab')
    rows, t = tables()
    device = frida.get_usb_device(timeout=10)
    if '--attach' in sys.argv:
        app = next(a for a in device.enumerate_applications() if a.identifier == PACKAGE and a.pid)
        pid, spawned = app.pid, False
    else:
        pid, spawned = device.spawn([PACKAGE]), True
    session = device.attach(pid)
    script = session.create_script(open(os.path.join(ROOT, 'tools/frida/boundary.js')).read())
    total = [0]
    def on_message(msg, data):
        if msg['type'] != 'send':
            print('agent:', msg.get('description') or msg, flush=True)
            return
        p = msg['payload']
        if p['type'] == 'loaded':
            script.post({'type': 'tables', **t})
        elif p['type'] == 'ready':
            print('hooked', p['hooked'], 'rules methods,', p['failed'], 'too small to hook', flush=True)
        elif p['type'] == 'stats':
            print('stats', p, 'client id', t['client'], flush=True)
        elif p['type'] == 'calls':
            out.write(data); out.flush()
            total[0] += p['n']
    script.on('message', on_message)
    script.load()
    if spawned:  # the rules library loads after resuming; hooks go in well before any game starts
        device.resume(pid)
    print('recording; Ctrl-C to stop', flush=True)
    last = 0
    try:
        while True:
            time.sleep(5)
            if total[0] != last:
                print('calls', total[0], flush=True)
                last = total[0]
    except KeyboardInterrupt:
        pass

if __name__ == '__main__':
    main()
