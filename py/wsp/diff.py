"""Weight-space diff (docs/design/weight-space.md, phase 0): how far each tensor moved between two
checkpoints, and how concentrated each matrix's change is (share of ||dW||^2 in its top 1 and top 8
singular directions). CPU only, 2 threads.

    python py/wsp/diff.py A.pt B.pt
"""
import collections
import sys

import torch

torch.set_num_threads(2)


def sd(p):
    c = torch.load(p, map_location='cpu', weights_only=False)
    for k in ('model', 'state_dict', 'net'):
        if isinstance(c, dict) and k in c and isinstance(c[k], dict):
            return c[k]
    return c


a, b = sd(sys.argv[1]), sd(sys.argv[2])
tot0=sum(v.float().norm()**2 for v in a.values() if v.is_floating_point())
groups=collections.defaultdict(lambda:[0.,0.,0])
rows=[]
for k,v in a.items():
    if not v.is_floating_point(): continue
    d=(b[k].float()-v.float())
    g=k.split('.')[0] if not k.startswith('trunk') else '.'.join(k.split('.')[:3])
    groups[g][0]+=float(d.norm()**2); groups[g][1]+=float(v.float().norm()**2); groups[g][2]+=v.numel()
    if d.dim()==2 and min(d.shape)>=16:
        s=torch.linalg.svdvals(d); e=(s**2)/(s**2).sum()
        rows.append((k,tuple(d.shape),float(d.norm()/v.float().norm()),float(e[:1].sum()),float(e[:8].sum())))
D=sum(x[0] for x in groups.values())
print('params', sum(x[2] for x in groups.values()), 'total |dW|/|W| = %.4f'%((D/float(tot0))**.5))
print('%-22s %9s %8s %8s'%('group','params','rel_chg','share_d2'))
for g,(d2,w2,n) in sorted(groups.items(), key=lambda x:-x[1][0]):
    print('%-22s %9d %8.4f %8.3f'%(g,n,(d2/max(w2,1e-12))**.5,d2/D))
print('\nlargest matrices: rel change, energy in top-1 / top-8 singular directions of dW')
for r in sorted(rows,key=lambda r:-r[2])[:14]: print('%-40s %-12s %.4f %.3f %.3f'%r)
