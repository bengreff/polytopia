"""WSP-1 control (research log WSP-1): ExPO weight extrapolation, theta = theta_B + alpha (theta_B - theta_A),
written as a checkpoint selfplay.py and train_rl.py can load, with a manifest (inputs' hashes, alpha).

    python py/wsp/expo.py s9v/gen000.pt s9v/gen083.pt 0.25 out.pt
"""
import hashlib
import json
import sys

import torch


def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()[:16]


def main():
    a_path, b_path, alpha, out = sys.argv[1], sys.argv[2], float(sys.argv[3]), sys.argv[4]
    a, b = torch.load(a_path, map_location='cpu', weights_only=False), torch.load(b_path, map_location='cpu', weights_only=False)
    if a.get('config', {}) != b.get('config', {}):
        sys.exit('different configurations')
    sd = {}
    for k, vb in b['model'].items():
        va = a['model'][k]
        sd[k] = (vb.float() + alpha * (vb.float() - va.float())).to(vb.dtype) if vb.is_floating_point() else vb
    manifest = dict(kind='expo', base=b_path, base_sha=sha(b_path), anchor=a_path, anchor_sha=sha(a_path), alpha=alpha,
                    rule='theta = base + alpha * (base - anchor), every floating-point tensor')
    torch.save(dict(model=sd, config=b.get('config', {}), step=b.get('step', 0), wsp=manifest), out)
    json.dump(dict(manifest, out=out, out_sha=sha(out)), open(out + '.json', 'w'), indent=1)
    print(json.dumps(manifest))


if __name__ == '__main__':
    main()
