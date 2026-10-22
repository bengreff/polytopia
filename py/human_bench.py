"""Agreement with held-out human (or AI) games: how often a checkpoint's policy picks the recorded move,
and how well its value head predicts the winner.

Uses train_bc's validation split (human games whose CRC32 of the game id is a multiple of 10; AI
games whose seed is a multiple of 50) and train_bc's acceptance rule. Reports policy top-1, top-5 and
cross-entropy over every recorded decision, and value accuracy and log loss over decisions of
games with a value target, overall and by turn bucket of the game (early / middle / late thirds of
each game's decisions).

    .venv/bin/python py/human_bench.py <checkpoint.pt> [--data data/human1] [--games 150]
"""
import argparse
import os

import numpy as np
import torch
import torch.nn.functional as F

import eval_vs_bot
import train_bc


@torch.no_grad()
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('checkpoint')
    ap.add_argument('--data', default='data/human1')
    ap.add_argument('--games', type=int, default=150, help='first N held-out games by file name')
    ap.add_argument('--batch', type=int, default=256)
    ap.add_argument('--device', default='mps' if torch.backends.mps.is_available() else 'cpu')
    args = ap.parse_args()
    net, step = eval_vs_bot.load_policy(args.checkpoint, args.device)
    files = train_bc.list_games(args.data, val=True)
    games = []
    for f in files:
        g = train_bc.load_game(f)
        if g is not None:
            games.append(g)
        if len(games) >= args.games:
            break
    top1, top5, nll, n = 0, 0, 0.0, 0
    vstats = np.zeros((3, 3))  # per third: correct, log loss sum, count
    for g in games:
        D = len(g['label'])
        third = np.minimum(np.arange(D) * 3 // max(D, 1), 2)
        for i in range(0, D, args.batch):
            picks = [(0, d) for d in range(i, min(D, i + args.batch))]
            b = train_bc.to_device(train_bc.collate([g], picks), args.device)
            logits, v, _ = net(b['tiles'], b['globals'], b['techs'], b['legal'], b['mask'])
            logits, v = logits.float(), v.float()
            lab = b['label']
            top1 += int((logits.argmax(1) == lab).sum())
            k = min(5, logits.shape[1])
            top5 += int((logits.topk(k, 1).indices == lab[:, None]).any(1).sum())
            nll += float(F.cross_entropy(logits, lab, reduction='sum'))
            n += len(picks)
            has = b['value'] >= 0
            if has.any():
                t = third[i:i + len(picks)][has.cpu().numpy()]
                corr = ((v[has] > 0).float() == b['value'][has]).float().cpu().numpy()
                ll = F.binary_cross_entropy_with_logits(v[has], b['value'][has], reduction='none').cpu().numpy()
                for j in range(3):
                    m = t == j
                    vstats[j] += [corr[m].sum(), ll[m].sum(), m.sum()]
    print(f'checkpoint step {step} on {len(games)} held-out games of {os.path.basename(os.path.normpath(args.data))} '
          f'({n} decisions): top-1 {top1 / n:.1%}, top-5 {top5 / n:.1%}, policy CE {nll / n:.3f}')
    tot = vstats.sum(0)
    if tot[2]:
        parts = ', '.join(f'{name} {s[0] / s[2]:.1%}' for name, s in zip(('early', 'middle', 'late'), vstats) if s[2])
        print(f'  value: accuracy {tot[0] / tot[2]:.1%}, log loss {tot[1] / tot[2]:.3f} over {int(tot[2])} decisions '
              f'({parts})')


if __name__ == '__main__':
    main()
