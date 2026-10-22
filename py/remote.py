"""Client for py/infer_server.py: runs the policy on the PC's GPU through an ssh pipe.

    net = remote.RemoteNet('runs/r3_m1/step090000.pt')   # uploads the checkpoint
    logits, value = net.infer(inputs)                     # eval_vs_bot.batch_inputs() arrays

logits come back as float32 [B, N] with -inf on padding, as the local forward pass gives them.
"""
import os
import socket
import struct
import subprocess
import zlib

import numpy as np
import torch

HOST = os.environ.get('POLY_INFER_HOST', 'backhouse')
SERVER = os.environ.get('POLY_INFER_CMD', 'wsl -d Ubuntu-22.04 -- bash -c "cd /home/greff/poly/py_r4 && '
                        'exec /home/greff/polyvenv/bin/python -u infer_server.py 2>>/home/greff/infer_server.err"')


PORT = int(os.environ.get('POLY_INFER_PORT', '0'))  # set: connect to localhost:PORT (an ssh -L tunnel)


class RemoteNet:
    def __init__(self, checkpoint):
        if PORT:
            self.sock = socket.create_connection(('127.0.0.1', PORT))
            self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            f = self.sock.makefile('rwb', buffering=0)
            self.proc = type('P', (), dict(stdin=f, stdout=f, wait=lambda *a, **k: None, kill=lambda *a: None))()
        else:
            self.proc = subprocess.Popen(['ssh', '-o', 'ConnectTimeout=20', HOST, SERVER], stdin=subprocess.PIPE,
                                         stdout=subprocess.PIPE, bufsize=0)
        data = open(checkpoint, 'rb').read()
        self._send(struct.pack('<II', 1, len(data)) + data)
        assert struct.unpack('<I', self._read(4))[0] == 0
        self.step = torch.load(checkpoint, map_location='cpu', weights_only=False).get('step')

    def _send(self, b):
        self.proc.stdin.write(b)
        self.proc.stdin.flush()

    def _read(self, n):
        buf = b''
        while len(buf) < n:
            chunk = self.proc.stdout.read(n - len(buf))
            if not chunk:
                raise EOFError('inference server closed the pipe (see /home/greff/infer_server.err on the PC)')
            buf += chunk
        return buf

    def infer(self, inputs):
        tiles, glob, techs = inputs['tiles'], inputs['globals'], inputs['techs']
        legal, mask = inputs['legal'], inputs['mask']
        B, N = legal.shape[:2]
        raw = b''.join([tiles.astype('<i2').tobytes(), glob.astype('<i4').tobytes(), techs.astype(np.uint8).tobytes(),
                        legal.astype('<i2').tobytes(), mask.astype(np.uint8).tobytes()])
        z = zlib.compress(raw, 1)
        self._send(struct.pack('<IIII', 2, B, N, len(z)) + z)
        (n,) = struct.unpack('<I', self._read(4))
        body = np.frombuffer(self._read(n), '<f4')
        k = int(mask.sum())
        logits = np.full((B, N), -np.inf, np.float32)
        logits[mask] = body[:k]
        return logits, body[k:k + B].copy()

    def close(self):
        try:
            self.proc.stdin.close()
            self.proc.wait(timeout=10)
        except Exception:
            self.proc.kill()
