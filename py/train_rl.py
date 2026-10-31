"""One policy-improvement step from the policy's own games (py/dagger.py output with --temperature 1):
PPO's clipped objective with the game's outcome as the only reward, plus a behaviour-cloning anchor.

- **Advantage:** every decision of a player in a game gets A = z - b, where z is 1 if that player won
  and 0 if they lost (capped games are skipped); with `--turn_cost c`, a win at turn T counts
  max(0.5, 1 - c T) instead, so faster wins are worth a little more (T counted from the game's
  `start_turn` for games that began from a human midgame position). With `--baseline opponent` (the default), b is the
  mean z of this batch's games against the same opponent (a bot difficulty, or self-play), and A is
  divided by its standard deviation over games. With `--baseline value`, b is the acting network's
  own value estimate before the move (`v_old`, recorded by py/selfplay.py), and A is centred and
  divided by its standard deviation over decisions: a baseline that depends only on the position
  adds no bias. With `--gae_lambda l` < 1 (value baseline only), credit goes move by move instead:
  each of a player's moves is judged by how its win estimate changed by the player's next move
  (delta = v_next - v, the outcome z after the last move), and A = sum_k l^k delta_{t+k}
  (generalized advantage estimation over the player's own decisions; l = 1 is the outcome-only
  advantage z - v_old above) (v_old estimates the chance of winning, not the turn-discounted z; that's still a
  function of the position only).
- **Policy loss:** -min(r A, clip(r, 1 - eps, 1 + eps) A) with r = pi(played) / pi_old(played),
  pi_old from the recorded `logp` (the sampling policy). pi is softmax(logits / --temperature), the
  same temperature the actors sampled at.
- **Anchor:** cross-entropy on batches of supervised data (--bc, train_bc.py's source syntax), weight
  --bc_weight, so the policy doesn't drift from what it learned by imitation.
- **Value:** BCE of the value head against the outcome (1 for a win, however long it took) on the RL
  games (weight --value_weight): the value head stays a probability of winning.
- Entropy bonus --entropy.
- **Anchor** (--anchor ckpt, --anchor_weight w): w KL(pi_ref || pi) over the legal rows of the RL
  batch's own positions, pi_ref the reference checkpoint's policy at the same temperature: a pull
  toward the reference (e.g. gen 0, trained on the human games) that replays no supervised data.
- **Critic** (--critic ckpt; research log S9): a separate value network, so value learning no longer
  shares the policy's trunk. Before the step it scores every recorded decision; those estimates
  replace `v_old` in the advantages. It then learns beside the policy, on its own optimizer
  (--critic_lr): BCE against the lambda-return over the player's own decisions (--critic_lambda; the
  outcome z after the last), plus --critic_own x the ownership loss. Its batches come from its own
  stream, not the policy's: a --critic_sample share of each game's decisions per pass (--critic_epochs),
  shuffled across --critic_files games at a time, so a batch holds few positions of any one game
  (the policy's stream, 16 games at a time, lets a value head memorize each game's outcome). With
  --critic_window, earlier generations' games join that stream, scored by the critic before the
  step the same way. Its steps are spread over the policy's, and it finishes its passes even if
  the policy stops early (--target_kl). Saved as <out_dir>/critic.pt. The policy's own value head
  still trains as before (--value_weight); only the advantages change.

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

MAX_LEGAL = 1024  # train_bc.collate's default max_legal


def game_targets(files, turn_cost=0.0):
    """Per (file, player) of the agent seats of finished games: (advantage with the per-opponent
    baseline, outcome, reward z); and the per-opponent baseline. The reward of a win at turn T is
    max(0.5, 1 - turn_cost T), of a loss 0."""
    rows = []
    for f in files:
        z = np.load(f)
        if not bool(z['ended']) or int(z['winner']) not in (1, 2):
            continue
        start = int(z['start_turn']) if 'start_turn' in z.files else 0
        rows.append((f, int(z['opponent']), [int(p) for p in z['agent_seats']], int(z['winner']), int(z['turns']) - start))
    win_z = lambda turns: max(0.5, 1.0 - turn_cost * turns)
    by_opp = {}
    for f, opp, seats, w, turns in rows:
        for p in seats:
            by_opp.setdefault(opp, []).append(win_z(turns) if p == w else 0.0)
    base = {o: float(np.mean(v)) for o, v in by_opp.items()}
    adv, won, reward = {}, {}, {}
    for f, opp, seats, w, turns in rows:
        for p in seats:
            won[(f, p)] = 1.0 if p == w else 0.0
            reward[(f, p)] = win_z(turns) if p == w else 0.0
            adv[(f, p)] = reward[(f, p)] - base[opp]
    sd = float(np.std(list(adv.values()))) or 1.0
    return {k: (v / sd, won[k], reward[k]) for k, v in adv.items()}, base, {o: len(v) for o, v in by_opp.items()}


def gae(f, player, v_old, adv, lam):
    """Per decision of the file's agent seats: the generalized advantage over that player's own
    decisions (NaN for other decisions and unfinished games)."""
    out = np.full(len(player), np.nan, np.float32)
    for p in np.unique(player):
        if (f, int(p)) not in adv:
            continue
        idx = np.nonzero(player == p)[0]
        v = v_old[idx].astype(np.float64)
        delta = np.append(v[1:], adv[(f, int(p))][2]) - v  # the next own move's estimate; the outcome after the last
        a, run = np.empty(len(idx)), 0.0
        for k in range(len(idx) - 1, -1, -1):
            run = delta[k] + lam * run
            a[k] = run
        out[idx] = a
    return out


class FileChunks(torch.utils.data.IterableDataset):
    """Each game file's observations, for scoring every decision with the critic: (file, tiles,
    globals, techs) per file, the files shared among the workers."""

    def __init__(self, files):
        self.files = sorted(files)

    def __iter__(self):
        info = torch.utils.data.get_worker_info()
        wid, nw = (info.id, info.num_workers) if info else (0, 1)
        for i, f in enumerate(self.files):
            if i % nw == wid:
                z = np.load(f)
                yield f, z['tiles'], z['globals'], z['techs']


@torch.no_grad()
def critic_values(critic, files, dev, workers, batch=1024):
    """{file: the critic's win probability before each recorded decision}."""
    critic.eval()
    out = {}
    loader = torch.utils.data.DataLoader(FileChunks(files), batch_size=None, num_workers=workers, prefetch_factor=4)
    for f, tiles, globs, techs in loader:
        vs = []
        for i in range(0, len(tiles), batch):
            t, g, te = (torch.as_tensor(x[i:i + batch]).to(dev) for x in (tiles, globs, techs))
            with train_bc.autocast(dev):
                v = critic.value(critic.encode(t, g, te)[:, 0]).squeeze(-1).float()
            vs.append(torch.sigmoid(v).cpu().numpy())
        out[f] = np.concatenate(vs).astype(np.float32)
    critic.train()
    return out


class RLStream(torch.utils.data.IterableDataset):
    """Epochs over the RL games: each worker takes its share of the files, loads them a window at a
    time, shuffles the decisions and yields batches with played, logp, advantage and value target.
    With `values` (the critic's estimates per file), they replace v_old, and each batch also carries
    the critic's target: the lambda-return with lambda `clam`. critic_only: no policy fields."""

    def __init__(self, files, adv, batch, window, epochs, seed, value_norm=None, lam=1.0, values=None, clam=0.95,
                 critic_only=False, sample=1.0):
        self.files, self.adv, self.batch, self.window, self.epochs, self.seed = files, adv, batch, window, epochs, seed
        self.value_norm = value_norm  # --baseline value: A = (z - v_old - mean) / sd, or (GAE - mean) / sd
        self.lam = lam
        self.values, self.clam, self.critic_only, self.sample = values, clam, critic_only, sample

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
                    a = np.array([self.adv.get((f, int(p)), (np.nan,) * 3) for p in player], np.float32).reshape(-1, 3)
                    v = self.values[f] if self.values is not None else z['v_old']
                    if self.value_norm and not self.critic_only:
                        mean, sd = self.value_norm
                        raw = a[:, 2] - v if self.lam >= 1 else gae(f, player, v, self.adv, self.lam)
                        a[:, 0] = np.where(np.isnan(a[:, 0]), np.nan, (raw - mean) / sd)
                    games.append(g)
                    e = dict(played=z['played'].astype(np.int64), logp=z['logp'].astype(np.float32), adv=a[:, 0], won=a[:, 1])
                    if self.values is not None:
                        e['ctarget'] = np.clip(v + gae(f, player, v, self.adv, self.clam), 0, 1)  # NaN off the agent seats
                    extra.append(e)
                # collate keeps at most MAX_LEGAL rows per decision; a played row beyond them can't be scored
                picks = [(gi, d) for gi, e in enumerate(extra) for d in range(len(e['adv']))
                         if not np.isnan(e['adv'][d]) and e['played'][d] < MAX_LEGAL]
                rng.shuffle(picks)
                if self.sample < 1:
                    picks = picks[:int(len(picks) * self.sample)]
                for j in range(0, len(picks) - self.batch + 1, self.batch):
                    chunk = picks[j:j + self.batch]
                    b = train_bc.collate(games, chunk)
                    b['played'] = np.array([extra[gi]['played'][d] for gi, d in chunk], np.int64)
                    b['logp'] = np.array([extra[gi]['logp'][d] for gi, d in chunk], np.float32)
                    b['adv'] = np.array([extra[gi]['adv'][d] for gi, d in chunk], np.float32)
                    b['won'] = np.array([extra[gi]['won'][d] for gi, d in chunk], np.float32)
                    if self.values is not None:
                        b['ctarget'] = np.array([extra[gi]['ctarget'][d] for gi, d in chunk], np.float32)
                    yield b


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('init')
    ap.add_argument('rl_dirs', help='comma-separated directories of py/dagger.py games')
    ap.add_argument('out_dir')
    ap.add_argument('--bc', default=None, help='supervised anchor data, train_bc.py source syntax')
    ap.add_argument('--bc_weight', type=float, default=0.5)
    ap.add_argument('--value_weight', type=float, default=0.25)
    ap.add_argument('--anchor', default=None, help='reference checkpoint for the KL anchor')
    ap.add_argument('--anchor_weight', type=float, default=0.0)
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
    ap.add_argument('--target_kl', type=float, default=0.0,
                    help='stop early once the mean KL to the acting policy over the last 50 steps passes this (0: never)')
    ap.add_argument('--gae_lambda', type=float, default=1.0,
                    help='with --baseline value: credit per move from changes in the win estimate (1: outcome only)')
    ap.add_argument('--turn_cost', type=float, default=0.0,
                    help='a win at turn T is worth max(0.5, 1 - turn_cost T) in the advantage (the value head still learns win/loss)')
    ap.add_argument('--baseline', choices=['opponent', 'value'], default='opponent',
                    help='opponent: per-opponent mean outcome; value: the acting network\'s v_old (py/selfplay.py games)')
    ap.add_argument('--critic', default=None, help='checkpoint of a separate value network for the advantages (it learns too)')
    ap.add_argument('--critic_lambda', type=float, default=0.95, help='lambda of the critic\'s lambda-return target')
    ap.add_argument('--critic_lr', type=float, default=1e-4)
    ap.add_argument('--critic_own', type=float, default=0.2, help='weight of the critic\'s ownership loss')
    ap.add_argument('--critic_window', default='', help='earlier generations\' game directories (comma-separated), for the critic only')
    ap.add_argument('--critic_sample', type=float, default=0.25, help='share of each game\'s decisions per critic pass')
    ap.add_argument('--critic_epochs', type=int, default=2)
    ap.add_argument('--critic_files', type=int, default=64, help='games shuffled together in the critic\'s stream')
    ap.add_argument('--device', default='cuda' if torch.cuda.is_available() else 'mps' if torch.backends.mps.is_available() else 'cpu')
    args = ap.parse_args()
    os.makedirs(args.out_dir, exist_ok=True)
    dev = args.device
    torch.manual_seed(args.seed)
    ck = torch.load(args.init, map_location='cpu')
    cfg = ck.get('config', dict(d=256, layers=6))
    net = M.Net(**cfg).to(dev)
    net.load_state_dict(ck['model'])
    ref = None
    if args.anchor and args.anchor_weight > 0:
        rk = torch.load(args.anchor, map_location='cpu')
        ref = M.Net(**rk.get('config', cfg)).to(dev).eval()
        ref.load_state_dict(rk['model'])
        for prm in ref.parameters():
            prm.requires_grad_(False)
    opt = torch.optim.AdamW(net.parameters(), lr=args.lr, weight_decay=0.01, betas=(0.9, 0.95), fused=dev == 'cuda')

    list_dir = lambda d: [os.path.join(d, f) for f in os.listdir(d) if f.endswith('.npz') and not f.endswith('.tmp.npz')]
    files = [f for d in args.rl_dirs.split(',') for f in list_dir(d)]
    adv, base, counts = game_targets(files, args.turn_cost)
    critic = cvals = None
    if args.critic:
        if args.baseline != 'value':
            ap.error('--critic needs --baseline value')
        kk = torch.load(args.critic, map_location='cpu')
        critic = M.Net(**kk.get('config', cfg)).to(dev)
        critic.load_state_dict(kk['model'])
        copt = torch.optim.AdamW(critic.parameters(), lr=args.critic_lr, weight_decay=0.01, betas=(0.9, 0.95), fused=dev == 'cuda')
        tc = time.time()
        cvals = critic_values(critic, files, dev, args.workers)
        v_rec = np.concatenate([np.load(f)['v_old'] for f in sorted(cvals)])
        v_cri = np.concatenate([cvals[f] for f in sorted(cvals)])
        print(f'critic {args.critic}: scored {len(v_cri)} decisions in {time.time() - tc:.0f} s; mean {v_cri.mean():.3f} '
              f'(policy head {v_rec.mean():.3f}), correlation with the policy head {np.corrcoef(v_rec, v_cri)[0, 1]:.3f}', flush=True)
    values_of = (lambda f, z: cvals[f]) if critic is not None else (lambda f, z: z['v_old'])
    value_norm = None
    if args.baseline == 'value':
        res, gres = [], []
        for f in files:
            z = np.load(f)
            for p, v in zip(z['player'], values_of(f, z)):
                if (f, int(p)) in adv:
                    res.append(adv[(f, int(p))][2] - float(v))
            if args.gae_lambda < 1:
                g = gae(f, z['player'].astype(np.int64), values_of(f, z), adv, args.gae_lambda)
                gres.extend(g[~np.isnan(g)].tolist())
        value_norm = (float(np.mean(res)), float(np.std(res)) or 1.0)
        print(f'value baseline: {len(res)} decisions, mean z - v_old {value_norm[0]:.3f}, sd {value_norm[1]:.3f}, '
              f'squared error {np.mean(np.square(res)):.3f} (outcome variance {np.var([w for _, w, _ in adv.values()]):.3f} per game)',
              flush=True)
        if args.gae_lambda < 1:
            value_norm = (float(np.mean(gres)), float(np.std(gres)) or 1.0)
            print(f'GAE lambda {args.gae_lambda}: advantage mean {value_norm[0]:.3f}, sd {value_norm[1]:.3f}', flush=True)
    if args.turn_cost:
        wins = [r for _, w, r in adv.values() if w]
        print(f'turn cost {args.turn_cost}: {len(wins)} wins, mean reward of a win {np.mean(wins):.3f} '
              f'(range {min(wins):.3f}-{max(wins):.3f})', flush=True)
    decisions = sum(len(np.load(f)['played']) for f in files)
    print(f'{len(files)} RL games, {decisions} decisions; baseline (win rate) by opponent: '
          + ', '.join(f"{'self' if o < 0 else o}: {base[o]:.3f} over {counts[o]}" for o in sorted(base)), flush=True)
    steps_total = max(1, args.epochs * decisions // args.batch)
    rl = torch.utils.data.DataLoader(RLStream(files, adv, args.batch, args.window, args.epochs, args.seed, value_norm,
                                              args.gae_lambda, values=cvals, clam=args.critic_lambda),
                                     batch_size=None, num_workers=args.workers, pin_memory=dev == 'cuda', prefetch_factor=4)
    c_iter = None
    if critic is not None:
        cfiles, cadv, cv_all = list(files), dict(adv), dict(cvals)
        wfiles = [f for d in args.critic_window.split(',') if d and os.path.isdir(d) for f in list_dir(d)]
        if wfiles:
            tc = time.time()
            cadv.update(game_targets(wfiles, args.turn_cost)[0])
            cv_all.update(critic_values(critic, wfiles, dev, args.workers))
            cfiles += wfiles
            print(f'critic window: {len(wfiles)} games from {args.critic_window}, scored in {time.time() - tc:.0f} s', flush=True)
        c_dec = sum(int(np.sum([(f, int(p)) in cadv for p in np.load(f)['player']])) for f in cfiles)
        c_total = max(1, int(args.critic_epochs * c_dec * args.critic_sample) // args.batch)
        c_every = max(1, steps_total // c_total)
        print(f'critic: {len(cfiles)} games, {c_dec} decisions, ~{c_total} steps (one per {c_every} policy steps)', flush=True)
        c_iter = iter(torch.utils.data.DataLoader(
            RLStream(cfiles, cadv, args.batch, args.critic_files, args.critic_epochs, args.seed + 11, values=cv_all,
                     clam=args.critic_lambda, critic_only=True, sample=args.critic_sample),
            batch_size=None, num_workers=max(2, args.workers // 2), pin_memory=dev == 'cuda', prefetch_factor=2))
    c_step = 0

    def critic_step(b):
        """One critic update on a batch with ctarget; returns (target loss, outcome loss, ownership loss)."""
        lr = args.critic_lr * min(1, (c_step + 1) / 100) * 0.5 * (1 + math.cos(math.pi * min(1, c_step / c_total)))
        for gr in copt.param_groups:
            gr['lr'] = lr
        with train_bc.autocast(dev):
            _, cv, cown = critic(b['tiles'], b['globals'], b['techs'], b['legal'], b['mask'])
        cv = cv.float()
        closs = F.binary_cross_entropy_with_logits(cv, b['ctarget'])
        cown_loss = F.cross_entropy(cown.float().reshape(-1, 3), b['own'].long().reshape(-1))
        copt.zero_grad(set_to_none=True)
        (closs + args.critic_own * cown_loss).backward()
        torch.nn.utils.clip_grad_norm_(critic.parameters(), 1.0)
        copt.step()
        with torch.no_grad():
            cz = F.binary_cross_entropy_with_logits(cv, b['won'])
        return closs.detach(), cz, cown_loss.detach()
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
        anc = torch.zeros((), device=dev)
        if ref is not None:  # KL(pi_ref || pi) on these positions
            with torch.no_grad(), train_bc.autocast(dev):
                rlogits, _, _ = ref(b['tiles'], b['globals'], b['techs'], b['legal'], b['mask'])
            rlogp = torch.log_softmax(rlogits.float() / args.temperature, 1)
            anc = (rlogp.exp() * (rlogp - logp_all).masked_fill(~b['mask'], 0)).sum(1).mean()
            loss = loss + args.anchor_weight * anc
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
                     kl=(b['logp'] - logp).mean(), anchor=anc)
        if c_iter is not None and step % c_every == 0:
            try:
                m['critic'], m['critic_z'], m['critic_own'] = critic_step(train_bc.to_device(next(c_iter), dev))
                c_step += 1
            except StopIteration:
                c_iter = None
        for k, x in m.items():
            tot, n = acc.get(k, (0.0, 0))
            acc[k] = (tot + float(x.detach()), n + 1)
        if step % 50 == 0:
            rec = dict(step=step, of=steps_total, lr=round(lr, 8), s=round(time.time() - t0), **{k: round(x / n, 4) for k, (x, n) in acc.items()})
            acc = {}
            print(json.dumps(rec), flush=True)
            log.write(json.dumps(rec) + '\n')
            log.flush()
            if args.target_kl and rec['kl'] > args.target_kl:  # PPO's early stop: the policy has moved enough
                print(f'stopping at step {step}: KL {rec["kl"]} > {args.target_kl}', flush=True)
                log.write(json.dumps(dict(step=step, stopped='target_kl', kl=rec['kl'])) + '\n')
                break
    torch.save(dict(model=net.state_dict(), step=ck.get('step', 0), rl_from=args.init, config=cfg),
               os.path.join(args.out_dir, 'policy.pt'))
    if c_iter is not None:  # the critic finishes its passes after the policy (an early stop, or rounding)
        acc, n0 = {}, c_step
        for b in c_iter:
            out = critic_step(train_bc.to_device(b, dev))
            c_step += 1
            for k, x in zip(('critic', 'critic_z', 'critic_own'), out):
                acc[k] = acc.get(k, 0.0) + float(x)
            if (c_step - n0) % 50 == 0:
                rec = dict(critic_step=c_step, of=c_total, s=round(time.time() - t0), **{k: round(x / 50, 4) for k, x in acc.items()})
                acc = {}
                print(json.dumps(rec), flush=True)
                log.write(json.dumps(rec) + '\n')
                log.flush()
        print(f'critic: {c_step} steps ({c_step - n0} after the policy)', flush=True)
    if critic is not None:
        torch.save(dict(model=critic.state_dict(), config=kk.get('config', cfg), critic_from=args.critic),
                   os.path.join(args.out_dir, 'critic.pt'))
    print(f'done: {step} steps in {time.time() - t0:.0f} s', flush=True)


if __name__ == '__main__':
    main()
