"""Generates imitation data from the game's own AI playing championship games against itself.

Every game is built as the real app builds a pass-and-play game (polyhost env::Match): 1v1 Might,
Lakes, Small (14x14), two human seats with handicap 1, both tribes drawn uniformly from the 12
regular tribes. The game's AI (AI.GetMove, which the client's advisor also runs for a person's
seat) chooses every command for both seats. On a human seat it makes no deliberate mistakes
(PlayerState.GetStupidity is 0 without AutoPlay).

Per decision it records the observation of the player to move, the legal list, and the index of
the AI's choice. An AI command outside the agent's legal list is played but not recorded: its
StayCommand (bot-only, marks a unit done), or, rarely, a command on a tile its player hasn't
explored (the AI sees the whole map; that is fine for the built-in AI, never for ours).
Each game is one compressed .npz in the output directory; see save_game for the arrays.

    python3 py/gen_data.py <out_dir> --games 2000 --procs 6 --slots 8 [--seed 0]
"""
import argparse
import multiprocessing as mp
import os
import time

import numpy as np

import polyenv

REGULAR_TRIBES = sorted(polyenv.TRIBES.values())
TURN_CAP = 150


def game_spec(seed):
    rng = np.random.default_rng(seed)
    t = rng.choice(REGULAR_TRIBES, 2)
    return polyenv.Spec(tribes=(int(t[0]), int(t[1])), bots=(None, None), seed=seed)


class Recorder:
    def __init__(self, seed, spec):
        self.seed, self.spec = seed, spec
        self.tiles, self.globs, self.techs, self.legal, self.labels, self.players = [], [], [], [], [], []
        self.unlabelled = 0

    def add(self, obs, legal, label, player):
        self.tiles.append(obs.tiles.reshape(-1, obs.tiles.shape[-1]))
        self.globs.append(obs.globals)
        self.techs.append(obs.techs)
        self.legal.append(legal)
        self.labels.append(label)
        self.players.append(player)


def save_game(out_dir, rec, st, owner, capital):
    """Arrays, D = recorded decisions:
    tiles int16 [D, 196, 32], globals int32 [D, 20], techs uint8 [D, 64], legal int16 [sum n, 4]
    with legal_offsets int32 [D + 1], label int16 [D] (row in that decision's legal list),
    player uint8 [D]; and per game tribes, seed, winner (player id, 0 if unfinished), turns,
    ended, final_owner and final_capital_of uint8 [196] (true state), unlabelled (AI commands outside
    the legal list), obs_version, legal_version."""
    offsets = np.zeros(len(rec.legal) + 1, np.int32)
    offsets[1:] = np.cumsum([len(l) for l in rec.legal])
    path = os.path.join(out_dir, f'g{rec.seed:08d}.npz')
    np.savez_compressed(
        path + '.tmp.npz',
        tiles=np.stack(rec.tiles), globals=np.stack(rec.globs), techs=np.stack(rec.techs),
        legal=np.concatenate(rec.legal), legal_offsets=offsets,
        label=np.asarray(rec.labels, np.int16), player=np.asarray(rec.players, np.uint8),
        tribes=np.asarray(rec.spec.tribes, np.int16), seed=np.int64(rec.seed), winner=np.uint8(st.winner),
        turns=np.int32(st.turn), ended=np.bool_(st.ended), final_owner=owner.reshape(-1),
        final_capital_of=capital.reshape(-1), unlabelled=np.int32(rec.unlabelled), obs_version=np.int32(polyenv.OBS_VERSION),
        legal_version=np.int32(polyenv.LEGAL_VERSION))
    os.replace(path + '.tmp.npz', path)


def worker(port, seeds, out_dir, slots, queue):
    seeds = list(seeds)
    with polyenv.launch(port) as env:
        live = {}  # slot -> (Recorder, Status)

        def start(slot):
            seed = seeds.pop()
            spec = game_spec(seed)
            live[slot] = (Recorder(seed, spec), env.new(slot, spec))

        for s in range(slots):
            if seeds:
                start(s)
        while live:
            order = sorted(live)
            obs = env.observe([(s, 0) for s in order])
            labels = env.bot(order)
            actions = []
            for s, o, label in zip(order, obs, labels):
                rec, st = live[s]
                if label >= 0:
                    rec.add(o, st.legal, label, st.current)
                else:
                    rec.unlabelled += 1
                actions.append((s, label if label >= 0 else polyenv.STEP_BOT_OWN))
            for (s, _), (ok, st) in zip(actions, env.step(actions)):
                if not ok:
                    raise polyenv.EnvError(f'slot {s}: the AI\'s command was rejected')
                rec = live[s][0]
                if st.ended or st.turn >= TURN_CAP:
                    owner, capital = env.owners(s)
                    save_game(out_dir, rec, st, owner, capital)
                    queue.put((rec.seed, rec.spec.tribes, st.winner, st.turn, st.ended, len(rec.labels)))
                    del live[s]
                    if seeds:
                        start(s)
                else:
                    live[s] = (rec, st)
    queue.put(None)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out_dir')
    ap.add_argument('--games', type=int, default=100)
    ap.add_argument('--procs', type=int, default=6)
    ap.add_argument('--slots', type=int, default=8)
    ap.add_argument('--seed', type=int, default=0, help='first game seed; games use seed, seed+1, ...')
    args = ap.parse_args()
    os.makedirs(args.out_dir, exist_ok=True)
    done_log = os.path.join(args.out_dir, 'games.tsv')  # every finished game, including ones synced away
    finished = {int(l.split('\t')[0]) for l in open(done_log)} if os.path.exists(done_log) else set()
    todo = [s for s in range(args.seed, args.seed + args.games)
            if s not in finished and not os.path.exists(os.path.join(args.out_dir, f'g{s:08d}.npz'))]
    queue = mp.Queue()
    procs = [mp.Process(target=worker, args=(8200 + i, todo[i::args.procs], args.out_dir, args.slots, queue))
             for i in range(args.procs)]
    for p in procs:
        p.start()
    t0, done, running, seat_wins, ended, decisions, turns = time.time(), 0, len(procs), [0, 0], 0, 0, 0
    log = open(os.path.join(args.out_dir, 'games.tsv'), 'a')
    while running:
        item = queue.get()
        if item is None:
            running -= 1
            continue
        seed, tribes, winner, turn, is_ended, n = item
        log.write(f'{seed}\t{tribes[0]}\t{tribes[1]}\t{winner}\t{turn}\t{int(is_ended)}\t{n}\n')
        log.flush()
        done += 1
        ended += is_ended
        decisions += n
        turns += turn
        if is_ended and winner in (1, 2):
            seat_wins[winner - 1] += 1
        if done % 10 == 0 or done == len(todo):
            dt = time.time() - t0
            print(f'{done}/{len(todo)} games, {done / dt * 3600:.0f}/h, {ended} ended, mean {turns / done:.1f} turns, '
                  f'{decisions / done:.0f} decisions/game, first seat won {seat_wins[0]}/{sum(seat_wins)}', flush=True)
    for p in procs:
        p.join()


if __name__ == '__main__':
    main()
