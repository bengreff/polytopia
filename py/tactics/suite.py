"""The tactical suite for planner v2: positions with kill chains, on offence and defence, and v1 vs v2
on them.

    python -m tactics.suite collect --net gen083.pt --games 40 --first_seed 41000000 --out DIR [--ben DIR]
        Greedy self-play games of the network (both seats), every turn start from turn 8 saved, each
        labelled on the true state by the tactical search (a large one, 'truth'):
        - offence: the best sequence for the side to move (kills, stars);
        - threat: the side to move ends its turn at once; the opponent's best sequence then.
        Kept: offence kills >= 2 (a chain is available), or threat kills >= 2 (the opponent threatens
        one); at most 3 of each per game. --ben adds Ben's game's turn starts (runs/tactics/ben).
    python -m tactics.suite run --net ... --scorer ... --dir DIR --out results.jsonl
        For each position, v1 and v2 each plan the side to move's turn (fog-safe, as in play); the
        commands are played on the true state, and measured there:
        - kills and stars gained this turn (did it find and play the chain?);
        - after its End Turn, the opponent's best tactical reply ('truth' search): stars and units lost.
"""
import argparse
import copy
import json
import os
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

TRUTH = dict(depth=16, beam=8, branch=12, max_sims=2500, top=1)
LABEL = dict(depth=14, beam=6, branch=10, max_sims=600, top=1)
END_TURN = 15


def search_best(tac, env, slot, st):
    r = tac.search(slot, st) if not st.ended else []
    return (r[0]['obj'], r[0]['kills'], r[0]['lost']) if r else (0.0, 0, 0)


def label(env, spec, blob, tac, slot=950):
    """(offence (stars, kills, lost), threat if the side to move passes (stars, kills, lost)) on the true state."""
    st = env.load(slot, spec, blob)
    off = search_best(tac, env, slot, st)
    e = np.nonzero(st.legal[:, 0] == END_TURN)[0]
    if not len(e):
        return off, (0.0, 0, 0)
    st = env.step([(slot, int(e[0]))])[0][1]
    return off, search_best(tac, env, slot, st)


def collect(args):
    import torch

    import eval_vs_bot
    import polyenv
    from tactics.match import tribes_for
    from tactics.search import Tactician
    dev = args.device or ('cuda' if torch.cuda.is_available() else 'mps' if torch.backends.mps.is_available() else 'cpu')
    if dev == 'cpu':
        torch.set_num_threads(2)
    os.makedirs(args.out, exist_ok=True)
    net, _ = eval_vs_bot.load_policy(args.net, dev)
    index = open(os.path.join(args.out, args.index), 'a')
    with polyenv.launch(args.port) as env:
        tac = Tactician(env, None, range(500, 540), **LABEL)
        if args.ben:
            spec = polyenv.Spec(tribes=(7, 13), bots=(None, None), seed=0)
            tac.spec = spec
            for n in sorted(os.listdir(args.ben)):
                if not n.endswith('.state') or n.endswith('p255.state') or int(n[1:3]) < 8:
                    continue
                blob = open(os.path.join(args.ben, n), 'rb').read()
                st = env.load(950, spec, blob)
                if st.current not in (1, 2):
                    continue
                off, thr = label(env, spec, blob, tac)
                name = 'ben_' + n
                open(os.path.join(args.out, name), 'wb').write(blob)
                index.write(json.dumps(dict(name=name, src='ben', tribes=[7, 13], seed=0, turn=int(st.turn), player=int(st.current),
                                            offence=off, threat=thr)) + '\n')
                index.flush()
        for g in range(args.games):
            seed = args.first_seed + g
            tribes = tribes_for(seed)
            spec = polyenv.Spec(tribes=tribes, bots=(None, None), seed=seed)
            tac.spec = spec
            st = env.new(0, spec)
            kept = {'off': 0, 'thr': 0}
            n, last = 0, None
            t0 = time.time()
            while not st.ended and st.turn < 60:
                if st.turn >= 8 and (st.turn, st.current) != last:
                    last = (st.turn, st.current)
                    blob = env.save(0)
                    off, thr = label(env, spec, blob, tac)
                    want_off = off[1] >= 2 and kept['off'] < 3
                    want_thr = thr[1] >= 2 and kept['thr'] < 3
                    if want_off or want_thr:
                        kept['off'] += want_off
                        kept['thr'] += want_thr
                        name = f'sp{seed}_t{st.turn:02d}_p{st.current}.state'
                        open(os.path.join(args.out, name), 'wb').write(blob)
                        index.write(json.dumps(dict(name=name, src='selfplay', tribes=list(tribes), seed=seed, turn=int(st.turn),
                                                    player=int(st.current), offence=off, threat=thr)) + '\n')
                        index.flush()
                obs = env.observe([(0, 0)])[0]
                a = int(eval_vs_bot.choose(net, dev, [obs], [st], 0.0, None)[0])
                n += 1
                st = env.step([(0, a)])[0][1]
            print(f'game {seed}: {st.turn} turns, kept {kept}, {time.time() - t0:.0f} s', flush=True)


def run(args):
    import torch

    import plan_agent
    import polyenv
    from tactics.search import Tactician, material
    dev = 'cuda' if torch.cuda.is_available() else 'mps' if torch.backends.mps.is_available() else 'cpu'
    rows = [json.loads(line) for f in sorted(os.listdir(args.dir)) if f.startswith('index') and f.endswith('.jsonl')
            for line in open(os.path.join(args.dir, f))]
    rows = [r for r in rows if r['offence'][1] >= 2 or r['threat'][1] >= 2]  # the suite: a chain for either side
    if args.only:
        rows = [r for r in rows if r['src'] == args.only]
    if args.per_game:  # at most this many self-play positions per game (evenly spread over its turns)
        by, keep = {}, []
        for r in rows:
            by.setdefault((r['src'], r['seed']), []).append(r)
        for (src, seed), rs in by.items():
            rs.sort(key=lambda r: (r['turn'], r['player']))
            if src != 'ben' and len(rs) > args.per_game:
                rs = [rs[int(j * len(rs) / args.per_game)] for j in range(args.per_game)]
            keep += rs
        rows = sorted(keep, key=lambda r: (r['src'] != 'ben', r['name']))
    done = set()
    if os.path.exists(args.out):
        done = {(json.loads(line)['name'], json.loads(line)['planner']) for line in open(args.out)}
    out = open(args.out, 'a')
    opts = json.loads(args.v2_opts) if args.v2_opts else {}
    with polyenv.launch(args.port) as env:
        P2 = plan_agent.Planner(env, args.net, args.scorer, dev, args.plans, args.samples, 0.5, tactical=opts)
        P1 = copy.copy(P2)
        P1.tactical = None
        for i, r in enumerate(rows):
            spec = polyenv.Spec(tribes=tuple(r['tribes']), bots=(None, None), seed=r['seed'])
            tac = Tactician(env, spec, range(600, 640), **TRUTH)
            blob = open(os.path.join(args.dir, r['name']), 'rb').read()
            for tag, P in (('v1', P1), (args.tag, P2)):
                if (r['name'], tag) in done:
                    continue
                P.rng = np.random.default_rng(1000 + i)
                t0 = time.time()
                played, info = P.plan_turn(spec, blob)
                secs = time.time() - t0
                st = env.load(900, spec, blob)
                me = st.current
                m0 = material(*env.observe([(900, me), (900, 3 - me)]))
                bad = 0
                for c in played:
                    k = plan_agent.Planner.find(st, c)
                    if k < 0:
                        bad += 1
                        break
                    st = env.step([(900, k)])[0][1]
                    if st.ended or st.current != me:
                        break
                if not st.ended and st.current == me:  # (a plan without its End Turn)
                    e = np.nonzero(st.legal[:, 0] == END_TURN)[0]
                    if len(e):
                        st = env.step([(900, int(e[0]))])[0][1]
                m1 = material(*env.observe([(900, me), (900, 3 - me)]))
                gained = (m1[0] - m0[0]) - (m1[1] - m0[1])
                kills = m0[3] - m1[3]
                thr = search_best(tac, env, 900, st) if not st.ended and st.current != me else (0.0, 0, 0)
                rec = dict(name=r['name'], src=r['src'], planner=tag, turn=r['turn'], player=me, offence_avail=r['offence'],
                           threat_pass=r['threat'], kills=int(kills), gained=round(gained, 2), threat_after=thr,
                           dealt=round(m0[1] - m1[1], 2), own_change=round(m1[0] - m0[0], 2),
                           won=bool(st.ended and st.winner == me), seconds=round(secs, 2), bad=bad,
                           kind=info.get('kind'), repairs=len(info.get('repairs', [])))
                out.write(json.dumps(rec) + '\n')
                out.flush()
                print(i, rec, flush=True)


def summary(path, tag='v2'):
    recs = [json.loads(line) for line in open(path)]
    by = {}
    for r in recs:
        if r['planner'] in ('v1', tag):
            by.setdefault(r['name'], {})['v1' if r['planner'] == 'v1' else 'v2'] = r
    pairs = [v for v in by.values() if 'v1' in v and 'v2' in v]
    out = {}
    for cat, sel in (('offence', lambda v: v['v1']['offence_avail'][1] >= 2), ('defence', lambda v: v['v1']['threat_pass'][1] >= 2)):
        for src in ('all', 'ben', 'selfplay'):
            ps = [v for v in pairs if sel(v) and (src == 'all' or v['v1']['src'] == src)]
            if not ps:
                continue
            row = dict(n=len(ps))
            for tag in ('v1', 'v2'):
                rs = [v[tag] for v in ps]
                avail = np.array([x['offence_avail'][1] for x in rs])
                kills = np.array([x['kills'] for x in rs])
                row[tag] = dict(
                    chain_played=round(float(np.mean(kills >= 2)), 3),            # >= 2 kills this turn
                    chain_full=round(float(np.mean(kills >= avail)), 3),          # as many kills as the search's best
                    kills=round(float(kills.mean()), 2), stars_dealt=round(float(np.mean([x['dealt'] for x in rs])), 2),
                    net_swing=round(float(np.mean([x['gained'] for x in rs])), 2),
                    lost_to_reply=round(float(np.mean([x['threat_after'][0] for x in rs])), 2),
                    units_lost_to_reply=round(float(np.mean([x['threat_after'][1] for x in rs])), 2),
                    reply_chain=round(float(np.mean([x['threat_after'][1] >= 2 for x in rs])), 3),
                    seconds=round(float(np.median([x['seconds'] for x in rs])), 2))
            # the pre-registered paired test: per position, v2 minus v1 (offence: stars gained; defence:
            # stars lost to the best reply, v1 minus v2), mean with a normal 95% interval
            if cat == 'offence':
                d = np.array([v['v2']['dealt'] - v['v1']['dealt'] for v in ps])
            else:
                d = np.array([v['v1']['threat_after'][0] - v['v2']['threat_after'][0] for v in ps])
            se = d.std(ddof=1) / np.sqrt(len(d)) if len(d) > 1 else float('nan')
            row['v2_better_by'] = dict(mean=round(float(d.mean()), 2), ci=[round(float(d.mean() - 1.96 * se), 2), round(float(d.mean() + 1.96 * se), 2)],
                                       v2_better=int((d > 0).sum()), v1_better=int((d < 0).sum()), same=int((d == 0).sum()))
            out[f'{cat}/{src}'] = row
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', choices=('collect', 'run', 'summary'))
    ap.add_argument('--net')
    ap.add_argument('--scorer')
    ap.add_argument('--games', type=int, default=40)
    ap.add_argument('--first_seed', type=int, default=41_000_000)
    ap.add_argument('--out')
    ap.add_argument('--dir')
    ap.add_argument('--ben', default=None)
    ap.add_argument('--only', default=None)
    ap.add_argument('--per_game', type=int, default=0)
    ap.add_argument('--plans', type=int, default=16)
    ap.add_argument('--samples', type=int, default=2)
    ap.add_argument('--v2_opts', default=None)
    ap.add_argument('--tag', default='v2', help='run / summary: the name of the v2 configuration (--v2_opts)')
    ap.add_argument('--port', type=int, default=10250)
    ap.add_argument('--device', default=None)
    ap.add_argument('--index', default='index.jsonl', help='collect: the index file written in --out')
    args = ap.parse_args()
    if args.cmd == 'collect':
        collect(args)
    elif args.cmd == 'run':
        run(args)
    else:
        print(json.dumps(summary(args.out, args.tag), indent=1))


if __name__ == '__main__':
    main()
