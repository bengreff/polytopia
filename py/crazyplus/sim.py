"""Engine helpers for CrazyPlus: clones, rollouts and the two polyhost ops it adds
(polyhost/crazyplus.hpp, build/polyhost-mac-cp).

The bot keeps a full view of the game, as the built-in AI does: it reads the true state and plays
candidate commands on copies of it, so every outcome it scores is the game's own.
"""
import os
import struct

import numpy as np

import polyenv

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BINARY = os.path.join(REPO, 'build', 'polyhost-mac-cp')

STAY = -100  # an action code of ours: play the StayCommand for a unit (tile = -(code - STAY) - 1, see stay_code)


def stay_code(tile):
    return STAY - 1 - tile


def stay_tile(code):
    return STAY - 1 - code


def bot_turn(env, slot, max_commands=400):
    """The built-in AI plays the current player's turn to its end, then the bot seats move.
    Returns (the commands it played as rows (type, a, b, arg), Status)."""
    buf = env.request(40, struct.pack('<II', slot, max_commands))
    (n,) = struct.unpack_from('<I', buf)
    rows = np.frombuffer(buf, dtype='<i2', count=4 * n, offset=4).reshape(n, 4)
    return [tuple(int(x) for x in r) for r in rows], env._status(buf, 4 + 8 * n)[0]


def stay(env, slot, tile):
    """The current player's unit on `tile` stays (does nothing more this turn). Returns (ok, Status)."""
    buf = env.request(41, struct.pack('<Ih', slot, tile))
    return buf[0] != 0, env._status(buf, 1)[0]


def play(env, slot, action):
    """Plays one of our action codes: a legal index, polyenv.STEP_BOT_OWN, or a stay code."""
    if action <= STAY:
        return stay(env, slot, stay_tile(action))
    return env.step([(slot, action)])[0]
