"""Puts the showcase human games (py/human_midgames.py's runs/human_midgames_showcase.tsv) in the local
web app's watch list (runs/games/human_showcase), each opening at its chosen midgame position, with
a note on why it was picked. Local only: these are other players' games, never published.

    .venv/bin/python py/human_showcase.py
"""
import base64
import json
import os

import polyenv

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRIBE_NAMES = {v: k for k, v in polyenv.TRIBES.items()}


def main():
    src = os.path.join(REPO, 'runs', 'human_midgames_showcase.tsv')
    out = os.path.join(REPO, 'runs', 'games', 'human_showcase')
    os.makedirs(out, exist_ok=True)
    rows = [l.rstrip('\n').split('\t') for l in open(src)]
    rows = [dict(zip(rows[0], r)) for r in rows[1:]]
    with open(os.path.join(out, 'games.jsonl'), 'w') as meta:
        for i, r in enumerate(rows):
            raw = json.load(open(os.path.join(REPO, 'data', 'replays', 'raw', r['game'] + '.json')))
            name = f"{i + 1:02d}_{r['game'][:8]}"
            for ext, key in (('start', 'initialGameStateData'), ('final', 'currentGameStateData')):
                with open(os.path.join(out, f'{name}.{ext}'), 'wb') as f:
                    f.write(base64.b64decode(raw[key]))
            tribes = json.loads(r['tribes'])
            units, cities, army = json.loads(r['units']), json.loads(r['cities']), json.loads(r['army'])
            p = int(r['player'])
            note = (f"opens at turn {r['turn']}, player {p} to move: scores within {100 - 100 * float(r['score_close']):.0f}%, "
                    f"{units[0] + units[1]} units ({r['contact']} in contact), cities {cities[0]}-{cities[1]}, armies "
                    f"{army[0]}-{army[1]} stars; {r['total_lead_changes']} score-lead changes in the game"
                    + ('; the player ahead here lost' if r['upset'] == 'True' else ''))
            winner = int(r['winner'])
            meta.write(json.dumps(dict(path=f'human_showcase/{name}', tag='human_showcase', time='', tribes=tribes,
                                       players={'1': 'human player 1', '2': 'human player 2'}, agent=1,
                                       result='win' if winner == 1 else 'loss', turns=int(r['game_turns']), note=note,
                                       jump=dict(turn=int(r['turn']), player=p))) + '\n')
            print(f"{name}: {'/'.join(TRIBE_NAMES.get(t, str(t)) for t in tribes)}, {note}")


if __name__ == '__main__':
    main()
