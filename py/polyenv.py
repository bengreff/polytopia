"""Python client for polyhost's bot interface (polyhost/wire.hpp, polyhost/env.hpp).

A polyhost process hosts many matches ("slots") and runs the game's own rules code. This module
starts such processes on the emulator, and wraps the wire protocol in numpy.

    import polyenv
    with polyenv.launch(port=8200) as env:
        st = env.new(0, polyenv.Spec(tribes=(7, 13), bots=(None, 3), seed=1))   # you vs Crazy
        while not st.ended:
            obs = env.observe([(0, 0)])[0]
            st = env.step([(0, pick(obs, st.legal))])[0][1]

Commands are rows of `legal`: (type, a, b, arg). `type` is the game's CommandType, `a` the tile
the command acts from or on, `b` the tile it targets (Move, Attack), `arg` the unit, improvement or
tech type, the city reward, or the opponent. Tiles are indices y * width + x, -1 for none.
"""
import contextlib
import dataclasses
import json
import os
import socket
import struct
import subprocess
import time

import numpy as np

ADB = os.environ.get('ADB', os.path.expanduser('~/Library/Android/sdk/platform-tools/adb'))
DEVICE_DIR = '/data/local/tmp/poly'

# Must match env.hpp; checked against the server's INFO reply.
OBS_VERSION = 1
LEGAL_VERSION = 2
TILE_FEATURES = [
    'explored', 'terrain', 'climate', 'owner', 'capital_of', 'ruling_city', 'resource', 'road', 'route', 'effects',
    'improvement', 'imp_owner', 'imp_level', 'imp_population', 'imp_production', 'imp_border', 'imp_connected',
    'imp_rewards', 'imp_effects', 'imp_xp',
    'unit', 'unit_owner', 'unit_health', 'unit_max_health', 'unit_promoted', 'unit_xp', 'unit_moved', 'unit_attacked',
    'unit_passenger', 'unit_effects', 'unit_age', 'unit_home',
]
GLOBAL_FEATURES = [
    'turn', 'seat', 'me', 'tribe', 'stars', 'income', 'score', 'cities', 'kills', 'casualties', 'handicap',
    'opp_tribe', 'opp_score', 'opp_known', 'relation', 'my_embassy', 'their_embassy', 'trigger', 'width', 'height',
]
TECH_SLOTS = 64

# The game's CommandType values (GameLogicAssembly CommandType).
COMMAND_TYPES = {
    1: 'Build', 2: 'Attack', 3: 'Recover', 4: 'HealOthers', 5: 'Train', 6: 'Move', 7: 'Capture', 8: 'Research',
    9: 'Destroy', 10: 'Disband', 11: 'CityReward', 13: 'Promote', 14: 'ExamineRuins', 15: 'EndTurn', 16: 'Upgrade',
    21: 'Stay', 22: 'EndMatch', 27: 'PeaceTreaty', 28: 'PeaceRequestResponse', 29: 'BreakPeace',
    30: 'EstablishEmbassy', 32: 'DestroyEmbassy', 33: 'Hide', 35: 'Resign', 36: 'Disembark',
}

# The 12 regular tribes (the game's TribeType values) and the 4 the free Android app has.
TRIBES = {'Aimo': 2, 'Bardur': 4, 'Hoodrick': 6, 'Imperius': 7, 'Kickoo': 8, 'Luxidoor': 9, 'Oumaji': 10,
          'Quetzali': 11, 'Vengir': 12, 'Xinxi': 13, 'Yadakk': 14, 'Zebasi': 15}
FREE_TRIBES = (13, 7, 4, 10)
BOT_DIFFICULTY = {'easy': 0, 'normal': 1, 'hard': 2, 'crazy': 3}

STEP_BOT_OWN = -2  # step action: play the built-in AI's own command for the current player
# replay_step labels that are not legal-list rows (env::Match::replayStep).
REPLAY_NOT_IN_LEGAL, REPLAY_NO_DECISION, REPLAY_OUT_OF_TURN = -1, -2, -3


@dataclasses.dataclass
class Spec:
    """A match. Defaults are the championship format: Might, Lakes, Small (14x14)."""
    tribes: tuple = (7, 13)
    bots: tuple = (None, None)  # per seat: None = moves come from outside, else BotDifficulty 0-3
    mode: int = 4               # GameMode.Might
    map_preset: int = 2         # MapPreset.Lakes
    map_size: int = 14
    seed: int = 1

    def pack(self):
        b = [-1 if x is None else x for x in self.bots]
        return struct.pack('<7iQ', self.tribes[0], b[0], self.tribes[1], b[1], self.mode, self.map_preset,
                           self.map_size, self.seed)


@dataclasses.dataclass
class Status:
    current: int      # player id to move (1 or 2)
    ended: bool
    winner: int       # player id, 0 while the game goes on
    turn: int
    legal: np.ndarray  # int16 [n, 4]: type, a, b, arg


@dataclasses.dataclass
class Obs:
    tiles: np.ndarray    # int16 [height, width, len(TILE_FEATURES)]
    globals: np.ndarray  # int32 [len(GLOBAL_FEATURES)]
    techs: np.ndarray    # uint8 [TECH_SLOTS]

    def tile(self, name):
        return self.tiles[..., TILE_FEATURES.index(name)]

    def get(self, name):
        return int(self.globals[GLOBAL_FEATURES.index(name)])


class EnvError(RuntimeError):
    pass


class Env:
    """One connection to one polyhost env process."""

    def __init__(self, host='127.0.0.1', port=8200, timeout=600):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        info = self.info()
        if (info['obs_version'], info['tile_features'], info['global_features'], info['tech_slots'],
                info['legal_version']) != (OBS_VERSION, len(TILE_FEATURES), len(GLOBAL_FEATURES), TECH_SLOTS, LEGAL_VERSION):
            raise EnvError(f'server observation layout {info} does not match this client')
        self.game_version = info['game_version']

    def close(self):
        self.sock.close()

    def _recv(self, n):
        buf = bytearray()
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise EnvError('polyhost closed the connection (it exits on a managed exception; see its log)')
            buf += chunk
        return bytes(buf)

    def request(self, op, payload=b''):
        body = bytes([op]) + payload
        self.sock.sendall(struct.pack('<I', len(body)) + body)
        (n,) = struct.unpack('<I', self._recv(4))
        resp = self._recv(n)
        if resp[0] != 0:
            raise EnvError(resp[1:].decode(errors='replace'))
        return resp[1:]

    @staticmethod
    def _status(buf, p):
        cur, ended, winner, turn, n = struct.unpack_from('<BBBIH', buf, p)
        p += 9
        legal = np.frombuffer(buf, dtype='<i2', count=4 * n, offset=p).reshape(n, 4).copy()
        return Status(cur, bool(ended), winner, turn, legal), p + 8 * n

    def info(self):
        v = struct.unpack('<7i', self.request(8))
        return dict(zip(['game_version', 'logic_version', 'obs_version', 'tile_features', 'global_features',
                         'tech_slots', 'legal_version'], v))

    def new(self, slot, spec):
        """Creates a match in `slot` (replacing any) and plays bot seats up to the first outside decision."""
        return self._status(self.request(1, struct.pack('<I', slot) + spec.pack()), 0)[0]

    def step(self, actions):
        """actions: [(slot, legal index or STEP_BOT_OWN)]. Returns [(ok, Status)], after bot seats move."""
        buf = self.request(2, struct.pack('<I', len(actions)) + b''.join(struct.pack('<Ii', s, a) for s, a in actions))
        out, p = [], 0
        for _ in actions:
            ok = buf[p] != 0
            st, p = self._status(buf, p + 1)
            out.append((ok, st))
        return out

    def observe(self, requests):
        """requests: [(slot, viewer player id or 0 for the player to move)]. Returns [Obs]."""
        buf = self.request(3, struct.pack('<I', len(requests)) + b''.join(struct.pack('<IB', s, v) for s, v in requests))
        out, p = [], 0
        nt, ng = len(TILE_FEATURES), len(GLOBAL_FEATURES)
        for _ in requests:
            w, h = struct.unpack_from('<HH', buf, p)
            p += 4
            tiles = np.frombuffer(buf, dtype='<i2', count=w * h * nt, offset=p).reshape(h, w, nt).copy()
            p += 2 * w * h * nt
            glob = np.frombuffer(buf, dtype='<i4', count=ng, offset=p).copy()
            p += 4 * ng
            techs = np.frombuffer(buf, dtype=np.uint8, count=TECH_SLOTS, offset=p).copy()
            p += TECH_SLOTS
            out.append(Obs(tiles, glob, techs))
        return out

    def bot(self, slots):
        """Index in the legal list of the command the built-in AI would play now, or -1 (its StayCommand)."""
        buf = self.request(4, struct.pack('<I', len(slots)) + b''.join(struct.pack('<I', s) for s in slots))
        return list(struct.unpack(f'<{len(slots)}i', buf))

    def save(self, slot):
        buf = self.request(5, struct.pack('<I', slot))
        (n,) = struct.unpack_from('<I', buf)
        return buf[4:4 + n]

    def load(self, slot, spec, state):
        return self._status(self.request(6, struct.pack('<I', slot) + spec.pack() + struct.pack('<I', len(state)) + state), 0)[0]

    def dump(self, slot):
        return json.loads(self.request(7, struct.pack('<I', slot)))

    def free(self, slot):
        self.request(9, struct.pack('<I', slot))

    def replay(self, slot, spec, start, final):
        """Loads a recorded game: `start` and `final` are GameState blobs as the game's server sends
        them (i32 version, then the state). Returns (commands left to play, Status)."""
        body = struct.pack('<I', slot) + spec.pack() + struct.pack('<I', len(start)) + start + struct.pack('<I', len(final)) + final
        buf = self.request(11, body)
        (left,) = struct.unpack_from('<I', buf)
        return left, self._status(buf, 4)[0]

    def replay_step(self, slot):
        """Plays the next recorded command. Returns (player, label, recorded command row, ok,
        commands left, Status); label is its row in the legal list before it ran, or REPLAY_*."""
        buf = self.request(12, struct.pack('<I', slot))
        player, label = struct.unpack_from('<Bi', buf)
        cmd = np.frombuffer(buf, dtype=np.int16, count=4, offset=5).copy()
        ok, left = struct.unpack_from('<BI', buf, 13)
        return player, label, cmd, bool(ok), left, self._status(buf, 18)[0]

    def owners(self, slot):
        """The true owner and capitalOf player id of every tile, [h, w] each. Not an observation:
        for training targets and checks only."""
        buf = self.request(10, struct.pack('<I', slot))
        w, h = struct.unpack_from('<HH', buf)
        owner = np.frombuffer(buf, dtype=np.uint8, count=w * h, offset=4).reshape(h, w).copy()
        capital = np.frombuffer(buf, dtype=np.uint8, count=w * h, offset=4 + w * h).reshape(h, w).copy()
        return owner, capital


def describe(cmd, width=14):
    """A command row as text, for logs."""
    t, a, b, arg = (int(x) for x in cmd)
    xy = lambda i: f'({i % width},{i // width})' if i >= 0 else ''
    s = COMMAND_TYPES.get(t, f'type{t}')
    if a >= 0: s += ' ' + xy(a)
    if b >= 0: s += ' -> ' + xy(b)
    if t in (1, 5, 8, 11, 16): s += f' [{arg}]'
    return s


def adb(*args, **kw):
    return subprocess.run([ADB, *args], check=True, capture_output=True, text=True, **kw).stdout


REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# POLYHOST=mac runs polyhost natively on macOS against the Steam build's GameAssembly.dylib (the
# same game build as the Android oracle, verified against the real app). POLYHOST=qemu runs the
# Android build (the oracle's own libil2cpp.so) under QEMU user-mode emulation on an x86-64 Linux
# host, e.g. the PC's WSL; POLYARM is its directory (tools/setup_pc_games.sh). Otherwise polyhost runs
# in the Android emulator through adb.
NATIVE = os.environ.get('POLYHOST', 'emulator') == 'mac'
QEMU = os.environ.get('POLYHOST', 'emulator') == 'qemu'
POLYARM = os.environ.get('POLYARM', os.path.expanduser('~/polyarm'))


@contextlib.contextmanager
def launch(port=8200, connect_timeout=60, binary='polyhost'):
    """Starts `polyhost env <port>` and yields a connected Env: natively on macOS with POLYHOST=mac
    (build/polyhost-mac, built by ./polyhost/run.sh mac), else on the emulator with the port
    forwarded (./polyhost/run.sh build pushes it). On the emulator, `binary` names another build in
    the device directory, so a test build can run beside a live one."""
    if NATIVE or QEMU:
        with _launch_native(port, connect_timeout) as env:
            yield env
        return
    adb('forward', f'tcp:{port}', f'tcp:{port}')
    log = f'{DEVICE_DIR}/env{port}.log'
    proc = subprocess.Popen([ADB, 'shell', f'cd {DEVICE_DIR} && exec ./{binary} env {port} > {log} 2>&1'])
    env = None
    try:
        deadline = time.time() + connect_timeout
        while env is None:
            try:
                env = Env(port=port)
            except (ConnectionError, OSError, EnvError):  # adb accepts before the server listens
                if time.time() > deadline or proc.poll() is not None:
                    raise EnvError('polyhost env did not start: ' + adb('shell', f'cat {log}'))
                time.sleep(0.2)
        yield env
    finally:
        if env:
            env.close()
        subprocess.run([ADB, 'shell', f'pkill -f "{binary} env {port}"'], capture_output=True)
        proc.wait(timeout=10)


def _die_with_parent():
    """Linux: the game process gets SIGKILL when the thread that started it dies, so a killed run
    leaves no orphans holding its ports."""
    import ctypes
    import signal
    ctypes.CDLL('libc.so.6', use_errno=True).prctl(1, signal.SIGKILL)  # PR_SET_PDEATHSIG


@contextlib.contextmanager
def _launch_native(port, connect_timeout):
    if QEMU:  # Cortex-A72 (ARMv8.0): QEMU's default CPU emulates pointer authentication slowly, ~25% slower
        os.makedirs(os.path.join(POLYARM, 'logs'), exist_ok=True)
        log = open(os.path.join(POLYARM, 'logs', f'env{port}.log'), 'w')
        cmd = [os.path.join(POLYARM, 'qemu-aarch64'), '-cpu', 'cortex-a72', '-L', os.path.join(POLYARM, 'root'),
               os.path.join(POLYARM, 'poly', 'polyhost'), 'env', str(port)]
        proc = subprocess.Popen(cmd, cwd=os.path.join(POLYARM, 'poly'), stdout=log, stderr=subprocess.STDOUT,
                                env=dict(os.environ, POLY_ROOT=os.path.join(POLYARM, 'poly')), preexec_fn=_die_with_parent)
    else:
        log = open(os.path.join(REPO, 'build', f'env{port}.log'), 'w')
        proc = subprocess.Popen([os.path.join(REPO, 'build', 'polyhost-mac'), 'env', str(port)], cwd=REPO,
                                stdout=log, stderr=subprocess.STDOUT)
    env = None
    try:
        deadline = time.time() + connect_timeout
        while env is None:
            try:
                env = Env(port=port)
            except (ConnectionError, OSError, EnvError):
                if time.time() > deadline or proc.poll() is not None:
                    raise EnvError(f'polyhost env did not start; see {log.name}')
                time.sleep(0.2)
        yield env
    finally:
        if env:
            env.close()
        proc.terminate()
        proc.wait(timeout=10)
