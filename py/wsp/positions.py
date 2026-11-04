"""WSP-1 (research log): turn-start and turn-end positions from recorded games, with the facts the
critic audit groups by, scored by one or more value networks. CPU by default (2-4 threads, nice).

A position is a recorded decision's observation:
- kind 0, turn start: the player's first recorded decision of a turn (for the planner, the position
  it scores: the start of its next turn, after the opponent's reply);
- kind 1, turn end: the decision at which the player chose End Turn (its plan done, stars banked).

Reads py/selfplay.py games (`played`) and converted human games (`label`). Writes one .npz of
per-position arrays (facts, outcome y: 1 win, 0 loss, -1 none; v_<name> per scorer), and with
--keep_obs the observations of a random share of the turn-start positions for the probes.

    python py/wsp/positions.py --games DIR[:N][:val] [...] --scorers critic=critic_g083.pt head=gen083.pt
        --out pos.npz [--keep_obs 0.1]
"""
import argparse
import os
import sys
import time

import numpy as np
import torch

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import model as M  # noqa: E402
import polyenv  # noqa: E402
import train_bc  # noqa: E402

T = {n: i for i, n in enumerate(polyenv.TILE_FEATURES)}
G = {n: i for i, n in enumerate(polyenv.GLOBAL_FEATURES)}
END_TURN = 15
CITY = 1  # ImprovementType City
UNIT_KINDS = 13  # UnitType 0..12 (tile value = type + 1)


def load_net(path):
    ck = torch.load(path, map_location='cpu', weights_only=False)
    net = M.Net(**ck.get('config', dict(d=256, layers=6)))
    net.load_state_dict(ck['model'])
    return net.eval()


@torch.no_grad()
def values(net, tiles, glob, techs, batch=128):
    """Win probability (sigmoid of the value logit) and the logit, for observation arrays."""
    out = []
    for i in range(0, len(tiles), batch):
        t = torch.from_numpy(np.ascontiguousarray(tiles[i:i + batch]))
        g = torch.from_numpy(np.ascontiguousarray(glob[i:i + batch]))
        k = torch.from_numpy(np.ascontiguousarray(techs[i:i + batch]))
        h = net.encode(t, g, k)
        out.append(net.value(h[:, 0]).squeeze(-1).float().numpy())
    return np.concatenate(out) if out else np.zeros(0, np.float32)


def facts(tiles, glob, techs):
    """Per-position facts from observations [B, 196, 32] etc. -> dict of arrays."""
    t = tiles.astype(np.int32)
    unit, uown = t[..., T['unit']], t[..., T['unit_owner']]
    own_units = np.stack([((unit == k + 1) & (uown == 1)).sum(1) for k in range(UNIT_KINDS)], 1)
    enemy_units = np.stack([((unit == k + 1) & (uown == 2)).sum(1) for k in range(UNIT_KINDS)], 1)
    imp, iown = t[..., T['improvement']], t[..., T['imp_owner']]
    return dict(
        turn=glob[:, G['turn']], seat=glob[:, G['seat']], tribe=glob[:, G['tribe']], opp_tribe=glob[:, G['opp_tribe']],
        stars=glob[:, G['stars']], income=glob[:, G['income']], score=glob[:, G['score']],
        opp_score=glob[:, G['opp_score']], cities=glob[:, G['cities']], kills=glob[:, G['kills']],
        casualties=glob[:, G['casualties']], opp_known=glob[:, G['opp_known']],
        ntechs=techs.astype(np.int32).sum(1), explored=(t[..., T['explored']] > 0).mean(1).astype(np.float32),
        own_land=(t[..., T['owner']] == 1).sum(1), enemy_land=(t[..., T['owner']] == 2).sum(1),
        enemy_cities_seen=((imp == CITY) & (iown == 2)).sum(1),
        own_units=own_units.astype(np.int16), enemy_units=enemy_units.astype(np.int16))


def game_positions(path):
    """(indices of turn-start decisions, of turn-end decisions, player per decision, outcome per decision, z)."""
    z = np.load(path)
    if not train_bc.accepted(z):
        return None
    rows = z['played'] if 'played' in z.files else z['label']
    player = z['player'].astype(np.int64)
    turn = z['globals'][:, G['turn']]
    legal, off = z['legal'], z['legal_offsets']
    D = len(player)
    starts, ends = [], []
    prev = None
    for d in range(D):
        key = (int(player[d]), int(turn[d]))
        if key != prev:
            starts.append(d)
            prev = key
        r = int(rows[d])
        if r >= 0 and off[d] + r < off[d + 1] and int(legal[off[d] + r][0]) == END_TURN:
            ends.append(d)
    ended, winner = bool(z['ended']), int(z['winner'])
    kicked = bool(z['kicked']) if 'kicked' in z.files else False
    has = ended and winner in (1, 2) and not kicked
    y = np.where(player == winner, 1, 0) if has else np.full(D, -1)
    return np.array(starts, np.int64), np.array(ends, np.int64), player, y, z


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--games', nargs='+', required=True,
                    help='DIR[:N][:val]: the first N accepted games by file name; :val keeps only train_bc\'s held-out split')
    ap.add_argument('--scorers', nargs='*', default=[], help='name=checkpoint (value head used)')
    ap.add_argument('--with_techs', action='store_true', help='also save each position\'s tech vector and unit counts of every kind')
    ap.add_argument('--out', required=True)
    ap.add_argument('--keep_obs', type=float, default=0.0, help='share of turn-start positions whose observations are kept')
    ap.add_argument('--threads', type=int, default=4)
    ap.add_argument('--seed', type=int, default=0)
    args = ap.parse_args()
    torch.set_num_threads(args.threads)
    nets = {}
    for s in args.scorers:
        n, p = s.split('=', 1)
        nets[n] = load_net(p)
    rng = np.random.default_rng(args.seed)
    cols, obs_keep = {}, dict(tiles=[], globals=[], techs=[], pos=[])
    n_pos, t0 = 0, time.time()
    for gi, spec in enumerate(args.games):
        parts = spec.split(':')
        d, lim, val = parts[0], int(parts[1]) if len(parts) > 1 and parts[1] else 0, 'val' in parts[2:]
        files = train_bc.list_games(d, val=True) if val else sorted(
            os.path.join(d, f) for f in os.listdir(d) if f.endswith('.npz') and not f.endswith('.tmp.npz'))
        used = 0
        for fi, f in enumerate(files):
            if lim and used >= lim:
                break
            r = game_positions(f)
            if r is None:
                continue
            used += 1
            starts, ends, player, y, z = r
            idx = np.concatenate([starts, ends])
            kind = np.concatenate([np.zeros(len(starts), np.int8), np.ones(len(ends), np.int8)])
            if not len(idx):
                continue
            tiles, glob, techs = z['tiles'][idx], z['globals'][idx], z['techs'][idx]
            rec = facts(tiles, glob, techs)
            rec.update(kind=kind, decision=idx.astype(np.int32), player=player[idx].astype(np.int8), y=y[idx].astype(np.int8),
                       src=np.full(len(idx), gi, np.int8), game=np.full(len(idx), fi, np.int32),
                       start_turn=np.full(len(idx), int(z['start_turn']) if 'start_turn' in z.files else 0, np.int16),
                       opponent=np.full(len(idx), int(z['opponent']) if 'opponent' in z.files else -2, np.int16),
                       game_turns=np.full(len(idx), int(z['turns']), np.int16),
                       v_old=z['v_old'][idx].astype(np.float32) if 'v_old' in z.files else np.full(len(idx), np.nan, np.float32))
            for n, net in nets.items():
                rec['logit_' + n] = values(net, tiles, glob, techs)
            if args.with_techs:
                rec['techs'] = techs.astype(np.uint8)
                u, o = tiles[..., T['unit']].astype(np.int32), tiles[..., T['unit_owner']]
                rec['own_units64'] = np.stack([((u == k + 1) & (o == 1)).sum(1) for k in range(64)], 1).astype(np.int8)
            for k, v in rec.items():
                cols.setdefault(k, []).append(v)
            if args.keep_obs > 0:
                pick = np.nonzero((kind == 0) & (rng.random(len(idx)) < args.keep_obs))[0]
                obs_keep['tiles'].append(tiles[pick]); obs_keep['globals'].append(glob[pick]); obs_keep['techs'].append(techs[pick])
                obs_keep['pos'].append(n_pos + pick)
            n_pos += len(idx)
            if used % 50 == 1:
                print(f'{spec}: {used} games, {n_pos} positions, {time.time() - t0:.0f} s', flush=True)
    out = {k: np.concatenate(v) for k, v in cols.items()}
    out['src_names'] = np.array(args.games)
    np.savez_compressed(args.out, **out)
    if args.keep_obs > 0:
        np.savez_compressed(args.out.replace('.npz', '_obs.npz'), **{k: np.concatenate(v) for k, v in obs_keep.items()})
    print(f'wrote {args.out}: {n_pos} positions, {time.time() - t0:.0f} s', flush=True)


if __name__ == '__main__':
    main()
