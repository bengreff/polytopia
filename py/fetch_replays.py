"""Downloads recorded games from the game's server, as the client does when it opens a share link.

POSTs {"GameId": id} to polytopia-prod.net/api/game/get_game_view_model, and to spectate_game if
that fails, with the token of a logged-in game client. The reply holds the game's settings,
participants and two GameState blobs (initial and current, base64): the client's
GameViewModel. Each game is saved as raw/<id>.json, unchanged. Each of --workers threads spaces its
requests --delay seconds apart; all pause for 10 minutes on HTTP 429. Games already saved are
skipped, and failures go to raw/errors.tsv.

    python3 py/fetch_replays.py --token-file <jwtToken> [--ids data/replays/share_ids.tsv]
        [--out data/replays/raw] [--workers 1] [--delay 4] [--limit N] [--newest-first]
"""
import argparse
import concurrent.futures as cf
import json
import os
import random
import threading
import time
import urllib.error
import urllib.request

API = 'https://polytopia-prod.net/api/game/'


def post(endpoint, token, game_id):
    req = urllib.request.Request(
        API + endpoint, data=json.dumps({'GameId': game_id}).encode(), method='POST',
        headers={'Content-Type': 'application/json', 'Accept': 'application/json', 'Authorization': f'Bearer {token}'})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.loads(r.read())


def fetch(token, game_id):
    """The server's game view model for game_id, or raises with the server's error."""
    errors = []
    for endpoint in ('get_game_view_model', 'spectate_game'):
        resp = post(endpoint, token, game_id)
        if resp.get('success') and resp.get('data'):
            return endpoint, resp['data']
        errors.append(f"{endpoint}: {resp.get('errorCode')} {resp.get('errorMessage')}")
        if resp.get('errorCode') == 1005:  # "Can't load game": spectating it fails the same way
            break
    raise RuntimeError('; '.join(errors))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--token-file', required=True)
    ap.add_argument('--ids', default='data/replays/share_ids.tsv')
    ap.add_argument('--out', default='data/replays/raw')
    ap.add_argument('--workers', type=int, default=1, help='requests in flight at once')
    ap.add_argument('--delay', type=float, default=4.0, help='mean seconds between one worker\'s requests')
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--newest-first', action='store_true', help='fetch in order of the list\'s date column, newest first')
    args = ap.parse_args()
    token = open(args.token_file).read().strip()
    os.makedirs(args.out, exist_ok=True)
    lines = open(args.ids).read().splitlines()
    rows = [l.split('\t') for l in lines[1:] if l.strip()]
    if args.newest_first:  # by the list's date column; recent games load, many older ones no longer do
        col = next(i for i, h in enumerate(lines[0].split('\t')) if h in ('started', 'publish_date'))
        rows.sort(key=lambda r: r[col], reverse=True)
    ids = list(dict.fromkeys(r[0] for r in rows))
    failed = set()
    err_path = os.path.join(args.out, 'errors.tsv')
    if os.path.exists(err_path):
        failed = {l.split('\t')[0] for l in open(err_path)}
    todo = [g for g in ids if g not in failed and not os.path.exists(os.path.join(args.out, f'{g}.json'))]
    if args.limit:
        todo = todo[:args.limit]
    print(f'{len(ids)} ids, {len(todo)} to fetch', flush=True)
    errors = open(err_path, 'a')
    lock = threading.Lock()
    paused_until = [0.0]
    done = [0, 0]  # fetched, tried

    def one(g):
        while True:
            time.sleep(max(0.0, paused_until[0] - time.time()) + args.delay * (0.5 + random.random()))
            try:
                endpoint, data = fetch(token, g)
                break
            except urllib.error.HTTPError as e:
                if e.code in (401, 403):
                    print(f'HTTP {e.code}: the token was refused (expired?). Log in to the game again.', flush=True)
                    os._exit(2)
                if e.code == 429:
                    with lock:
                        if paused_until[0] < time.time():
                            print('rate-limited; all workers wait 10 minutes', flush=True)
                            paused_until[0] = time.time() + 600
                    continue
                return g, f'HTTP {e.code}'
            except Exception as e:
                return g, str(e)
        data['_endpoint'] = endpoint
        path = os.path.join(args.out, f'{g}.json')
        with open(path + '.tmp', 'w') as f:
            json.dump(data, f)
        os.replace(path + '.tmp', path)
        return g, None

    with cf.ThreadPoolExecutor(args.workers) as ex:
        for g, err in ex.map(one, todo):
            with lock:
                done[1] += 1
                if err:
                    errors.write(f'{g}\t{err}\n')
                    errors.flush()
                else:
                    done[0] += 1
                    if done[0] % 50 == 0:
                        print(f'{done[0]} fetched ({done[1]}/{len(todo)} tried)', flush=True)
    print(f'done: {done[0]} fetched, {done[1] - done[0]} failed', flush=True)


if __name__ == '__main__':
    main()
