"""Plays a trained network as the browser UI's opponent (polyhost serve, difficulty -1 = "Our network").

polyhost sends, for each decision of the network's seat: u16 width, u16 height, u32 legal rows, then
the observation (tiles int16 [h*w*32], globals int32 [20], techs uint8 [64]) and the legal rows
(int16 [n*4]), exactly as the bot interface gives them to Python. The reply is the chosen row, i32.
The network sees only that observation: the same fog-limited view as in evaluation.

    .venv/bin/python py/agent_server.py runs/r4_human_ft/step006000.pt [--port 8131] [--temperature 0]
    POLY_AGENT_PORT=8131 build/polyhost-mac serve 8123      # then http://localhost:8123
"""
import argparse
import socket
import struct

import numpy as np
import torch

import eval_vs_bot
import polyenv

NT, NG = len(polyenv.TILE_FEATURES), len(polyenv.GLOBAL_FEATURES)


def read(conn, n):
    buf = b''
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            raise EOFError
        buf += chunk
    return buf


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('checkpoint')
    ap.add_argument('--port', type=int, default=8131)
    ap.add_argument('--temperature', type=float, default=0.0)
    ap.add_argument('--device', default='mps' if torch.backends.mps.is_available() else 'cpu')
    args = ap.parse_args()
    net, step = eval_vs_bot.load_policy(args.checkpoint, args.device)
    rng = np.random.default_rng()
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('127.0.0.1', args.port))
    srv.listen()
    print(f'agent: checkpoint step {step} on {args.device}, listening on 127.0.0.1:{args.port}', flush=True)
    while True:
        conn, _ = srv.accept()
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        decisions = 0
        try:
            while True:
                w, h, n = struct.unpack('<HHI', read(conn, 8))
                tiles = np.frombuffer(read(conn, w * h * NT * 2), '<i2').reshape(h, w, NT)
                glob = np.frombuffer(read(conn, NG * 4), '<i4')
                techs = np.frombuffer(read(conn, polyenv.TECH_SLOTS), np.uint8)
                legal = np.frombuffer(read(conn, n * 8), '<i2').reshape(n, 4)
                if (w, h) != (14, 14) or n == 0:  # the network only knows Small maps
                    choice = -1
                else:
                    obs = polyenv.Obs(tiles, glob, techs)
                    st = polyenv.Status(current=0, ended=False, winner=0, turn=0, legal=legal)
                    choice = int(eval_vs_bot.choose(net, args.device, [obs], [st], args.temperature, rng)[0])
                conn.sendall(struct.pack('<i', choice))
                decisions += 1
        except (EOFError, ConnectionResetError):
            print(f'agent: connection closed after {decisions} decisions', flush=True)
        conn.close()


if __name__ == '__main__':
    main()
