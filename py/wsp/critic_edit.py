"""WSP-1 Part A: closed-form edits of the fog critic's value head (research log WSP-1).

Edit `star_dir`: the critic may overvalue stars (outcome residual about -0.008 logit per star at
turn starts in held-out self-play, with other facts as covariates). The value head is
logit = w2 . GELU(W1 hg + b1) + b2 on the final global token hg. The edit adds about c * (u . hg + a)
to the logit, where u . hg + a is a ridge probe of the observation's star count from hg (fitted on
tuning positions). No hidden unit is active on most positions, so one unit is repurposed (the one
whose contribution varies least; its mean goes into b2): input row c * u, bias 40 (GELU linear
there), output weight 1. Four tensors touched: one row of W1, one entry of b1, one of w2, and b2.
Edit `star_input`: scale the two star columns of the global input embedding (g_dense: stars / 20
and log(1 + stars)) by s. A cruder edit, kept for comparison.

Writes the checkpoint, plus <out>.json: the source's hash, the edit, the tensors touched and how much
they changed, and the fit on the tuning positions.

    python py/wsp/critic_edit.py critic_g083.pt pos_sp_obs.npz pos_sp.npz --edit star_dir --c -0.0078 --out edited.pt
"""
import argparse
import hashlib
import json
import os
import sys

import numpy as np
import torch
import torch.nn.functional as F

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import polyenv  # noqa: E402
from positions import load_net  # noqa: E402

G = {n: i for i, n in enumerate(polyenv.GLOBAL_FEATURES)}


def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()[:16]


@torch.no_grad()
def hidden(net, tiles, glob, techs, batch=128):
    """hg [N, d], the value head's pre-activations [N, d], and the logit [N]."""
    hs, pres, lgs = [], [], []
    for i in range(0, len(tiles), batch):
        t, g, k = (torch.from_numpy(np.ascontiguousarray(x[i:i + batch])) for x in (tiles, glob, techs))
        hg = net.encode(t, g, k)[:, 0]
        pre = net.value[0](hg)
        hs.append(hg.numpy()); pres.append(pre.numpy()); lgs.append(net.value(hg).squeeze(-1).numpy())
    return np.concatenate(hs), np.concatenate(pres), np.concatenate(lgs)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('critic')
    ap.add_argument('obs', help='positions.py --keep_obs file (turn-start observations, with pos = row in the table)')
    ap.add_argument('table', help='the positions.py table the obs came from (for the game split)')
    ap.add_argument('--edit', choices=['star_dir', 'star_input'], required=True)
    ap.add_argument('--c', type=float, default=-0.0078, help='star_dir: logit change per star')
    ap.add_argument('--s', type=float, default=0.75, help='star_input: scale of the star input columns')
    ap.add_argument('--ridge', type=float, default=1.0)
    ap.add_argument('--tune_parity', type=int, default=0, help='tuning positions: games with this index parity')
    ap.add_argument('--threads', type=int, default=4)
    ap.add_argument('--out', required=True)
    args = ap.parse_args()
    torch.set_num_threads(args.threads)
    ck = torch.load(args.critic, map_location='cpu', weights_only=False)
    net = load_net(args.critic)
    z, tab = np.load(args.obs), np.load(args.table)
    game = tab['game'][z['pos']]
    tune = game % 2 == args.tune_parity
    tiles, glob, techs = z['tiles'], z['globals'], z['techs']
    stars = glob[:, G['stars']].astype(np.float64)
    hg, pre, lg0 = hidden(net, tiles, glob, techs)
    info = dict(source=args.critic, source_sha=sha(args.critic), edit=args.edit, tuning_positions=int(tune.sum()),
                held_out_positions=int((~tune).sum()), tune_parity=args.tune_parity)
    sd = {k: v.clone() for k, v in ck['model'].items()}
    if args.edit == 'star_dir':
        X = hg[tune].astype(np.float64)
        mu = X.mean(0)
        Xc = X - mu
        u = np.linalg.solve(Xc.T @ Xc + args.ridge * np.eye(X.shape[1]), Xc.T @ (stars[tune] - stars[tune].mean()))
        a = stars[tune].mean() - mu @ u
        fit = hg.astype(np.float64) @ u + a
        r2 = lambda m: 1 - ((fit[m] - stars[m]) ** 2).sum() / ((stars[m] - stars[m].mean()) ** 2).sum()
        W1, b1 = net.value[0].weight.detach().numpy().astype(np.float64), net.value[0].bias.detach().numpy().astype(np.float64)
        w2 = net.value[2].weight.detach().numpy()[0].astype(np.float64)
        gelu = lambda x: F.gelu(torch.from_numpy(np.asarray(x, np.float64))).numpy()
        # No hidden unit of the value head is active on most positions (every unit's 10th percentile
        # pre-activation is below -0.69), so there is no linear path to shift. Instead one unit is
        # repurposed: the one whose contribution to the logit varies least over the tuning positions
        # (its loss is replaced by its mean, in b2). Its new input row is the star probe, with a large
        # bias B so GELU is linear on it: it adds c * (u . hg) + const, and b2 absorbs the constants.
        contrib = gelu(pre[tune].astype(np.float64)) * w2[None]
        k = int(np.argmin(contrib.std(0)))
        B = 40.0
        W1k = args.c * u  # with w2[k] set to 1 below
        sd['value.0.weight'][k] = torch.from_numpy(W1k.astype(np.float32))
        sd['value.0.bias'][k] = float(B)
        sd['value.2.weight'][0, k] = 1.0
        # b2: + old unit's mean contribution - B + c * a (so the change is c * (u . hg + a) on average)
        const = float(contrib[:, k].mean()) - B + args.c * a
        sd['value.2.bias'] = sd['value.2.bias'] + const
        info.update(c=args.c, probe_r2_tune=float(r2(tune)), probe_r2_held=float(r2(~tune)), probe_intercept=float(a),
                    unit=k, unit_contrib_sd=float(contrib[:, k].std()), unit_contrib_mean=float(contrib[:, k].mean()),
                    old_w2=float(w2[k]), bias_B=B, b2_shift=const,
                    weights_touched=f'value.0.weight row {k} (256), value.0.bias[{k}], value.2.weight[0, {k}], value.2.bias')
    else:
        Wg = sd['g_dense.weight']
        import model as M
        scaled = list(M.GLOBAL_SCALED)
        cols = [scaled.index('stars'), len(scaled)]  # stars / 20, and log1p(stars) right after the scaled block
        old = Wg[:, cols].clone()
        Wg[:, cols] = Wg[:, cols] * args.s
        info.update(s=args.s, columns=cols, weights_touched=f'g_dense.weight columns {cols} (2 x {Wg.shape[0]})',
                    dW_norm=float((Wg[:, cols] - old).norm()))
    out = dict(ck, model=sd, wsp=info)
    torch.save(out, args.out)
    # check the edit with real forward passes: realized change vs c * stars
    net2 = load_net(args.out)
    _, _, lg1 = hidden(net2, tiles, glob, techs)
    d = lg1 - lg0
    for name, m in (('tuning', tune), ('held-out', ~tune)):
        A = np.stack([np.ones(m.sum()), stars[m]], 1)
        coef, *_ = np.linalg.lstsq(A, d[m], rcond=None)
        resid = d[m] - A @ coef
        info[f'realized_{name}'] = dict(slope_per_star=float(coef[1]), intercept=float(coef[0]),
                                        r2=float(1 - resid.var() / max(d[m].var(), 1e-12)), mean=float(d[m].mean()),
                                        sd=float(d[m].std()))
    # sensitivity: the edited critic's d logit for +1 star (probe), before and after
    g2 = glob.copy(); g2[:, G['stars']] += 1
    _, _, a0 = hidden(net, tiles, g2, techs)
    _, _, a1 = hidden(net2, tiles, g2, techs)
    info['probe_plus1_star'] = dict(before=float((a0 - lg0).mean()), after=float((a1 - lg1).mean()))
    info['out'] = args.out
    info['out_sha'] = sha(args.out)
    torch.save(dict(out, wsp=info), args.out)
    info['out_sha'] = sha(args.out)
    json.dump(info, open(args.out + '.json', 'w'), indent=1)
    print(json.dumps({k: v for k, v in info.items() if k != 'units'}, indent=1))


if __name__ == '__main__':
    main()
