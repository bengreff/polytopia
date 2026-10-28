"""Saves the human midgame positions chosen by py/human_midgames.py as game states, for self-play
games that start from them (py/selfplay.py --starts; research log S3c).

Each game is replayed from the server's recorded states (as py/convert_replays.py does) up to the
position: just before the player's first recorded decision of that turn. Most human games were
played in older game versions (121, 122); the state is loaded into a fresh slot marked as the
current version (polyhost op 13), so self-play continues under the pinned rules, and saved again.
Check, after that and after a plain reload of the saved state: the observation and legal list must
equal the ones recorded in data/human1 for that decision (positions where the current rules list
different moves are skipped). Only the human games' states are kept, never their moves.
Games from versions before --min_version are skipped.

Output: <out>/<game>_t<turn>_p<player>.state and <out>/index.tsv (file, game, turn, player, tribes,
interest). Positions come from py/human_midgames.py or py/human_positions.py. With --shard i/k
(several processes, e.g. on the PC's native engine) a process takes every k-th game and writes
index_<i>.tsv; concatenate them into index.tsv.

    POLYHOST=mac .venv/bin/python py/human_starts.py [--positions runs/human_midgames.tsv] [--out runs/human_starts]
    POLYHOST=linux python human_starts.py --positions human_positions.tsv --out ~/polydata/human_starts_big --shard 3/14
"""
import argparse
import base64
import contextlib
import json
import os

import numpy as np

import polyenv

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--positions', default=os.path.join(REPO, 'runs', 'human_midgames.tsv'))
    ap.add_argument('--raw', default=os.path.join(REPO, 'data', 'replays', 'raw'))
    ap.add_argument('--data', default=os.path.join(REPO, 'data', 'human1'))
    ap.add_argument('--out', default=os.path.join(REPO, 'runs', 'human_starts'))
    ap.add_argument('--port', type=int, default=8460)
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--min_version', type=int, default=121)
    ap.add_argument('--shard', default=None, help='i/k: every k-th game, from the i-th')
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    rows = [l.rstrip('\n').split('\t') for l in open(args.positions)]
    cols, rows = rows[0], [dict(zip(rows[0], r)) for r in rows[1:]]
    if args.limit:
        rows = rows[:args.limit]
    by_game = {}
    for r in rows:
        by_game.setdefault(r['game'], []).append(r)
    games = sorted(by_game.items())
    index_name = 'index.tsv'
    if args.shard:
        i, k = map(int, args.shard.split('/'))
        games, index_name = games[i::k], f'index_{i}.tsv'
    index = open(os.path.join(args.out, index_name), 'w')
    index.write('file\tgame\tturn\tplayer\ttribes\tinterest\n')
    saved = failed = 0
    stack = contextlib.ExitStack()
    env = stack.enter_context(polyenv.launch(args.port))
    try:
        for gi, (game, wanted) in enumerate(games):
            try:
                raw = json.load(open(os.path.join(args.raw, game + '.json')))
                z = np.load(os.path.join(args.data, game + '.npz'))
                version = int(z['game_version'])
                if version < args.min_version:
                    continue
                start = base64.b64decode(raw['initialGameStateData'])
                final = base64.b64decode(raw['currentGameStateData'])
                left, st = env.replay(0, polyenv.Spec(), start, final)
                targets = {int(r['decision']): r for r in wanted}
                done = 0  # recorded decisions played so far (labels >= 0, as in convert_replays.py)
                while left and targets:
                    blob = env.save(0) if done in targets else None
                    player, label, cmd, ok, left, st = env.replay_step(0)
                    if label < 0:
                        continue
                    if done in targets:
                        r = targets.pop(done)
                        tribes = tuple(int(x) for x in (json.loads(r['tribes']) if r['tribes'].startswith('[') else r['tribes'].split(',')))
                        spec = polyenv.Spec(tribes=tribes)
                        # check: loaded fresh as the current version, the state shows that player what
                        # the human saw; then again after saving it and loading it the plain way
                        off = z['legal_offsets']
                        same = lambda st2: (st2.current == int(r['player']) and np.array_equal(
                            env.observe([(1, 0)])[0].tiles.reshape(-1, z['tiles'].shape[-1]), z['tiles'][done])
                            and np.array_equal(env.observe([(1, 0)])[0].globals, z['globals'][done])
                            and np.array_equal(st2.legal, z['legal'][off[done]:off[done + 1]]))
                        ok = same(env.load_current(1, spec, version, blob))
                        if ok:
                            blob = env.save(1)
                            ok = same(env.load(1, spec, blob))
                        same = ok
                        if same:
                            name = f"{game}_t{r['turn']}_p{r['player']}.state"
                            with open(os.path.join(args.out, name), 'wb') as f:
                                f.write(blob)
                            index.write(f"{name}\t{game}\t{r['turn']}\t{r['player']}\t{tribes[0]},{tribes[1]}\t{r.get('interest', '')}\n")
                            saved += 1
                        else:
                            failed += 1
                            print(f'{game} turn {r["turn"]}: the loaded state differs from the recording', flush=True)
                    done += 1
            except polyenv.EnvError as e:  # the engine exits on the game's own exceptions (old versions)
                failed += 1
                print(f'{game}: engine error, game skipped ({e})', flush=True)
                stack.close()
                stack = contextlib.ExitStack()
                env = stack.enter_context(polyenv.launch(args.port))
            if (gi + 1) % 100 == 0:
                print(f'{gi + 1}/{len(games)} games, {saved} states saved, {failed} failed checks', flush=True)
                index.flush()

    finally:
        stack.close()
    index.close()
    print(f'done: {saved} states saved, {failed} failed checks -> {args.out}')


if __name__ == '__main__':
    main()
