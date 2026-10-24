"""Plays a trained network as the browser UI's opponent (polyhost serve, difficulty -1 = "Our network").

polyhost sends, for each decision of the network's seat: u16 width, u16 height, u32 legal rows, then
the observation (tiles int16 [h*w*32], globals int32 [20], techs uint8 [64]) and the legal rows
(int16 [n*4]), exactly as the bot interface gives them to Python. The reply is the chosen row, i32.
The network sees only that observation: the same fog-limited view as in evaluation.

It also serves what the network is doing, for the web app's network panel (JSON, any origin):
  GET http://127.0.0.1:<stats_port>/info    the checkpoint: run, generation, file hash, parameters,
                                            its evaluations and learning step (from the run's files)
  GET http://127.0.0.1:<stats_port>/stats   the last decision (win estimate, top moves, predicted
                                            owners, attention, per-layer activations) and this game's
                                            history
The trunk is run layer by layer for this (the same computation as Net.encode; checked against it on
the first decision).

    .venv/bin/python py/agent_server.py runs/sp/s3/gen024.pt [--port 8131] [--stats_port 8132] [--temperature 0]
    POLY_AGENT_PORT=8131 POLY_AGENT_STATS=8132 build/polyhost-mac serve 8123      # then http://localhost:8123
"""
import argparse
import hashlib
import http.server
import json
import math
import os
import re
import socket
import struct
import threading
import time

import numpy as np
import torch
import torch.nn.functional as F

import eval_vs_bot
import model as M
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


def checkpoint_info(path, net, device):
    """What the panel shows about the checkpoint: file, run and generation, architecture and
    parameters, and (for a self-play generation) its evaluations and learning step."""
    ck = torch.load(path, map_location='cpu')
    sha1 = hashlib.sha1(open(path, 'rb').read()).hexdigest()
    run_dir = os.path.dirname(os.path.abspath(path))
    m = re.match(r'gen(\d+)\.pt$', os.path.basename(path))
    gen = int(m.group(1)) if m else None
    info = dict(file=os.path.basename(path), path=os.path.relpath(path), run=os.path.basename(run_dir), generation=gen,
                sha1=sha1[:12], modified=time.strftime('%Y-%m-%d %H:%M', time.localtime(os.path.getmtime(path))),
                step=ck.get('step'), rl_from=os.path.basename(ck['rl_from']) if ck.get('rl_from') else None,
                config=dict(dict(d=256, layers=6, heads=8), **ck.get('config', {})), device=device)
    groups = {}
    for name, p in net.named_parameters():
        g = name.split('.')[0]
        if g == 'trunk':
            g = f'trunk layer {int(name.split(".")[2]) + 1}'
        e = groups.setdefault(g, [0, 0.0])
        e[0] += p.numel()
        e[1] += float((p.detach().float() ** 2).sum())
    info['parameters'] = sum(n for n, _ in groups.values())
    info['parameter_groups'] = [dict(name=g, count=n, rms=round(math.sqrt(s / n), 4)) for g, (n, s) in groups.items()]
    if gen is not None:
        summary = os.path.join(run_dir, 'summary.tsv')
        evals = []
        if os.path.exists(summary):
            for line in open(summary):
                c = line.rstrip('\n').split('\t')
                if c[0] != 'gen' and len(c) >= 11 and int(c[0]) == gen:
                    evals.append(dict(vs=c[1], games=int(c[2]), wins=int(c[3]), rate=float(c[4]),
                                      ci=[float(c[5]), float(c[6])], mean_turns=float(c[10])))
        info['evaluations'] = evals
        # the loop's log: this generation's opponents, its games and its learning step, and the
        # settings in force then (the run's defaults, changed by the override lines before it)
        settings = {}
        cfg = os.path.join(run_dir, 'config.json')
        if os.path.exists(cfg):
            c = json.load(open(cfg))
            settings = {k: c[k] for k in ('games', 'self_share', 'temperature', 'lr', 'epochs', 'target_kl', 'baseline',
                                          'bc_weight', 'bot_share', 'fixed', 'recent', 'turn_cost') if k in c}
        settings.setdefault('turn_cost', 0.0)
        log = os.path.join(run_dir, 'loop.log')
        if os.path.exists(log):
            for line in open(log):
                o = re.search(r'override: (\w+) = (.*) \(was ', line)
                if o and 'learning' not in info:
                    settings[o.group(1)] = o.group(2)
                if f'gen {gen:03d}: playing' in line and ' pool ' in line:
                    info['pool'] = line.split(' pool ', 1)[1].strip()
                if f'gen {gen:03d}: games done' in line:
                    info['games'] = line.split(': ', 2)[-1].strip()[:400]
                if f'gen {gen:03d}: learned in' in line and '{' in line:
                    try:
                        info['learning'] = json.loads(line[line.index('{'):])
                        info['trained'] = line[:16]
                    except ValueError:
                        pass
        info['run_settings'] = settings
    return info


@torch.no_grad()
def inspect(net, device, obs, st, check=False):
    """One decision, with the trunk run layer by layer: logits, value, ownership and per-layer
    statistics. With check, also compares the logits with the plain forward pass."""
    inputs = eval_vs_bot.batch_inputs([obs], [st])
    t = {k: torch.from_numpy(v).to(device) for k, v in inputs.items()}
    x = net.embed(t['tiles'], t['globals'], t['techs'])
    layers, glob_attn = [], torch.zeros(x.shape[1] - 1, device=x.device)
    for L in net.trunk.layers:  # pre-norm: x + attn(norm1(x)), then + ffn(norm2(x))
        size_in = x.norm(dim=-1).mean()
        y = L.norm1(x)
        a, w = L.self_attn(y, y, y, need_weights=True, average_attn_weights=False)  # w [1, heads, 197, 197]
        x = x + a
        size_mid = x.norm(dim=-1).mean()
        pre = L.linear1(L.norm2(x))
        f = L.linear2(L.activation(pre))
        x = x + f
        ent = -(w * w.clamp_min(1e-12).log()).sum(-1).mean()
        glob_attn += w[0, :, 0, 1:].mean(0)
        layers.append(dict(resid=round(float(x.norm(dim=-1).mean()), 2),
                           attn=round(float(a.norm(dim=-1).mean() / size_in), 3),
                           ffn=round(float(f.norm(dim=-1).mean() / size_mid), 3),
                           active=round(float((pre > 0).float().mean()), 3),
                           focus=round(float(1 - ent / math.log(w.shape[-1])), 3)))
    logits, v, own = net.heads(net.norm(x), t['legal'], t['mask'])
    diff = None
    if check:
        ref, _, _ = net(t['tiles'], t['globals'], t['techs'], t['legal'], t['mask'])
        diff = float((ref - logits).abs().max())
    return (logits[0].float().cpu().numpy(), float(torch.sigmoid(v[0])), F.softmax(own[0], -1).float().cpu().numpy(),
            (glob_attn / glob_attn.max()).cpu().numpy(), layers, diff)


def describe_row(row, width):
    t, a, b, arg = (int(x) for x in row)
    xy = lambda i: [i % width, i // width] if i >= 0 else None
    return dict(cmd=polyenv.COMMAND_TYPES.get(t, f'type{t}'), type=t, at=xy(a), to=xy(b), arg=arg)


class Stats:
    """The last decision and this game's history, shared with the HTTP thread."""

    def __init__(self, info):
        self.lock = threading.Lock()
        self.info = info
        self.last, self.choice, self.turn_log, self.history, self.game, self.turn = None, None, [], [], 0, -1

    def record(self, d):
        with self.lock:
            if d['turn'] < self.turn:  # a new game
                self.history, self.game, self.choice = [], self.game + 1, None
            if d['turn'] != self.turn:
                self.turn_log = []
            self.turn = d['turn']
            d['game'] = self.game
            d['n'] = len(self.history)
            self.history.append({k: d[k] for k in ('n', 'turn', 'value', 'entropy', 'p', 'legal', 'ms')})
            self.turn_log.append(dict(n=d['n'], move=d['chosen'], legal=d['legal'], value=d['value'], entropy=d['entropy']))
            self.last = d
            if d['legal'] > 1:  # a real choice (not a lone End Turn or a forced move)
                self.choice = d

    def json(self, what):
        with self.lock:
            if what == 'info':
                return json.dumps(self.info)
            return json.dumps(dict(game=self.game, last=self.last, choice=self.choice or self.last, turn_log=self.turn_log,
                                   history=self.history))


def serve_stats(stats, port):
    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            what = self.path.strip('/').split('?')[0]
            if what not in ('info', 'stats'):
                self.send_error(404)
                return
            body = stats.json(what).encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Access-Control-Allow-Origin', '*')
            self.send_header('Cache-Control', 'no-store')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *a):
            pass

    srv = http.server.ThreadingHTTPServer(('127.0.0.1', port), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('checkpoint')
    ap.add_argument('--port', type=int, default=8131)
    ap.add_argument('--stats_port', type=int, default=8132, help='the network panel\'s JSON (0: off)')
    ap.add_argument('--temperature', type=float, default=0.0)
    ap.add_argument('--device', default='mps' if torch.backends.mps.is_available() else 'cpu')
    args = ap.parse_args()
    net, step = eval_vs_bot.load_policy(args.checkpoint, args.device)
    info = checkpoint_info(args.checkpoint, net, args.device)
    info['temperature'] = args.temperature
    stats = Stats(info)
    if args.stats_port:
        serve_stats(stats, args.stats_port)
    rng = np.random.default_rng()
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('127.0.0.1', args.port))
    srv.listen()
    print(f'agent: {info["path"]} ({info["parameters"]:,} parameters, sha1 {info["sha1"]}) on {args.device}, '
          f'listening on 127.0.0.1:{args.port}, stats on {args.stats_port}', flush=True)
    checked = False
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
                    t0 = time.perf_counter()
                    obs = polyenv.Obs(tiles, glob, techs)
                    st = polyenv.Status(current=0, ended=False, winner=0, turn=0, legal=legal)
                    logits, value, own, attn, layers, diff = inspect(net, args.device, obs, st, check=not checked)
                    if diff is not None:
                        checked = True
                        stats.info['selfcheck_max_logit_diff'] = diff
                        print(f'agent: layer-by-layer logits match the forward pass within {diff:.2e}', flush=True)
                    z = logits[:n] - logits[:n].max()
                    p = np.exp(z) / np.exp(z).sum()
                    if args.temperature <= 0:
                        choice = int(np.argmax(logits[:n]))
                    else:
                        q = np.exp(z / args.temperature)
                        choice = int(rng.choice(n, p=q / q.sum()))
                    top = np.argsort(-p)[:6]
                    entropy = max(0.0, float(-(p * np.log(np.clip(p, 1e-12, 1))).sum()))
                    share = own.mean(0)
                    stats.record(dict(
                        turn=int(glob[M.G['turn']]), legal=int(n), ms=round(1000 * (time.perf_counter() - t0), 1),
                        value=round(value, 4), entropy=round(entropy, 3), p=round(float(p[choice]), 4),
                        chosen=dict(describe_row(legal[choice], w), p=round(float(p[choice]), 4)),
                        top=[dict(describe_row(legal[i], w), p=round(float(p[i]), 4)) for i in top],
                        own=dict(mine=round(float(share[1]), 3), theirs=round(float(share[2]), 3), none=round(float(share[0]), 3)),
                        ownMap=[round(float(x), 3) for x in own[:, 1] - own[:, 2]],
                        attnMap=[round(float(x), 3) for x in attn], layers=layers,
                        stars=int(glob[M.G['stars']]), cities=int(glob[M.G['cities']])))
                conn.sendall(struct.pack('<i', choice))
                decisions += 1
        except (EOFError, ConnectionResetError):
            print(f'agent: connection closed after {decisions} decisions', flush=True)
        conn.close()


if __name__ == '__main__':
    main()
