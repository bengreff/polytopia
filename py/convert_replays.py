"""Turns downloaded human games (py/fetch_replays.py) into training data in gen_data.py's layout.

Each game is loaded from the server's initial GameState with the game's own deserializer, and the
final state's command stack is played through ActionManager one command at a time (polyhost op
REPLAY/RSTEP). Before each command of a seated player, it records that player's observation and
legal list, and the command's row in that list as the label. Commands outside our legal list are
counted and logged (they point at legal-list gaps), except StartMatch (the client plays it) and
Resign (logged as `resigned`). Nature's commands and EndMatch are played but not recorded.

Only the championship format is converted unless --all: two players of regular tribes, Might, Lakes,
Small (14x14).

    python3 py/convert_replays.py [--raw data/replays/raw] [--out data/human1] [--port 8400]
        [--binary polyhost_replay] [--all]
(POLYHOST=mac runs polyhost natively on the Mac, see polyenv.launch.)

Each game also gets a final check: the state our replay ends in against the final state the server
recorded (column `final` in games.tsv): 'bytes', 'content', or 'differs <n> <first field>'.
"""
import argparse
import base64
import json
import os
import sys

import numpy as np

import polyenv
from gen_data import Recorder

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
import statecompare

MIGHT, LAKES, SMALL = 4, 2, 14
START_MATCH, RESIGN = 20, 35  # CommandType
REGULAR_TRIBES = set(polyenv.TRIBES.values())


def describe_game(dump):
    s = dump['Settings']
    players = [p for p in dump['PlayerStates'] if p['Id'] != 255]
    return {
        'version': dump['Version'], 'mode': s['rulesGameMode'], 'map_preset': s['mapPreset'], 'map_size': s['MapSize'],
        'game_type': s['GameType'], 'time_limit': s['TimeLimit'], 'live': s['LiveGamePreset'],
        'auto_skip': s['IsAutoSkipEnabled'], 'players': [(p['Id'], p['tribe'], p['UserName']) for p in players],
    }


def championship(meta):
    return (len(meta['players']) == 2 and (meta['mode'], meta['map_preset'], meta['map_size']) == (MIGHT, LAKES, SMALL)
            and all(p[1] in REGULAR_TRIBES for p in meta['players']))


def save(path, game_id, rec, meta, st, owner, capital, not_in_legal, out_of_turn, check):
    """gen_data.save_game's arrays (see there), with the game id and settings instead of a seed."""
    offsets = np.zeros(len(rec.legal) + 1, np.int32)
    offsets[1:] = np.cumsum([len(l) for l in rec.legal])
    np.savez_compressed(
        path + '.tmp.npz',
        tiles=np.stack(rec.tiles), globals=np.stack(rec.globs), techs=np.stack(rec.techs),
        legal=np.concatenate(rec.legal), legal_offsets=offsets,
        label=np.asarray(rec.labels, np.int16), player=np.asarray(rec.players, np.uint8),
        tribes=np.asarray([p[1] for p in meta['players']], np.int16), game_id=game_id,
        game_version=np.int32(meta['version']), winner=np.uint8(st.winner), turns=np.int32(st.turn),
        ended=np.bool_(st.ended), final_owner=owner.reshape(-1), final_capital_of=capital.reshape(-1),
        not_in_legal=np.int32(not_in_legal), out_of_turn=np.int32(out_of_turn), final_check=check, source='human',
        obs_version=np.int32(polyenv.OBS_VERSION), legal_version=np.int32(polyenv.LEGAL_VERSION))
    os.replace(path + '.tmp.npz', path)


def convert(env, raw, out_dir, keep_all, misses):
    game_id = raw['id']
    start = base64.b64decode(raw['initialGameStateData'])
    final = base64.b64decode(raw['currentGameStateData'])
    left, st = env.replay(0, polyenv.Spec(), start, final)
    meta = describe_game(env.dump(0))
    if not keep_all and not championship(meta):
        return meta, None
    rec = Recorder(0, None)
    not_in_legal = out_of_turn = resigned = 0
    while left:
        obs = env.observe([(0, 0)])[0] if not st.ended else None
        before = st
        player, label, cmd, ok, left, st = env.replay_step(0)
        if label >= 0:
            rec.add(obs, before.legal, label, player)
        elif label == polyenv.REPLAY_NOT_IN_LEGAL and cmd[0] == START_MATCH:
            pass  # the client starts the match; nobody chooses it
        elif label == polyenv.REPLAY_NOT_IN_LEGAL and cmd[0] == RESIGN:
            resigned = player  # a person's choice our agent never makes; the game ends here
        elif label == polyenv.REPLAY_NOT_IN_LEGAL:
            not_in_legal += 1
            misses.write(f'{game_id}\t{before.turn}\t{player}\t{polyenv.describe(cmd)}\t{len(before.legal)}\n')
        elif label == polyenv.REPLAY_OUT_OF_TURN:
            out_of_turn += 1
        if not ok:
            raise polyenv.EnvError(f'recorded command {polyenv.describe(cmd)} was rejected')
    check = final_check(env, final)
    if rec.labels:
        owner, capital = env.owners(0)
        save(os.path.join(out_dir, f'{game_id}.npz'), game_id, rec, meta, st, owner, capital, not_in_legal, out_of_turn, check)
    return meta, (len(rec.labels), not_in_legal, out_of_turn, st, check, resigned)


def final_check(env, final):
    """Compares the state our replay ended in with the final state the server recorded: 'bytes' if
    identical after the same save and load, 'content' if equal by tools/statecompare.py (which ignores
    UI side effects and AI scratch state), otherwise the number of differing fields and the first one."""
    version = final[:4]
    ours = version + env.save(0)
    env.replay(1, polyenv.Spec(), ours, ours)
    env.replay(2, polyenv.Spec(), final, final)
    if env.save(1) == env.save(2):
        return 'bytes'
    out = []
    statecompare.diff(statecompare.canon(env.dump(2)), statecompare.canon(env.dump(1)), '', out)
    return 'content' if not out else f'differs {len(out)} {out[0][0]}'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--raw', default='data/replays/raw')
    ap.add_argument('--out', default='data/human1')
    ap.add_argument('--port', type=int, default=8400)
    ap.add_argument('--binary', default='polyhost_replay')
    ap.add_argument('--all', action='store_true', help='convert every game, not only the championship format')
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    files = sorted(f for f in os.listdir(args.raw) if f.endswith('.json'))
    log = open(os.path.join(args.out, 'games.tsv'), 'a')
    misses = open(os.path.join(args.out, 'not_in_legal.tsv'), 'a')
    done = {l.split('\t')[0] for l in open(os.path.join(args.out, 'games.tsv'))}
    totals = dict(games=0, skipped=0, failed=0, decisions=0, not_in_legal=0)
    with polyenv.launch(args.port, binary=args.binary) as env:
        for f in files:
            raw = json.load(open(os.path.join(args.raw, f)))
            if raw['id'] in done:
                continue
            try:
                meta, result = convert(env, raw, args.out, args.all, misses)
            except polyenv.EnvError as e:
                totals['failed'] += 1
                log.write(f"{raw['id']}\tfailed\t{e}\n")
                log.flush()
                continue
            fmt = f"v{meta['version']} mode{meta['mode']} map{meta['map_preset']} size{meta['map_size']} {len(meta['players'])}p"
            if result is None:
                totals['skipped'] += 1
                log.write(f"{raw['id']}\tskipped\t{fmt}\n")
            else:
                n, nil, oot, st, check, resigned = result
                totals['games'] += 1
                totals['decisions'] += n
                totals['not_in_legal'] += nil
                tribes = ','.join(str(p[1]) for p in meta['players'])
                log.write(f"{raw['id']}\tok\t{fmt}\ttribes {tribes}\twinner {st.winner}\tturns {st.turn}\t"
                          f"decisions {n}\tnot_in_legal {nil}\tout_of_turn {oot}\tresigned {resigned}\tfinal {check}\n")
            log.flush()
            misses.flush()
    print(totals)


if __name__ == '__main__':
    main()
