"""Policy inference on the PC's GPU for the Mac's game processes, over a pipe (stdin/stdout).

The Mac starts it through ssh (py/remote.py), so no port has to be opened into WSL. Messages are
little-endian:
- LOAD (op 1): u32 length + the checkpoint file's bytes. Reply: u32 0.
- INFER (op 2): u32 B, u32 N, u32 length + zlib of tiles int16 [B,196,32], globals int32 [B,20],
  techs uint8 [B,64], legal int16 [B,N,4], mask uint8 [B,N], concatenated. Reply: u32 length +
  float32 logits of the masked rows in row order, then float32 value logits [B].
Float32 throughout (no autocast), so results match the Mac's own forward pass to rounding.

    ssh backhouse "wsl -d Ubuntu-22.04 -- /home/greff/polyvenv/bin/python /home/greff/poly/py_r4/infer_server.py"
"""
import io
import socket
import struct
import sys
import threading
import zlib

import numpy as np
import torch

import model as M


def read(f, n):
    buf = b''
    while len(buf) < n:
        chunk = f.read(n - len(buf))
        if not chunk:
            raise EOFError
        buf += chunk
    return buf


@torch.no_grad()
def serve(inp, out):
    dev = 'cuda' if torch.cuda.is_available() else 'cpu'
    torch.backends.cuda.matmul.allow_tf32 = False
    net = None
    while True:
        try:
            (op,) = struct.unpack('<I', read(inp, 4))
        except EOFError:
            return
        if op == 1:
            (n,) = struct.unpack('<I', read(inp, 4))
            ck = torch.load(io.BytesIO(read(inp, n)), map_location='cpu')
            net = M.Net(**ck.get('config', {}))
            net.load_state_dict(ck['model'])
            net = net.to(dev).eval()
            out.write(struct.pack('<I', 0))
        elif op == 2:
            B, N, n = struct.unpack('<III', read(inp, 12))
            raw = zlib.decompress(read(inp, n))
            sizes = [B * 196 * 32 * 2, B * 20 * 4, B * 64, B * N * 4 * 2, B * N]
            parts, p = [], 0
            for s in sizes:
                parts.append(raw[p:p + s])
                p += s
            tiles = torch.from_numpy(np.frombuffer(parts[0], '<i2').reshape(B, 196, 32).copy()).to(dev)
            glob = torch.from_numpy(np.frombuffer(parts[1], '<i4').reshape(B, 20).copy()).to(dev)
            techs = torch.from_numpy(np.frombuffer(parts[2], np.uint8).reshape(B, 64).copy()).to(dev)
            legal = torch.from_numpy(np.frombuffer(parts[3], '<i2').reshape(B, N, 4).copy()).to(dev)
            mask = torch.from_numpy(np.frombuffer(parts[4], np.uint8).reshape(B, N).astype(bool)).to(dev)
            logits, value, _ = net(tiles, glob, techs, legal, mask)
            body = logits[mask].float().cpu().numpy().astype('<f4').tobytes() + value.float().cpu().numpy().astype('<f4').tobytes()
            out.write(struct.pack('<I', len(body)) + body)
        else:
            raise ValueError(f'unknown op {op}')
        out.flush()


def main():
    """No arguments: one client on stdin/stdout. `--port P`: listen on port P (reach it with
    ssh -L P:localhost:P backhouse), one thread and one loaded model per connection."""
    if '--port' not in sys.argv:
        return serve(sys.stdin.buffer, sys.stdout.buffer)
    port = int(sys.argv[sys.argv.index('--port') + 1])
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('0.0.0.0', port))  # WSL's localhost forwarding reaches 0.0.0.0 listeners only
    srv.listen()
    while True:
        conn, _ = srv.accept()
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        f = conn.makefile('rwb', buffering=0)
        threading.Thread(target=lambda: (serve(f, f), conn.close()), daemon=True).start()


if __name__ == '__main__':
    main()
