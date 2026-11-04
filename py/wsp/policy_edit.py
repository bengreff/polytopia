"""WSP-1 Part B: a closed-form edit of how the policy rates one kind of command (research log WSP-1).

The policy scores each legal row with cmd_out(z_row), z_row = cmd_emb[type, arg] + cmd_type[type]
+ the source and target tiles' projections + cmd_g(hg). cmd_type[T] reaches rows of type T only
(e.g. 8 Research, 15 End Turn). The edit moves cmd_type[T] along v, the mean gradient of those rows'
logits with respect to it over the tuning positions, by the amount alpha that changes the type's
log-sum-exp by --delta on average there (a secant search with real forward passes; delta > 0 raises it).
Every other row's score is unchanged.

Writes the checkpoint and <out>.json: source hash, type, delta, alpha, |v|, the realized shift
(mean, sd, quantiles), the tensor touched.

    python py/wsp/policy_edit.py gen083.pt --data DIR:N[:val] --type 8 --delta 0.38 --out edited.pt
"""
import argparse
import hashlib
import json
import os
import sys

import numpy as np
import torch

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import eval_vs_bot  # noqa: E402
import train_bc  # noqa: E402



def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()[:16]


def batches(data, share, batch, seed=1):
    parts = data.split(':')
    d, lim, val = parts[0], int(parts[1]) if len(parts) > 1 and parts[1] else 0, 'val' in parts[2:]
    files = train_bc.list_games(d, val=True) if val else sorted(
        os.path.join(d, f) for f in os.listdir(d) if f.endswith('.npz') and not f.endswith('.tmp.npz'))
    rng = np.random.default_rng(seed)
    used, out = 0, []
    for f in files:
        if lim and used >= lim:
            break
        g = train_bc.load_game(f)
        if g is None:
            continue
        used += 1
        D = len(g['label'])
        pick = np.nonzero(rng.random(D) < share)[0]
        for i in range(0, len(pick), batch):
            b = train_bc.collate([g], [(0, int(x)) for x in pick[i:i + batch]])
            out.append({k: torch.from_numpy(v) for k, v in b.items() if k in ('tiles', 'globals', 'techs', 'legal', 'mask')})
    return out


def et_logits(net, bs, T):
    """Log-sum-exp of the type-T rows minus that of the other rows, per decision with a type-T row."""
    vals = []
    with torch.no_grad():
        for b in bs:
            lg, _, _ = net(b['tiles'], b['globals'], b['techs'], b['legal'], b['mask'])
            is_et = (b['legal'][..., 0] == T) & b['mask']
            has = is_et.any(1)
            et = torch.where(is_et, lg, torch.full_like(lg, -1e9)).logsumexp(1)
            rest = torch.where(is_et | ~b['mask'], torch.full_like(lg, -1e9), lg).logsumexp(1)
            vals.append((et - rest)[has].numpy())
    return np.concatenate(vals)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('checkpoint')
    ap.add_argument('--data', required=True)
    ap.add_argument('--share', type=float, default=0.05)
    ap.add_argument('--type', type=int, default=15, help='command type (8 Research, 15 End Turn, ...)')
    ap.add_argument('--delta', type=float, required=True, help='mean change of the type\'s log-sum-exp (logits; > 0 raises)')
    ap.add_argument('--batch', type=int, default=64)
    ap.add_argument('--threads', type=int, default=4)
    ap.add_argument('--out', required=True)
    args = ap.parse_args()
    torch.set_num_threads(args.threads)
    net, _ = eval_vs_bot.load_policy(args.checkpoint, 'cpu')
    bs = batches(args.data, args.share, args.batch)
    T = args.type
    # v: mean gradient of the type-T rows' logits with respect to cmd_type[T]
    w = net.cmd_type.weight
    grad = torch.zeros_like(w[T])
    n = 0
    for b in bs:
        net.zero_grad()
        lg, _, _ = net(b['tiles'], b['globals'], b['techs'], b['legal'], b['mask'])
        is_et = (b['legal'][..., 0] == T) & b['mask']
        s = lg[is_et].sum()
        s.backward()
        grad += w.grad[T]
        n += int(is_et.sum())
    v = (grad / max(n, 1)).detach()
    vn = v / v.norm()
    base = et_logits(net, bs, T)
    orig = w.data[T].clone()
    sign = 1.0 if args.delta >= 0 else -1.0

    def shift(alpha):
        w.data[T] = orig + sign * alpha * vn
        return et_logits(net, bs, T) - base

    # secant search on alpha (the change is close to linear in alpha): a few forward passes
    a0, f0 = 0.0, 0.0
    a1 = 1.0
    f1 = float(shift(a1).mean()) * sign
    for _ in range(8):
        if abs(f1 - abs(args.delta)) < 0.005 or f1 == f0:
            break
        a2 = a1 + (abs(args.delta) - f1) * (a1 - a0) / (f1 - f0)
        a0, f0, a1 = a1, f1, max(a2, 1e-6)
        f1 = float(shift(a1).mean()) * sign
    alpha = a1
    dlt = shift(alpha)
    ck = torch.load(args.checkpoint, map_location='cpu', weights_only=False)
    sd = dict(ck['model'])
    sd['cmd_type.weight'] = w.data.clone()
    info = dict(kind='policy_type_shift', type=T, source=args.checkpoint, source_sha=sha(args.checkpoint), data=args.data,
                share=args.share, decisions=int(len(base)), delta=args.delta, alpha=float(sign * alpha),
                grad_norm=float(v.norm()), cmd_type_row_norm=float(orig.norm()),
                realized=dict(mean=float(dlt.mean()), sd=float(dlt.std()), q05=float(np.quantile(dlt, 0.05)),
                              q50=float(np.quantile(dlt, 0.5)), q95=float(np.quantile(dlt, 0.95))),
                weights_touched=f'cmd_type.weight row {T} (256 values; rows of command type {T} only)')
    torch.save(dict(ck, model=sd, wsp=info), args.out)
    info['out_sha'] = sha(args.out)
    json.dump(info, open(args.out + '.json', 'w'), indent=1)
    print(json.dumps(info, indent=1))


if __name__ == '__main__':
    main()
