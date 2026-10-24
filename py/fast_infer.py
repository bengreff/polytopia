"""Cheaper inference for self-play: the same network and the same numbers, with less overhead per call.

Self-play asks for small batches (a few to a few dozen positions) from up to ten networks, so the
time per call is mostly Python and kernel-launch overhead, not GPU arithmetic. Net.forward also
synchronizes with the GPU twice (selecting the real legal rows with nonzero(), and the result copy).
FastPolicy:
- scores every padded legal row densely (the same per-row computation as Net.forward, no nonzero),
  then masks the padding;
- with graphs=True, captures the whole call as a CUDA graph per (batch, legal rows) bucket, padding
  inputs up to the bucket, and replays it: one launch instead of hundreds;
- with bf16=True (as self-play's training games and py/train_rl.py use), under bfloat16 autocast.
The outputs of a graphed call are views of the graph's buffers: copy them before the next call.

    python fast_infer.py <checkpoint.pt> <games dir>      benchmark and check against Net.forward
"""
import argparse
import os
import time

import numpy as np
import torch

import model as M

B_BUCKETS = (4, 8, 16, 32, 64, 128)  # larger batches run without a graph
N_BUCKETS = (64, 128, 256, 512, 1024)


def dense_forward(net, tiles, glob, techs, legal, mask):
    """Net.forward with every padded legal row scored (no nonzero): policy logits [B, N] (padding at
    -inf) and value logits [B]. The ownership head is skipped."""
    h = net.encode(tiles, glob, techs)
    hg, ht = h[:, 0], h[:, 1:]
    B, N = mask.shape
    L = legal.long()
    ty = L[..., 0].clamp(0, M.N_TYPES - 1)
    e = net.cmd_emb(ty * M.N_ARGS + L[..., 3].clamp(0, M.N_ARGS - 1)) + net.cmd_type(ty)
    pa = torch.cat([net.cmd_a(ht), net.cmd_a(net.no_tile[0]).expand(B, 1, -1)], 1)
    pb = torch.cat([net.cmd_b(ht), net.cmd_b(net.no_tile[1]).expand(B, 1, -1)], 1)
    none = net.tiles
    a = torch.where(L[..., 1] >= 0, L[..., 1], none)
    b = torch.where(L[..., 2] >= 0, L[..., 2], none)
    d = pa.shape[-1]
    z = e + pa.gather(1, a[..., None].expand(B, N, d)) + pb.gather(1, b[..., None].expand(B, N, d)) + net.cmd_g(hg)[:, None]
    scores = net.cmd_out(z).squeeze(-1).float()
    return scores.masked_fill(~mask, float('-inf')), net.value(hg).squeeze(-1).float()


_POOL = []


def shared_pool():
    """One CUDA graph memory pool for every FastPolicy in the process: their graphs only ever replay
    one at a time (one inference thread), and each call's outputs are read before the next."""
    if not _POOL:
        _POOL.append(torch.cuda.graph_pool_handle())
    return _POOL[0]


def bucket(n, sizes):
    for s in sizes:
        if n <= s:
            return s
    return n


class FastPolicy:
    """Callable like Net for inference: (tiles, globals, techs, legal, mask) as numpy arrays (eval_vs_bot.
    batch_inputs) -> (logits [B, N] float32 tensor with padding at -inf, value logits [B]) on the device."""

    def __init__(self, net, device, graphs=True, bf16=True):
        self.net, self.device, self.graphs = net.eval(), device, graphs and device == 'cuda'
        self.bf16 = bf16 and device == 'cuda'
        self.cache = {}  # (B, N) -> (static inputs, static outputs, graph)
        self.pool = shared_pool() if self.graphs else None

    @torch.no_grad()
    def __call__(self, inputs):
        n_real, n_rows = inputs['mask'].shape
        if not self.graphs or n_real > B_BUCKETS[-1] or n_rows > N_BUCKETS[-1]:
            t = {k: torch.from_numpy(v).to(self.device, non_blocking=True) for k, v in inputs.items()}
            return self.forward(t)
        B, N = bucket(n_real, B_BUCKETS), bucket(n_rows, N_BUCKETS)
        if (B, N) not in self.cache:
            self.cache[(B, N)] = self.capture(B, N, inputs)
        static, out, graph = self.cache[(B, N)]
        for k, v in inputs.items():
            dst = static[k]
            src = torch.from_numpy(v)
            if k in ('legal', 'mask'):
                dst.zero_()
                dst[:n_real, :n_rows].copy_(src, non_blocking=True)
            else:
                dst[n_real:].zero_()
                dst[:n_real].copy_(src, non_blocking=True)
        graph.replay()
        return out[0][:n_real, :n_rows], out[1][:n_real]

    def capture(self, B, N, example):
        static = {}
        for k, v in example.items():
            shape = (B, N) + v.shape[2:] if k in ('legal', 'mask') else (B,) + v.shape[1:]
            static[k] = torch.zeros(shape, dtype=torch.from_numpy(v[:1]).dtype, device=self.device)
        s = torch.cuda.Stream()
        s.wait_stream(torch.cuda.current_stream())
        with torch.cuda.stream(s):
            for _ in range(2):  # warm up kernels and the allocator outside the graph
                self.forward(static)
        torch.cuda.current_stream().wait_stream(s)
        graph = torch.cuda.CUDAGraph()
        with torch.cuda.graph(graph, pool=self.pool):
            out = self.forward(static)
        return static, out, graph

    def forward(self, t):
        with torch.autocast('cuda', dtype=torch.bfloat16, enabled=self.bf16):
            return dense_forward(self.net, t['tiles'], t['globals'], t['techs'], t['legal'], t['mask'])


def main():
    import eval_vs_bot
    import train_bc
    ap = argparse.ArgumentParser()
    ap.add_argument('checkpoint')
    ap.add_argument('games')
    ap.add_argument('--device', default='cuda')
    args = ap.parse_args()
    torch.backends.cuda.matmul.allow_tf32 = True
    torch.backends.cudnn.allow_tf32 = True
    net, _ = eval_vs_bot.load_policy(args.checkpoint, args.device)
    files = sorted(os.path.join(args.games, f) for f in os.listdir(args.games) if f.endswith('.npz'))[:20]
    games = [train_bc.load_game(f) for f in files]
    picks = [(g, d) for g in range(len(games)) for d in range(len(games[g]['label']))]
    rng = np.random.default_rng(0)
    fast = FastPolicy(net, args.device, graphs=False, bf16=False)
    graphed32 = FastPolicy(net, args.device, graphs=True, bf16=False)
    graphed = FastPolicy(net, args.device, graphs=True, bf16=True)

    def batch(n):
        chunk = [picks[i] for i in rng.choice(len(picks), n, replace=False)]
        b = train_bc.collate(games, chunk)
        return dict(tiles=b['tiles'], globals=b['globals'], techs=b['techs'], legal=b['legal'], mask=b['mask'])

    def current(inp):  # selfplay.Inference.run's computation before this change
        t = {k: torch.from_numpy(v).to(args.device, non_blocking=True) for k, v in inp.items()}
        with torch.autocast('cuda', dtype=torch.bfloat16):
            logits, v, _ = net(t['tiles'], t['globals'], t['techs'], t['legal'], t['mask'])
        return logits.float().masked_fill(~t['mask'], float('-inf')), v.float()

    def exact(inp):
        t = {k: torch.from_numpy(v).to(args.device) for k, v in inp.items()}
        logits, v, _ = net(t['tiles'], t['globals'], t['techs'], t['legal'], t['mask'])
        return logits.float().masked_fill(~t['mask'], float('-inf')), v.float()

    with torch.no_grad():
        for n in (2, 8, 32, 64):
            batches = [batch(n) for _ in range(30)]
            ref = [exact(x) for x in batches]
            cur = [current(x) for x in batches]
            for name, fn in (('current (bf16 autocast)', current), ('fp32, dense rows', fast), ('fp32, dense, CUDA graph', graphed32),
                             ('bf16, dense, CUDA graph', graphed)):
                outs = [tuple(o.clone() for o in fn(x)) for x in batches]  # warm-up and check (graph outputs are reused buffers)
                diff = max(float((o[0] - r[0]).nan_to_num(0, 0, 0).abs().max()) for o, r in zip(outs, ref))
                agree = np.mean([float((o[0].argmax(1) == r[0].argmax(1)).float().mean()) for o, r in zip(outs, ref)])
                torch.cuda.synchronize()
                t0 = time.perf_counter()
                for x in batches * 3:
                    lo, v = fn(x)
                    lo.argmax(1).cpu(), v.cpu()
                ms = 1000 * (time.perf_counter() - t0) / (len(batches) * 3)
                agree_cur = np.mean([float((o[0].argmax(1) == c[0].argmax(1)).float().mean()) for o, c in zip(outs, cur)])
                vdiff = max(float((o[1] - r[1]).abs().max()) for o, r in zip(outs, ref))
                print(f'batch {n:3d}  {name:26s} {ms:6.2f} ms/call  {1000 * n / ms:7.0f} positions/s   '
                      f'vs fp32: max |logit diff| {diff:.3f}, value {vdiff:.3f}, same argmax {agree:.1%}; '
                      f'same argmax as current {agree_cur:.1%}', flush=True)


if __name__ == '__main__':
    main()
