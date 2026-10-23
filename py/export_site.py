"""Builds the public replay site (GitHub Pages): the web app's renderer in static mode
(polyhost/web/static.js), replays of chosen games and the self-play results. Nothing of the game's
code or data files is published: replays are exported board states, and data/gamedata.json holds
only names, tribe colours, four unit stats and the tech tree's links.

Games are replayed through polyhost-mac's own server on a private port, one state per move; the site
stores the first state and what each move changed.

    POLYHOST=mac .venv/bin/python py/export_site.py ~/polytopia-site --summary runs/sp/s2/summary.tsv \
        --sets runs/games/sp_s2_crazy_g000 runs/games/sp_s2_prev_g001 ... [--featured sp_s2_crazy_g000/10000094_s1]
"""
import argparse
import json
import os
import shutil
import subprocess
import time
import urllib.parse
import urllib.request

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TABLES = ['terrainData', 'resourceData', 'improvementData', 'unitData', 'techData', 'tribeData']
DISCLAIMER = ('An unofficial research project, not affiliated with or endorsed by Midjiwan AB. The Battle of Polytopia is '
              'a trademark of Midjiwan AB. The games were played by the game\'s own rules code on the owner\'s computer; '
              'this site shows only recorded board states, drawn by its own simple renderer.')


def gamedata_extract():
    full = json.load(open(os.path.join(REPO, 'game', 'dump', 'gamedata', 'GameLogicData28.txt')))
    out = {}
    for t in TABLES:
        out[t] = {}
        for name, v in full[t].items():
            if not isinstance(v, dict) or 'idx' not in v:
                continue
            e = {'idx': v['idx']}
            if t == 'tribeData' and 'color' in v:
                e['color'] = v['color']
            if t == 'unitData':
                e.update({k: v[k] for k in ('attack', 'defence', 'movement', 'range') if k in v})
            if t == 'techData':
                e.update({k: v[k] for k in ('techUnlocks', 'cost') if k in v})
            out[t][name] = e
    return out


class Server:
    """polyhost-mac serve on a private port, for replaying recorded games."""

    def __init__(self, port, games_dir):
        self.port = port
        self.proc = subprocess.Popen([os.path.join(REPO, 'build', 'polyhost-mac'), 'serve', str(port)], cwd=REPO,
                                     env=dict(os.environ, POLY_GAMES=games_dir),
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):
            try:
                self.get('/games')
                return
            except OSError:
                time.sleep(0.2)
        raise RuntimeError('polyhost serve did not start')

    def get(self, path, method='GET'):
        req = urllib.request.Request(f'http://127.0.0.1:{self.port}{path}', method=method)
        with urllib.request.urlopen(req, timeout=60) as r:
            return json.loads(r.read())

    def close(self):
        self.proc.terminate()
        self.proc.wait(timeout=10)


def export_game(srv, path):
    """One recorded game: its first state, then per move the tiles that changed, players, turn and the
    move's log line."""
    s = srv.get(f'/watch?path={urllib.parse.quote(path)}', 'POST')
    w = s['watch']
    keep = lambda st: {k: v for k, v in st.items() if k not in ('watch', 'legal', 'human', 'reveal')}
    first = keep(s)
    prev = first
    deltas = []
    while s['watch']['at'] < w['total']:
        s = srv.get('/wstep?d=1', 'POST')
        cur = keep(s)
        d = {}
        tiles = [[i, t] for i, (t, p) in enumerate(zip(cur['tiles'], prev['tiles'])) if t != p]
        if tiles:
            d['t'] = tiles
        if cur['players'] != prev['players']:
            d['p'] = cur['players']
        for k, short in (('turn', 'turn'), ('currentPlayer', 'cp'), ('state', 's')):
            if cur[k] != prev[k]:
                d[short] = cur[k]
        if cur.get('log'):
            d['log'] = cur['log'][-1:]
        deltas.append(d)
        prev = cur
    return dict(path=path, base=w['base'], turnStarts=w['turnStarts'], first=first, deltas=deltas)


def read_summary(path):
    gens = {}
    for line in open(path):
        if line.startswith('gen\t') or not line.strip():
            continue
        c = line.rstrip('\n').split('\t')
        g, vs = int(c[0]), c[1]
        key = 'crazy' if vs == 'crazy' else 'gen0' if vs == 'gen000' else 'prev'
        gens.setdefault(g, {'gen': g})[key] = dict(games=int(c[2]), wins=int(c[3]), rate=float(c[4]),
                                                   ci=[float(c[5]), float(c[6])])
    return [gens[g] for g in sorted(gens)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('site')
    ap.add_argument('--summary', required=True)
    ap.add_argument('--sets', nargs='+', required=True, help='runs/games/<tag> directories to pick games from')
    ap.add_argument('--featured', action='append', default=[], help='game paths to put first')
    ap.add_argument('--per_set', type=int, default=6, help='games per set: most back-and-forth wins, comebacks, losses')
    ap.add_argument('--port', type=int, default=8127)
    ap.add_argument('--model', default='')
    args = ap.parse_args()
    site = os.path.expanduser(args.site)
    os.makedirs(os.path.join(site, 'data', 'replays'), exist_ok=True)

    games, chosen = {}, []
    for d in args.sets:
        lines = {}
        for l in open(os.path.join(d, 'games.jsonl')):
            if l.strip():
                g = json.loads(l)
                lines[g['path']] = g
        wins = sorted((g for g in lines.values() if g.get('result') == 'win' and 'swing' in g), key=lambda g: -g['swing'])
        comebacks = sorted((g for g in lines.values() if g.get('result') == 'win' and 'comeback' in g), key=lambda g: -g['comeback'])
        losses = [g for g in lines.values() if g.get('result') == 'loss']
        n = args.per_set
        pick = wins[:n // 2] + comebacks[:n // 3] + losses[:max(1, n - n // 2 - n // 3)]
        for g in pick:
            if g['path'] not in games:
                games[g['path']] = g
                chosen.append(g['path'])
        for p in args.featured:
            if p in lines:
                games[p] = lines[p]
    featured = [p for p in args.featured if p in games]
    sent = os.path.join(REPO, 'runs', 'games', 'sent.jsonl')
    notes = {g['path']: g.get('note', '') for g in map(json.loads, open(sent)) if g} if os.path.exists(sent) else {}
    labels = {'bot:3': 'Crazy (the game\'s hardest bot)', 'bot, difficulty 3': 'Crazy (the game\'s hardest bot)'}
    for g in games.values():
        g['players'] = {k: labels.get(v, v) for k, v in g.get('players', {}).items()}
    order = featured + [p for p in chosen if p not in featured]

    srv = Server(args.port, os.path.join(REPO, 'runs', 'games'))
    try:
        keep_files = set()
        for p in order:
            f = os.path.join(site, 'data', 'replays', p.replace('/', '__') + '.json')
            keep_files.add(os.path.basename(f))
            if not os.path.exists(f):
                rep = export_game(srv, p)
                with open(f + '.tmp', 'w') as out:
                    json.dump(rep, out, separators=(',', ':'))
                os.replace(f + '.tmp', f)
                print(f'{p}: {len(rep["deltas"])} moves, {os.path.getsize(f) / 1e6:.2f} MB', flush=True)
        for f in os.listdir(os.path.join(site, 'data', 'replays')):
            if f not in keep_files:
                os.remove(os.path.join(site, 'data', 'replays', f))
    finally:
        srv.close()

    meta = lambda p: {k: v for k, v in games[p].items() if k != 'note'}
    json.dump({'featured': [dict(meta(p), note=notes.get(p, '')) for p in featured],
               'games': [meta(p) for p in order if p not in featured]},
              open(os.path.join(site, 'data', 'games.json'), 'w'), separators=(',', ':'))
    json.dump(gamedata_extract(), open(os.path.join(site, 'data', 'gamedata.json'), 'w'), separators=(',', ':'))
    gens = read_summary(args.summary)
    current = max((g['gen'] for g in gens if 'crazy' in g), default=0)
    json.dump({
        'title': 'A Polytopia bot that learns by playing itself',
        'summary': ('A neural network that plays The Battle of Polytopia in the World Championship format (1v1, Might, '
                    'Lakes, small map, any of the 12 regular tribes). It first learned from 7,732 human tournament games; '
                    'since then it improves by self-play, one generation at a time. Every generation is tested against the '
                    'game\'s hardest built-in bot, against the previous generation and against generation 0. '
                    'Watch its games below: the whole map is shown. Source code and research log: '
                    '<a href="https://github.com/bengreff/polytopia" style="color:#9ec5ff">github.com/bengreff/polytopia</a>.'),
        'model': args.model or ('5.9M-parameter transformer over the 196 map tiles; it scores every legal move. Generation 0 '
                                'is the network trained on the human games. Self-play: PPO on the outcome, with the network\'s '
                                'own win estimate as baseline; opponents are itself and earlier generations.'),
        'current': current, 'generations': gens, 'featured': bool(featured),
        'updated': time.strftime('%Y-%m-%d %H:%M %Z'), 'disclaimer': DISCLAIMER,
    }, open(os.path.join(site, 'data', 'results.json'), 'w'), indent=1)

    web = os.path.join(REPO, 'polyhost', 'web')
    shutil.copy(os.path.join(web, 'app.js'), os.path.join(site, 'app.js'))
    shutil.copy(os.path.join(web, 'static.js'), os.path.join(site, 'static.js'))
    html = open(os.path.join(web, 'index.html')).read()
    html = html.replace('<title>Polytopia, real rules</title>', '<title>Polytopia self-play bot</title>')
    html = html.replace('<script src="app.js"></script>', '<script src="static.js"></script>\n<script src="app.js"></script>')
    html = html.replace('</head>', '''<style>
  #rightbar, #logbtn, #wview, #menuview { display: none !important; }
  #siteview .panel { max-width: 760px; max-height: 88vh; overflow: auto; }
  #siteview table.res { border-collapse: collapse; width: 100%; font-size: 14px; margin: 8px 0; }
  #siteview table.res td, #siteview table.res th { padding: 4px 8px; border-bottom: 1px solid #2a3040; text-align: left; }
  #siteview .btnrow { display: flex; gap: 10px; margin: 10px 0; flex-wrap: wrap; }
</style>
</head>''', 1)
    html = html.replace('<body>', '<body>\n<div class="overlay" id="siteview"><div class="panel"></div></div>', 1)
    open(os.path.join(site, 'index.html'), 'w').write(html)
    open(os.path.join(site, '.nojekyll'), 'w').close()
    print(f'site: {len(order)} games, generations {[g["gen"] for g in gens]}, current {current}', flush=True)


if __name__ == '__main__':
    main()
