"""Weight averaging ("model soup") of policy checkpoints (radical-ideas.md B4): the uniform mean of
every parameter, written as a checkpoint py/selfplay.py and py/train_rl.py can load. All inputs
must have the same configuration (same architecture), e.g. a run's last generations, or several
tracks branched from one parent.

    python soup.py out.pt gen076.pt gen077.pt ... gen083.pt
"""
import sys

import torch


def main():
    out, paths = sys.argv[1], sys.argv[2:]
    if len(paths) < 2:
        sys.exit('give an output and at least two checkpoints')
    cks = [torch.load(p, map_location='cpu') for p in paths]
    cfg = cks[0].get('config', {})
    if any(c.get('config', {}) != cfg for c in cks):
        sys.exit('checkpoints have different configurations')
    keys = cks[0]['model'].keys()
    soup = {}
    for k in keys:
        ts = [c['model'][k] for c in cks]
        soup[k] = (torch.stack([t.float() for t in ts]).mean(0).to(ts[0].dtype) if ts[0].is_floating_point() else ts[0])
    torch.save(dict(model=soup, config=cfg, soup_of=paths, step=cks[-1].get('step', 0)), out)
    print(f'{out}: mean of {len(paths)} checkpoints')


if __name__ == '__main__':
    main()
