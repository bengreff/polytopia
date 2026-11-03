"""A network against a scripted opponent (py/rulebot, or a later bot package with the same
`<package>.match.run` interface), as a py/sp_loop.py evaluation: prints a SUMMARY line in the format
of py/selfplay.py --match, from the network's side (each seed played twice, seats swapped).

    python script_eval.py gen083.pt --bot rulebot --seeds 50 --first_seed 19000000 --procs 8 --port 9600
"""
import argparse
import importlib
import json

import eval_vs_bot


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('net')
    ap.add_argument('--bot', default='rulebot', help='the scripted bot package (with a match.run function)')
    ap.add_argument('--seeds', type=int, default=50)
    ap.add_argument('--first_seed', type=int, default=19_000_000)
    ap.add_argument('--procs', type=int, default=8)
    ap.add_argument('--port', type=int, default=9600)
    args = ap.parse_args()
    run = importlib.import_module(args.bot + '.match').run
    summary, results = run(args.net, args.seeds, args.first_seed, args.procs, args.port, opponent=args.net, quiet=True)
    done = [r for r in results if r['result'] != 'error']
    net_wins = [r for r in done if r['result'] == 'loss']  # the bot lost a finished game: the network won it
    n, w = len(done), len(net_wins)
    lo, hi = eval_vs_bot.wilson(w, n)
    by_seat = [sum(r['seat'] == 1 - s for r in net_wins) for s in (0, 1)]  # the network's seat is the other one
    turns = [r['turns'] for r in done if r['result'] != 'cap']
    print('SUMMARY ' + json.dumps(dict(a=args.net, b=args.bot, games=n, a_wins=w, a_rate=w / max(n, 1), ci=[lo, hi],
                                       capped=summary['caps'], a_wins_by_seat=by_seat, errors=summary['errors'],
                                       mean_turns=sum(turns) / len(turns) if turns else None, first_seed=args.first_seed,
                                       seconds=round(summary['secs']))), flush=True)


if __name__ == '__main__':
    main()
