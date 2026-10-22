"""One policy-improvement step from the policy's own games (py/dagger.py output with --temperature 1):
PPO's clipped objective with the game's outcome as the only reward, plus a behaviour-cloning anchor.

- **Advantage:** every decision of a player in a game gets A = z - b, where z is 1 if that player won
  and 0 if they lost (capped games are skipped), and b is the mean z of this batch's games against the
  same opponent (a bot difficulty, or self-play). A is then divided by its standard deviation over
  games. No learned baseline: the value head isn't trustworthy yet (research-log, R3).
- **Policy loss:** -min(r A, clip(r, 1 - eps, 1 + eps) A) with r = pi(played) / pi_old(played),
  pi_old from the recorded `logp` (the sampling policy). pi is softmax(logits / --temperature), the
  same temperature the actors sampled at.
- **Anchor:** cross-entropy on batches of supervised data (--bc, train_bc.py's source syntax), weight
  --bc_weight, so the policy doesn't drift from what it learned by imitation.
- **Value:** BCE of the value head against z on the RL games (weight --value_weight).
- Entropy bonus --entropy.

    python train_rl.py <init.pt> <rl_dir[,rl_dir2...]> <out_dir> --bc human1:0.5,m1v2:0.5 [--epochs 2]
"""
import argparse
import json
import math
import os
import random
import time

import numpy as np
import torch
import torch.nn.functional as F

import model as M
import train_bc


def game_targets(files):
    """Per file: {player id: z} for the agent seats of finished games, and the opponent; then the
    per-opponent baseline and the advantage per (file, player)."""
    rows = []
    for f in files:
        z = np.load(f)
        if not bool(z['ended']) or int(z['winner']) not in (1, 2):
            continue
        rows.append((f, int(z['opponent']), [int(p) for p in z['agent_seats']], int(z['winner'])))
    by_opp = {}
    for f, opp, seats, w in rows:
        for p in seats:
            by_opp.setdefault(opp, []).append(1.0 if p == w else 0.0)
    base = {o: float(np.mean(v)) for o, v in by_opp.items()}
    adv, won = {}, {}
    for f, opp, seats, w in rows:
        for p in seats:
            won[(f, p)] = 1.0 if p == w else 0.0
            adv[(f, p)] = won[(f, p)] - base[opp]
    sd = float(np.std(list(adv.values()))) or 1.0
    return {k: (v / sd, won[k]) for k, v in adv.items()}, base, {o: len(v) for o, v in by_opp.items()}


class RLStream(torch.utils.data.IterableDataset):
    """Epochs over the RL games: each worker takes its share of the files, loads them a window at a
    time, shuffles the decisions and yields batches with played, logp, advantage and value target."""

    def __init__(self, files, adv, batch, window, epochs, seed):
        self.files, self.adv, self.batch, self.window, self.epochs, self.seed = files, adv, batch, window, epochs, seed

    def __iter__(self):
        info = torch.utils.data.get_worker_info()
        wid, nw = (info.id, info.num_workers) if info else (0, 1)
        rng = random.Random(self.seed * 1000 + wid)
        for ep in range(self.epochs):
            files = [f for i, f in enumerate(sorted(self.files)) if i % nw == wid]
            rng.shuffle(files)
            for i in range(0, len(files), self.window):
                games, extra = [], []
                for f in files[i:i + self.window]:
                    g = train_bc.load_game(f)
                    z = np.load(f)
                    player = z['player'].astype(np.int64)
                    a = np.array([self.adv.get((f, int(p)), (np.nan, np.nan)) for p in player], np.float32).reshape(-1, 2)
                    games.append(g)
                    extra.append(dict(played=z['played'].astype(np.int64), logp=z['logp'].astype(np.float32),
                                      adv=a[:, 0], won=a[:, 1]))
                picks = [(gi, d) for gi, e in enumerate(extra) for d in range(len(e['adv'])) if not np.isnan(e['adv'][d])]
                rng.shuffle(picks)
                for j in range(0, len(picks) - self.batch + 1, self.batch):
                    chunk = picks[j:j + self.batch]
                    b = train_bc.collate(games, chunk)
                    b['played'] = np.array([extra[gi]['played'][d] for gi, d in chunk], np.int64)
                    b['logp'] = np.array([extra[gi]['logp'][d] for gi, d in chunk], np.float32)
                    b['adv'] = np.array([extra[gi]['adv'][d] for gi, d in chunk], np.float32)
                    b['won'] = np.array([extra[gi]['won'][d] for gi, d in chunk], np.float32)
                    yield b


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('init')
    ap.add_argument('rl_dirs', help='comma-separated directories of py/dagger.py games')
    ap.add_argument('out_dir')
    ap.add_argument('--bc', default=None, help='supervised anchor data, train_bc.py source syntax')
    ap.add_argument('--bc_weight', type=float, default=0.5)
    ap.add_argument('--value_weight', type=float, default=0.25)
    ap.add_argument('--entropy', type=float, default=0.003)
    ap.add_argument('--clip', type=float, default=0.2)
    ap.add_argument('--temperature', type=float, default=1.0,
                    help='the policy being improved is softmax(logits / T); must equal the actors\' --temperature')
    ap.add_argument('--lr', type=float, default=3e-5)
    ap.add_argument('--epochs', type=int, default=2)
    ap.add_argument('--batch', type=int, default=256)
    ap.add_argument('--window', type=int, default=16)
    ap.add_argument('--workers', type=int, default=6)
    ap.add_argument('--seed', type=int, default=0)
    ap.add_argument('--device', default='cuda' if torch.cuda.is_available() else 'mps' if torch.backends.mps.is_available() else 'cpu')
    args = ap.parse_args()
    os.makedirs(args.out_dir, exist_ok=True)
    dev = args.device
    torch.manual_seed(args.seed)
    ck = torch.load(args.init, map_location='cpu')
    cfg = ck.get('config', dict(d=256, layers=6))
    net = M.Net(**cfg).to(dev)
    net.load_state_dict(ck['model'])
    opt = torch.optim.AdamW(net.parameters(), lr=args.lr, weight_decay=0.01, betas=(0.9, 0.95), fused=dev == 'cuda')

    files = [os.path.join(d, f) for d in args.rl_dirs.split(',') for f in os.listdir(d)
             if f.endswith('.npz') and not f.endswith('.tmp.npz')]
    adv, base, counts = game_targets(files)
    decisions = sum(len(np.load(f)['played']) for f in files)
    print(f'{len(files)} RL games, {decisions} decisions; baseline (win rate) by opponent: '
          + ', '.join(f"{'self' if o < 0 else o}: {base[o]:.3f} over {counts[o]}" for o in sorted(base)), flush=True)
    steps_total = max(1, args.epochs * decisions // args.batch)
    rl = torch.utils.data.DataLoader(RLStream(files, adv, args.batch, args.window, args.epochs, args.seed),
                                     batch_size=None, num_workers=args.workers, pin_memory=dev == 'cuda', prefetch_factor=4)
    bc_iter = None
    if args.bc:
        bc = torch.utils.data.DataLoader(train_bc.Stream(args.bc, args.batch, 24, args.seed + 7), batch_size=None,
                                         num_workers=max(1, args.workers // 2), pin_memory=dev == 'cuda', prefetch_factor=4)
        bc_iter = iter(bc)
    log = open(os.path.join(args.out_dir, 'log.jsonl'), 'a')
    json.dump(dict(vars(args), rl_games=len(files), rl_decisions=decisions, baseline=base, started=time.strftime('%Y-%m-%d %H:%M:%S')),
              open(os.path.join(args.out_dir, 'config.json'), 'w'), indent=1)
    t0, step, acc = time.time(), 0, {}
    for b in rl:
        lr = args.lr * min(1, (step + 1) / 100) * 0.5 * (1 + math.cos(math.pi * min(1, step / steps_total)))
        for gr in opt.param_groups:
            gr['lr'] = lr
        b = train_bc.to_device(b, dev)
        with train_bc.autocast(dev):
            logits, v, _ = net(b['tiles'], b['globals'], b['techs'], b['legal'], b['mask'])
        logits = logits.float()
        logp_all = torch.log_softmax(logits / args.temperature, 1)
        logp = logp_all.gather(1, b['played'][:, None]).squeeze(1)
        ratio = torch.exp(logp - b['logp'])
        A = b['adv']
        pg = -torch.min(ratio * A, ratio.clamp(1 - args.clip, 1 + args.clip) * A).mean()
        p = logp_all.exp()
        ent = -(p * logp_all.masked_fill(~b['mask'], 0)).sum(1).mean()  # padding is -inf: keep NaN out of the gradient
        val = F.binary_cross_entropy_with_logits(v.float(), b['won'])
        loss = pg - args.entropy * ent + args.value_weight * val
        bcl = torch.zeros((), device=dev)
        if bc_iter is not None:
            bb = train_bc.to_device(next(bc_iter), dev)
            with train_bc.autocast(dev):
                pol, _, _, _, _ = train_bc.losses(net, bb)
            bcl = pol.float()
            loss = loss + args.bc_weight * bcl
        opt.zero_grad(set_to_none=True)
        loss.backward()
        torch.nn.utils.clip_grad_norm_(net.parameters(), 1.0)
        opt.step()
        step += 1
        with torch.no_grad():
            m = dict(pg=pg, ent=ent, value=val, bc=bcl, ratio=ratio.mean(), clipfrac=((ratio - 1).abs() > args.clip).float().mean(),
                     kl=(b['logp'] - logp).mean())
        for k, x in m.items():
            acc[k] = acc.get(k, 0.0) + float(x.detach())
        if step % 50 == 0:
            rec = dict(step=step, of=steps_total, lr=round(lr, 8), s=round(time.time() - t0), **{k: round(x / 50, 4) for k, x in acc.items()})
            acc = {}
            print(json.dumps(rec), flush=True)
            log.write(json.dumps(rec) + '\n')
            log.flush()
    torch.save(dict(model=net.state_dict(), step=ck.get('step', 0), rl_from=args.init, config=cfg),
               os.path.join(args.out_dir, 'policy.pt'))
    print(f'done: {step} steps in {time.time() - t0:.0f} s', flush=True)


if __name__ == '__main__':
    main()
