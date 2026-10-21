"""Downloads recorded games from the game's server, as the client does when it opens a share link.

POSTs {"GameId": id} to polytopia-prod.net/api/game/get_game_view_model, and to spectate_game if
that fails, with the token of a logged-in game client. The reply holds the game's settings,
participants and two GameState blobs (initial and current, base64): the client's
GameViewModel. Each game is saved as raw/<id>.json, unchanged. Requests are spaced out, games
already saved are skipped, and failures go to raw/errors.tsv.

    python3 py/fetch_replays.py --token-file <jwtToken> [--ids data/replays/share_ids.tsv]
        [--out data/replays/raw] [--delay 4] [--limit N]
"""
import argparse
import json
import os
import random
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
    raise RuntimeError('; '.join(errors))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--token-file', required=True)
    ap.add_argument('--ids', default='data/replays/share_ids.tsv')
    ap.add_argument('--out', default='data/replays/raw')
    ap.add_argument('--delay', type=float, default=4.0, help='mean seconds between requests')
    ap.add_argument('--limit', type=int, default=0)
    args = ap.parse_args()
    token = open(args.token_file).read().strip()
    os.makedirs(args.out, exist_ok=True)
    ids = [l.split('\t')[0] for l in open(args.ids).read().splitlines()[1:] if l.strip()]
    failed = set()
    err_path = os.path.join(args.out, 'errors.tsv')
    if os.path.exists(err_path):
        failed = {l.split('\t')[0] for l in open(err_path)}
    todo = [g for g in ids if g not in failed and not os.path.exists(os.path.join(args.out, f'{g}.json'))]
    if args.limit:
        todo = todo[:args.limit]
    print(f'{len(ids)} ids, {len(todo)} to fetch', flush=True)
    errors = open(err_path, 'a')
    got = 0
    for i, g in enumerate(todo):
        if i:
            time.sleep(args.delay * (0.5 + random.random()))
        try:
            endpoint, data = fetch(token, g)
        except urllib.error.HTTPError as e:
            if e.code in (401, 403):
                raise SystemExit(f'HTTP {e.code}: the token was refused (expired?). Log in to the game again.')
            if e.code == 429:
                print('rate-limited; waiting 10 minutes', flush=True)
                time.sleep(600)
                continue
            errors.write(f'{g}\tHTTP {e.code}\n'); errors.flush()
            continue
        except Exception as e:
            errors.write(f'{g}\t{e}\n'); errors.flush()
            continue
        data['_endpoint'] = endpoint
        path = os.path.join(args.out, f'{g}.json')
        with open(path + '.tmp', 'w') as f:
            json.dump(data, f)
        os.replace(path + '.tmp', path)
        got += 1
        if got % 25 == 0:
            print(f'{got} fetched ({i + 1}/{len(todo)} tried)', flush=True)
    print(f'done: {got} fetched, {len(todo) - got} failed or skipped', flush=True)


if __name__ == '__main__':
    main()
