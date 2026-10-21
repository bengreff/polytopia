"""Runs the real Polytopia app under Frida with tools/frida/agent.js and records hooked calls.

    trace.py <hooks.json> <out_dir> [--attach]

Writes out_dir/trace.jsonl (one call per line, call chains symbolized from
game/dump/symbols.tsv) and out_dir/state_<n>.bin (GameState snapshots, if a hook asks).
"""
import bisect, json, os, sys, time
import frida

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PACKAGE = 'air.com.midjiwan.polytopia'

def load_symbols():
    rows = []
    for line in open(os.path.join(ROOT, 'game/dump/symbols.tsv')):
        off, _, name = line.rstrip('\n').split('\t')
        rows.append((int(off, 16), name))
    rows.sort()
    return [r[0] for r in rows], [r[1] for r in rows]

def main():
    hooks = json.load(open(sys.argv[1]))
    out_dir = sys.argv[2]
    # --program <path> [args...]: trace a native process (e.g. polyhost) instead of the app.
    program = sys.argv[sys.argv.index('--program') + 1:] if '--program' in sys.argv else None
    # --autoplay <games.json>: the app plays the local seat itself through these games.
    autoplay = sys.argv[sys.argv.index('--autoplay') + 1] if '--autoplay' in sys.argv else None
    events = []
    os.makedirs(out_dir, exist_ok=True)
    offs, names = load_symbols()
    def sym(rva):
        i = bisect.bisect_right(offs, int(rva, 16)) - 1
        return f'{names[i]}+{int(rva, 16) - offs[i]:#x}' if i >= 0 else rva
    trace = open(os.path.join(out_dir, 'trace.jsonl'), 'a')
    global trace_lines
    trace_lines = [0]
    n = [0]
    device = frida.get_usb_device(timeout=10)
    if program:  # a native process started separately; attach by name
        proc = None
        for _ in range(50):
            proc = next((p for p in device.enumerate_processes() if p.name == os.path.basename(program[0])), None)
            if proc: break
            time.sleep(0.2)
        pid = proc.pid
        session = device.attach(pid)
    elif '--attach' in sys.argv:
        app = next(a for a in device.enumerate_applications() if a.identifier == PACKAGE and a.pid)
        session = device.attach(app.pid)
        pid = None
    else:
        pid = device.spawn([PACKAGE])
        session = device.attach(pid)
    script = session.create_script(open(os.path.join(ROOT, 'tools/frida/agent.js')).read())
    def on_message(msg, data):
        if msg['type'] != 'send':
            print('agent:', msg.get('description') or msg, flush=True)
            return
        p = msg['payload']
        if p.get('type') == 'runtime':
            print('runtime up, installing', len(hooks), 'hooks', flush=True)
            script.post({'type': 'hooks', 'hooks': hooks})
            return
        if p.get('type') in ('ready', 'error'):
            print(p, flush=True)
            if p.get('type') == 'ready' and autoplay is not None:
                script.post({'type': 'autoplay'})
            return
        if str(p.get('type', '')).startswith('autoplay') or p.get('type') == 'configured':
            events.append(p)
            print(json.dumps(p), flush=True)
            trace.write(json.dumps(p) + '\n'); trace.flush()
            return
        if p.get('type') in ('snapshot', 'snapshot-error'):
            n[0] += 1
            if data:
                path = os.path.join(out_dir, f"state_{p['callSeq']}_{p['source'].get('field') or ('arg' if p['source'].get('arg') else 'ret')}.bin")
                open(path, 'wb').write(data)
                p['state'] = os.path.basename(path)
            trace.write(json.dumps(p) + '\n'); trace.flush()
            print(f"      snapshot {p.get('state') or p.get('error')}", flush=True)
            return
        if 'bt' in p:
            p['bt'] = [sym(a) for a in p['bt']]
        n[0] += 1
        p['seq'] = n[0]
        if data:
            path = os.path.join(out_dir, f'state_{n[0]}.bin')
            open(path, 'wb').write(data)
            p['state'] = os.path.basename(path)
        trace.write(json.dumps(p) + '\n')
        trace.flush()
        trace_lines[0] += 1
        print(f"{n[0]:5d} {p['fn']}", flush=True)
    script.on('message', on_message)
    script.load()
    if pid is not None and not program:
        device.resume(pid)
    print('tracing; Ctrl-C to stop', flush=True)
    if autoplay is not None:
        spec = json.load(open(autoplay))
        play_all(spec if isinstance(spec, list) else [spec], script, events)
        return
    try:
        while True:
            time.sleep(1)
            if program and not any(p.pid == pid for p in device.enumerate_processes()):
                break
    except KeyboardInterrupt:
        pass

TAPS = {  # Game Mode screen and tribe picker positions on the 1080x2400 emulator screen
    'mode': {1: (540, 606), 2: (540, 1114), 5: (540, 1622)},
    'tribe': {13: (252, 430), 7: (540, 430), 4: (828, 430), 10: (252, 780)},
}

def adb(*args):
    import subprocess
    subprocess.run([os.path.expanduser('~/Library/Android/sdk/platform-tools/adb'), *args], check=False, capture_output=True)

def tap(x, y, wait=3.0):
    adb('shell', 'input', 'tap', str(x), str(y)); time.sleep(wait)

def wait_for(events, kind, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        for e in events:
            if e.get('type') == kind: return e
        time.sleep(1)
    return None

def play_all(games, script, events):
    """Hands the agent a list of games; it creates and plays them itself, no taps."""
    if not wait_for(events, 'autoplay-ready', 120): print('agent never became ready'); return
    time.sleep(60)  # the main menu must be fully up before the first game is created
    script.post({'type': 'games', 'games': games})
    # Each game opens with the "Mighty Leader!" popup, which holds the game until OK is tapped
    # (in-game, centre of the screen). A watchdog saves a screenshot if moves stop coming.
    seen_starts, last_count, last_change = 0, -1, time.time()
    shots = os.path.join(os.path.dirname(os.path.abspath(sys.argv[2])), os.path.basename(sys.argv[2]) + '_stalls')
    os.makedirs(shots, exist_ok=True)
    while not any(e.get('type') == 'autoplay-done' for e in events):
        starts = sum(1 for e in events if e.get('type') == 'autoplay-start')
        if starts > seen_starts:
            seen_starts = starts
            time.sleep(15)
        count = trace_lines[0]
        if count != last_count: last_count, last_change = count, time.time()
        elif time.time() - last_change > 90:
            path = os.path.join(shots, f'stall_{int(time.time())}.png')
            adb('shell', 'screencap', '-p', '/data/local/tmp/stall.png'); adb('pull', '/data/local/tmp/stall.png', path)
            print('stalled; screenshot', path, flush=True)
            tap(540, 1500, wait=1)
            last_change = time.time()
        time.sleep(3)
    print('all games done', flush=True)

def play(game, script, events):
    """One game: menus by tap to Game Setup, settings via the agent, Start, then let it play."""
    if not wait_for(events, 'autoplay-ready', 120): print('agent never became ready'); return
    time.sleep(25)  # the main menu
    tap(540, 1186)                       # New Game
    tap(*TAPS['mode'][game['mode']])     # Perfection / Domination / Creative
    tap(*TAPS['tribe'][game['tribe']])   # tribe
    tap(908, 1822)                       # Pick
    script.post({'type': 'configure', 'game': game})
    if not wait_for(events, 'configured', 30): print('settings were not applied'); return
    tap(540, 936, wait=12)               # Start Game
    tap(540, 1500)                       # OK on the "Mighty Leader!" intro
    end = wait_for(events, 'autoplay-end', game.get('timeout', 3600))
    err = next((e for e in events if e.get('type') == 'autoplay-error'), None)
    print('game finished:', end, 'error:', err, flush=True)

if __name__ == '__main__':
    main()
