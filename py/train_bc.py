"""M1: behaviour cloning of the game's own AI on championship games (py/gen_data.py output).

Losses: cross-entropy of the pointer policy against the AI's choice; binary cross-entropy of the
value head against the result for the player to move (finished games only); cross-entropy of
each tile's final owner (none / mine / the opponent's).

    python train_bc.py <data_dir> <run_dir> [--steps 60000] [--batch 256]

Games whose seed is a multiple of 50 are held out for validation.
"""
import argparse
import json
import math
import os
import random
import subprocess
import time

import numpy as np
import torch
import torch.nn.functional as F

import model as M
import polyenv

VAL_EVERY = 50


def list_games(data_dir, val, min_seed=None):
    out = []
    for f in os.listdir(data_dir):
        if f.startswith('g') and f.endswith('.npz') and not f.endswith('.tmp.npz'):
            seed = int(f[1:9])
            if (seed % VAL_EVERY == 0) == val and (min_seed is None or seed >= min_seed):
                out.append(os.path.join(data_dir, f))
    return sorted(out)


def load_game(path):
    z = np.load(path)
    D = len(z['label'])
    player = z['player'].astype(np.int64)
    ended, winner = bool(z['ended']), int(z['winner'])
    value = np.where(player == winner, 1.0, 0.0).astype(np.float32) if ended else np.full(D, -1.0, np.float32)
    owner = z['final_owner'].astype(np.int64)
    own = np.where(owner[None, :] == player[:, None], 1, np.where((owner[None, :] == 1) | (owner[None, :] == 2), 2, 0))
    return dict(tiles=z['tiles'], globals=z['globals'], techs=z['techs'], legal=z['legal'],
                offsets=z['legal_offsets'], label=z['label'].astype(np.int64), value=value, own=own.astype(np.int8))


def collate(games, picks, max_legal=1024, rng=None):
    """picks: [(game index, decision index)] -> a padded batch of numpy arrays. With rng, each label
    is replaced by a uniformly random legal row (the R2 canary)."""
    B = len(picks)
    n = [int(games[g]['offsets'][d + 1] - games[g]['offsets'][d]) for g, d in picks]
    N = min(max(n), max_legal)
    legal = np.zeros((B, N, 4), np.int16)
    mask = np.zeros((B, N), bool)
    label = np.zeros(B, np.int64)
    for i, (g, d) in enumerate(picks):
        o = games[g]['offsets'][d]
        k = min(n[i], N)
        legal[i, :k] = games[g]['legal'][o:o + k]
        mask[i, :k] = True
        label[i] = min(int(games[g]['label'][d]), k - 1) if rng is None else rng.randrange(k)
    return dict(
        tiles=np.stack([games[g]['tiles'][d] for g, d in picks]),
        globals=np.stack([games[g]['globals'][d] for g, d in picks]),
        techs=np.stack([games[g]['techs'][d] for g, d in picks]),
        legal=legal, mask=mask, label=label,
        value=np.array([games[g]['value'][d] for g, d in picks], np.float32),
        own=np.stack([games[g]['own'][d] for g, d in picks]))


class Stream(torch.utils.data.IterableDataset):
    """Each worker loads a window of random training games, shuffles their decisions, and yields
    batches; the game list is re-read every window, so data still being generated joins in."""

    def __init__(self, data_dir, batch, window, seed, min_seed=None, random_labels=False):
        self.data_dir, self.batch, self.window, self.seed = data_dir, batch, window, seed
        self.min_seed, self.random_labels = min_seed, random_labels

    def __iter__(self):
        info = torch.utils.data.get_worker_info()
        rng = random.Random(self.seed * 1000 + (info.id if info else 0))
        while True:
            files = list_games(self.data_dir, val=False, min_seed=self.min_seed)
            games = []
            for f in rng.sample(files, min(self.window, len(files))):
                try:
                    games.append(load_game(f))
                except Exception as e:  # e.g. a file still being copied: skip it, never kill the run
                    print(f'skipping unreadable {f}: {e}', flush=True)
            picks = [(g, d) for g, game in enumerate(games) for d in range(len(game['label']))]
            rng.shuffle(picks)
            for i in range(0, len(picks) - self.batch + 1, self.batch):
                yield collate(games, picks[i:i + self.batch], rng=rng if self.random_labels else None)


def to_device(b, dev):
    return {k: (v if torch.is_tensor(v) else torch.from_numpy(v)).to(dev, non_blocking=True) for k, v in b.items()}


def losses(net, b):
    logits, v, own = net(b['tiles'], b['globals'], b['techs'], b['legal'], b['mask'])
    pol = F.cross_entropy(logits, b['label'])
    has_v = b['value'] >= 0
    val = F.binary_cross_entropy_with_logits(v[has_v], b['value'][has_v]) if has_v.any() else v.sum() * 0
    own_loss = F.cross_entropy(own.reshape(-1, 3), b['own'].long().reshape(-1))
    acc = (logits.argmax(1) == b['label']).float().mean()
    vacc = ((v[has_v] > 0).float() == b['value'][has_v]).float().mean() if has_v.any() else acc * 0
    return pol, val, own_loss, acc, vacc


FP32 = False  # --fp32: no bf16 autocast


def autocast(dev):
    if FP32 or dev not in ('cuda', 'mps'):
        return torch.autocast('cpu', enabled=False)
    return torch.autocast(dev, dtype=torch.bfloat16)


@torch.no_grad()
def validate(net, val_games, dev, batch=256):
    net.eval()
    sums, count = np.zeros(5), 0
    picks = [(g, d) for g, game in enumerate(val_games) for d in range(len(game['label']))]
    for i in range(0, len(picks), batch):
        b = to_device(collate(val_games, picks[i:i + batch]), dev)
        with autocast(dev):
            out = losses(net, b)
        k = len(picks[i:i + batch])
        sums += np.array([float(x) for x in out]) * k
        count += k
    net.train()
    return dict(zip(['policy', 'value', 'owner', 'top1', 'value_acc'], (sums / count).round(4).tolist()))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('data_dir')
    ap.add_argument('run_dir')
    ap.add_argument('--steps', type=int, default=60000)
    ap.add_argument('--batch', type=int, default=256)
    ap.add_argument('--lr', type=float, default=3e-4)
    ap.add_argument('--d', type=int, default=256)
    ap.add_argument('--layers', type=int, default=6)
    ap.add_argument('--window', type=int, default=24, help='games per worker shuffle window')
    ap.add_argument('--workers', type=int, default=10)
    ap.add_argument('--val_games', type=int, default=40)
    ap.add_argument('--value_weight', type=float, default=0.5)
    ap.add_argument('--owner_weight', type=float, default=0.2)
    ap.add_argument('--seed', type=int, default=0)
    ap.add_argument('--resume', action='store_true')
    ap.add_argument('--device', default='cuda' if torch.cuda.is_available() else 'mps' if torch.backends.mps.is_available() else 'cpu')
    ap.add_argument('--min_seed', type=int, default=None, help='train only on games with seed >= this')
    ap.add_argument('--random_labels', action='store_true', help='the R2 canary: uniformly random policy labels')
    ap.add_argument('--minutes', type=float, default=None, help='stop after this much wall time')
    ap.add_argument('--fp32', action='store_true', help='train in float32 (no bf16 autocast)')
    ap.add_argument('--val_every', type=int, default=2000, help='validate and save last.pt every this many steps')
    ap.add_argument('--save_every', type=int, default=10000, help='keep a stepNNNNNN.pt checkpoint every this many steps')
    args = ap.parse_args()
    global FP32
    FP32 = args.fp32
    os.makedirs(args.run_dir, exist_ok=True)
    torch.manual_seed(args.seed)
    dev = args.device
    net = M.Net(d=args.d, layers=args.layers).to(dev)
    opt = torch.optim.AdamW(net.parameters(), lr=args.lr, weight_decay=0.01, betas=(0.9, 0.95), fused=dev == 'cuda')
    step = 0
    ckpt_path = os.path.join(args.run_dir, 'last.pt')
    if args.resume and os.path.exists(ckpt_path):
        ck = torch.load(ckpt_path, map_location=dev)
        net.load_state_dict(ck['model'])
        opt.load_state_dict(ck['opt'])
        step = ck['step']
    n_params = sum(p.numel() for p in net.parameters())
    if dev == 'cuda' and os.environ.get('CC'):  # torch.compile needs a C compiler; the fixed-shape encoder gains ~1.4x
        net.encode = torch.compile(net.encode)
    try:
        commit = subprocess.run(['git', 'rev-parse', '--short', 'HEAD'], capture_output=True, text=True,
                                cwd=os.path.dirname(os.path.abspath(__file__))).stdout.strip()
    except OSError:
        commit = ''
    json.dump(dict(vars(args), params=n_params, obs_version=polyenv.OBS_VERSION, commit=commit,
                   started=time.strftime('%Y-%m-%d %H:%M:%S')), open(os.path.join(args.run_dir, 'config.json'), 'w'),
              indent=1)
    print(f'{n_params / 1e6:.1f}M parameters', flush=True)

    val_files = list_games(args.data_dir, val=True)[:args.val_games]
    val_games = [load_game(f) for f in val_files]
    print(f'validation: {len(val_games)} games, {sum(len(g["label"]) for g in val_games)} decisions', flush=True)
    stream = Stream(args.data_dir, args.batch, args.window, args.seed + step, args.min_seed, args.random_labels)
    loader = torch.utils.data.DataLoader(stream, batch_size=None, num_workers=args.workers, pin_memory=dev == 'cuda',
                                         prefetch_factor=4, persistent_workers=True)
    log = open(os.path.join(args.run_dir, 'log.jsonl'), 'a')
    warmup = 1000
    t0, seen, acc_sum = time.time(), 0, np.zeros(5)
    for batch in loader:
        if step >= args.steps or (args.minutes and time.time() - t0 > 60 * args.minutes):
            break
        lr = args.lr * min(1, (step + 1) / warmup) * 0.5 * (1 + math.cos(math.pi * min(1, step / args.steps)))
        for gr in opt.param_groups:
            gr['lr'] = lr
        b = to_device(batch, dev)
        with autocast(dev):
            pol, val, own, acc, vacc = losses(net, b)
            loss = pol + args.value_weight * val + args.owner_weight * own
        opt.zero_grad(set_to_none=True)
        loss.backward()
        torch.nn.utils.clip_grad_norm_(net.parameters(), 1.0)
        opt.step()
        step += 1
        seen += len(b['label'])
        acc_sum += np.array([float(x.detach()) for x in (pol, val, own, acc, vacc)])
        if step % 200 == 0:
            m = dict(zip(['policy', 'value', 'owner', 'top1', 'value_acc'], (acc_sum / 200).round(4).tolist()))
            acc_sum[:] = 0
            rec = dict(step=step, lr=round(lr, 7), samples_per_s=round(seen / (time.time() - t0)), train=m,
                       games=len(list_games(args.data_dir, False, args.min_seed)))
            if step % args.val_every == 0:
                rec['val'] = validate(net, val_games, dev)
                torch.save(dict(model=net.state_dict(), opt=opt.state_dict(), step=step,
                                config=dict(d=args.d, layers=args.layers)), ckpt_path)
                if step % args.save_every == 0:
                    torch.save(dict(model=net.state_dict(), step=step, config=dict(d=args.d, layers=args.layers)),
                               os.path.join(args.run_dir, f'step{step:06d}.pt'))
            print(json.dumps(rec), flush=True)
            log.write(json.dumps(rec) + '\n')
            log.flush()
    torch.save(dict(model=net.state_dict(), opt=opt.state_dict(), step=step, config=dict(d=args.d, layers=args.layers)),
               ckpt_path)
    print('done', json.dumps(validate(net, val_games, dev)), flush=True)


if __name__ == '__main__':
    main()
