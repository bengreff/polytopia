"""Lists every tournament of Challengermode spaces, for py/cm_replays.py.

Challengermode's public API lists only a game's 50 newest tournaments. The website lists a space's
whole history through its own search (ap2.challengermode.com/api/SearchAggregation), which needs the
anonymous token the site issues to any visitor. This opens the space's page in a headless Chrome with a
fresh temporary profile (nothing signed in), takes that token from the page's own requests, and
pages through the search. The search returns at most 10,000 results per query, so it is split by
tournament type (1 tournaments, 4 instant tournaments, ...); a type above the cap is reported.

Appends new ids to data/replays/cm_tournaments.txt. Needs Google Chrome and the playwright package.

    python3 tools/cm_list_tournaments.py Polytopia PolyChampions [--types 1]
"""
import argparse
import json
import os

from playwright.sync_api import sync_playwright

SITE = 'https://www.challengermode.com'
API = 'https://ap2.challengermode.com/api'
CAP = 10000
OUT = 'data/replays/cm_tournaments.txt'


def F(key, *values):
    return {'Key': key, 'Value': [{'Value': v} for v in values]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('spaces', nargs='+', help='space slugs, as in challengermode.com/s/<slug>')
    ap.add_argument('--types', default='1', help='comma-separated tournament types (1 tournament, 4 instant)')
    args = ap.parse_args()
    known = {l.split()[0] for l in open(OUT) if l.strip() and not l.startswith('#')} if os.path.exists(OUT) else set()
    found = []
    with sync_playwright() as p:
        browser = p.chromium.launch(channel='chrome', headless=True)
        ctx = browser.new_context()
        page = ctx.new_page()
        token = {}
        page.on('request', lambda r: token.setdefault('auth', r.headers['authorization'])
                if r.url.startswith(API) and 'authorization' in r.headers else None)
        page.goto(f'{SITE}/s/{args.spaces[0]}/tournaments', wait_until='networkidle', timeout=90000)
        headers = {'authorization': token['auth'], 'content-type': 'application/json; charset=utf-8',
                   'x-requested-with': 'XMLHttpRequest', 'origin': SITE, 'referer': SITE + '/'}
        for slug in args.spaces:
            space = ctx.request.post(f'{API}/page/page/resolve_slug?slug={slug}', headers=headers).json()
            for kind in args.types.split(','):
                skip, n = 0, 0
                while True:
                    body = {'Searches': [{'ChildFilters': [], 'OrderBy': [], 'Index': 'tournament', 'SearchText': '',
                                          'Skip': skip, 'Take': 100,
                                          'Filters': [F('IsBlueprint', 'false'), F('OwnerPageId', space), F('Type', kind)]}]}
                    r = ctx.request.post(f'{API}/SearchAggregation/search/search', data=json.dumps(body), headers=headers)
                    res = r.json()[0]
                    if skip == 0:
                        total = sum(v['Count'] for v in res['Filters'].get('Type', {}).values() if v['Selected'])
                        if total > CAP:
                            print(f'{slug} type {kind}: {total} tournaments, only the first {CAP} are reachable')
                    for t in res['Results']:
                        found.append((t['Id'], (t['ScheduledStartTime'] or '')[:10], slug, t['Name']))
                        n += 1
                    if len(res['Results']) < 100 or skip + 100 >= CAP:
                        break
                    skip += 100
                print(f'{slug} type {kind}: {n} tournaments', flush=True)
        browser.close()
    new = [f for f in dict.fromkeys(found) if f[0] not in known]
    with open(OUT, 'a') as f:
        for tid, date, slug, name in new:
            f.write(f'{tid} {date} {slug}: {name}\n')
    print(f'{len(new)} new tournament ids added to {OUT}')


if __name__ == '__main__':
    main()
