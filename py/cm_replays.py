"""Collects replay links of Polytopia tournament games from Challengermode, for py/fetch_replays.py.

Every Challengermode match played through the game's integration reports statistics, among them the
game's share link (ReplayLink; General.ReplayLink in older matches), the mode, map size, team count
and both tribes. This walks whole tournament brackets through Challengermode's public GraphQL API
and writes one row per game to data/replays/cm_ids.tsv.

Tournaments come from three places:
  - the game's 50 most recently completed tournaments (all the API lists);
  - data/replays/cm_tournaments.txt, ids found elsewhere (one per line, text after the id ignored);
  - the Challengermode tournament id each downloaded game carries (raw/*.json gameContext).
Only tournaments set to Might, Lakes, Small are walked unless --all. Completed tournaments do not
change, so each is walked once (data/replays/cm_walked.tsv).

Needs a Challengermode API application's refresh key in ~/.config/polytopia/challengermode_refresh_key.

    python3 py/cm_replays.py [--all]
"""
import argparse
import glob
import json
import os
import time
import urllib.error
import urllib.request

API = 'https://publicapi.challengermode.com'
KEY_FILE = os.path.expanduser('~/.config/polytopia/challengermode_refresh_key')
OUT = 'data/replays'
# Challengermode's settings for Polytopia: GameMode 1 Might; MapType 0 Drylands, 1 Lakes,
# 2 Continents, 3 Archipelago, 5 Pangea; MapSize 0 Tiny, 1 Small.
CHAMPIONSHIP = (1, 1, 1)

MATCHES = '''matchSeriesPage(first: 200) { pageInfo { hasNextPage } nodes {
  matches(includeFailed: true) { id state statistics { gameSessionStatistics {
    globalStatistics(first: 20) { nodes { name serializedValue } }
    competitorStatistics(first: 20) { nodes { name serializedValue } } } } } } }'''
TOURNAMENT = '''query($id: UUID!) { tournament(tournamentId: $id) { id name
  schedule { startedAt } settings { gameSessionSettings }
  stages { ... on TournamentEliminationStage { brackets { rounds { %s } } }
           ... on TournamentSwissStage { rounds { %s } }
           ... on TournamentGroupStage { groups { %s } } } } }''' % (MATCHES, MATCHES, MATCHES)
RECENT = '''{ tournamentsForGame(input: {gameSlug: "polytopia", includeUnlisted: true, includeInviteOnly: true,
  tournamentFilter: {completedTournamentSelector: {tournamentsAfter: "2020-01-01T00:00:00Z"}}}) { id } }'''


class Client:
    def __init__(self):
        self.key = open(KEY_FILE).read().strip()
        self.token, self.expires = None, 0

    def post(self, url, body, auth=True):
        headers = {'Content-Type': 'application/json', 'Accept': 'application/json',
                   'User-Agent': 'Mozilla/5.0 (polytopia-research)'}  # Cloudflare refuses urllib's default
        if auth:
            if time.time() > self.expires:  # access keys last 20 minutes
                r = self.post(API + '/mk1/v1/auth/access_keys', {'refreshKey': self.key}, auth=False)
                self.token, self.expires = r['value'], time.time() + 15 * 60
            headers['Authorization'] = f'Bearer {self.token}'
        req = urllib.request.Request(url, data=json.dumps(body).encode(), headers=headers, method='POST')
        return json.loads(urllib.request.urlopen(req, timeout=120).read())

    def query(self, q, variables=None):
        r = self.post(API + '/graphql', {'query': q, 'variables': variables or {}})
        if r.get('errors'):
            raise RuntimeError(r['errors'][0]['message'])
        return r['data']


def games(tour):
    """One dict per match that reported a replay link."""
    pages = []
    for stage in tour['stages'] or []:
        for b in stage.get('brackets') or []:
            pages += [r['matchSeriesPage'] for r in b['rounds'] or []]
        pages += [r['matchSeriesPage'] for r in stage.get('rounds') or []]
        pages += [g['matchSeriesPage'] for g in stage.get('groups') or []]
    for p in pages:
        if p['pageInfo']['hasNextPage']:
            raise RuntimeError('a round has more than 200 match series; add paging')
        for series in p['nodes']:
            for m in series['matches'] or []:
                st = (m['statistics'] or {}).get('gameSessionStatistics') or {}
                g = {n['name'].removeprefix('General.'): n['serializedValue'] for n in (st.get('globalStatistics') or {}).get('nodes') or []}
                tribes = [v for n, v in ((n['name'], n['serializedValue']) for n in (st.get('competitorStatistics') or {}).get('nodes') or [])
                          if n.endswith('TribeType')]
                if g.get('ReplayLink'):
                    yield dict(game_id=g['ReplayLink'].rstrip('/').split('/')[-1], match_id=m['id'], mode=g.get('GameMode'),
                               size=g.get('MapSize'), teams=g.get('NumberOfTeams'), turns=g.get('CurrentTurnNumber'),
                               tribes=','.join(tribes))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--all', action='store_true', help='walk every tournament, not only Might, Lakes, Small')
    args = ap.parse_args()
    cm = Client()
    ids = [t['id'] for t in cm.query(RECENT)['tournamentsForGame']]
    listed = os.path.join(OUT, 'cm_tournaments.txt')
    if os.path.exists(listed):
        ids += [l.split()[0] for l in open(listed) if l.strip() and not l.startswith('#')]
    for f in glob.glob(os.path.join(OUT, 'raw', '*.json')):
        ctx = json.load(open(f)).get('gameContext') or {}
        if ctx.get('externalTournamentId'):
            ids.append(ctx['externalTournamentId'])
    walked_path, ids_path = os.path.join(OUT, 'cm_walked.tsv'), os.path.join(OUT, 'cm_ids.tsv')
    walked = {l.split('\t')[0] for l in open(walked_path)} if os.path.exists(walked_path) else set()
    todo = [i for i in dict.fromkeys(ids) if i not in walked]
    print(f'{len(set(ids))} tournaments known, {len(todo)} not walked yet', flush=True)
    new_ids = not os.path.exists(ids_path)
    out, log = open(ids_path, 'a'), open(walked_path, 'a')
    if new_ids:
        out.write('game_id\ttournament_id\tmatch_id\tmode\tsize\tteams\tturns\ttribes\tstarted\ttournament\n')
    total = 0
    for tid in todo:
        try:
            tour = cm.query(TOURNAMENT, {'id': tid})['tournament']
        except (RuntimeError, urllib.error.URLError) as e:
            print(f'{tid}: {e}', flush=True)
            continue
        s = tour['settings']['gameSessionSettings'] or {}
        fmt = (s.get('GameMode'), s.get('MapType'), s.get('MapSize'))
        n = 0
        if args.all or fmt == CHAMPIONSHIP:
            started = (tour['schedule']['startedAt'] or '')[:10]
            for g in games(tour):
                out.write(f"{g['game_id']}\t{tid}\t{g['match_id']}\t{g['mode']}\t{g['size']}\t{g['teams']}\t{g['turns']}\t"
                          f"{g['tribes']}\t{started}\t{tour['name']}\n")
                n += 1
        log.write(f"{tid}\t{fmt}\t{n}\t{tour['name']}\n")
        out.flush()
        log.flush()
        total += n
        print(f"{n:4} games  {fmt}  {tour['name']}", flush=True)
    print(f'{total} new games', flush=True)


if __name__ == '__main__':
    main()
