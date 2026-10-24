"""The self-play generation loop (research-log S1). Runs on the PC (WSL), unattended:

    generation g: play --games training games with gen(g-1)   (py/selfplay.py, QEMU game processes + GPU)
                  one PPO step on them -> gen(g)               (py/train_rl.py: value baseline, self-play games only)
                  evaluate gen(g), in the background beside the next generation's games:
                    vs gen(g-1)  greedy, --h2h_seeds x 2 games, seeds from 13,000,000 (the same every generation)
                    vs Crazy     greedy, --crazy_seeds x 2 games, seeds from 10,000,000 (the standard bot seeds)
                    vs gen 0     greedy, --h2h_seeds x 2 games, seeds from 14,000,000
                  (Crazy and gen 0 every --crazy_every generations)

Opponents in the training games: the policy itself (--self_share), the game's AI (--bot_share, at
difficulty --bot), else an earlier generation drawn uniformly from the --fixed ones (always there,
e.g. 0,1,2) and the last --recent ones before gen(g-1). With --turn_cost, faster wins are worth a
little more (py/train_rl.py --turn_cost). With --starts and --start_share, that share of the games
(not those against the bot) begin from human midgame positions (py/human_starts.py).

Files under <run>: gen000.pt (the start), gen001.pt, ...; games/gNNN (training games); learn/gNNN
(learner output and log); evals/<kind>_gNNN.{tsv,json,log}; watch/<kind>_gNNN (evaluation games for
the web app's watch mode); summary.tsv (one line per evaluation); loop.log; status.json.
Restartable: finished steps are skipped (games already played, gen files present, evals with a json).
Stop between steps: touch <run>/STOP. Change settings between steps: <run>/override.json.

    cd ~/poly/py_sp && POLYHOST=qemu setsid nohup ~/polyvenv/bin/python sp_loop.py ~/runs/sp/s1 \
        --init ~/runs/r4_human_ft/step020000.pt > ~/runs/sp/s1.out 2>&1 < /dev/null &
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PY = sys.executable


def log(run, msg):
    line = f"{time.strftime('%Y-%m-%d %H:%M:%S')} {msg}"
    print(line, flush=True)
    with open(os.path.join(run, 'loop.log'), 'a') as f:
        f.write(line + '\n')


def status(run, **kw):
    path = os.path.join(run, 'status.json')
    st = json.load(open(path)) if os.path.exists(path) else {}
    st.update(kw, updated=time.strftime('%Y-%m-%d %H:%M:%S'))
    json.dump(st, open(path + '.tmp', 'w'), indent=1)
    os.replace(path + '.tmp', path)


def gen_path(run, g):
    return os.path.join(run, f'gen{g:03d}.pt')


def stopped(run):
    return os.path.exists(os.path.join(run, 'STOP'))


def run_cmd(run, cmd, logfile, tries=3):
    """Runs a step to completion, retrying a failed one (the steps resume where they stopped);
    raises after `tries` failures."""
    for k in range(tries):
        with open(logfile, 'a') as f:
            f.write(f"\n# {time.strftime('%Y-%m-%d %H:%M:%S')} {' '.join(cmd)}\n")
            f.flush()
            rc = subprocess.call(cmd, cwd=HERE, stdout=f, stderr=subprocess.STDOUT)
        if rc == 0:
            return
        log(run, f'step failed (exit {rc}, try {k + 1} of {tries}): see {logfile}')
        time.sleep(30)
    raise RuntimeError(f'step failed {tries} times: see {logfile}')


def apply_overrides(run, args):
    """<run>/override.json, if present, changes settings from the next step on, e.g.
    {"bc_weight": 0, "games": 2000}; every change is logged once."""
    path = os.path.join(run, 'override.json')
    if not os.path.exists(path):
        return
    try:
        new = json.load(open(path))
    except ValueError as e:
        log(run, f'override.json unreadable, ignored: {e}')
        return
    for k, v in new.items():
        if hasattr(args, k) and getattr(args, k) != v:
            log(run, f'override: {k} = {v} (was {getattr(args, k)})')
            setattr(args, k, v)


def free_procs(cap):
    """Game processes for the next step. The PC is shared: Ben's other projects (crucible) run only
    10pm-6am, so the loop takes up to `cap` in the daytime and half of it at night."""
    return cap if 6 <= time.localtime().tm_hour < 22 else max(8, cap // 2)


def eval_jobs(run, g, args):
    """The evaluations of generation g: [(kind, opponent, seeds, first seed)]."""
    jobs = []
    if g > 0:
        jobs.append(('prev', gen_path(run, g - 1), args.h2h_seeds, 13_000_000))
    if g % args.crazy_every == 0 or g == 0:
        jobs.append(('crazy', 'bot:3', args.crazy_seeds, 10_000_000))
        if g > 0:
            jobs.append(('gen0', gen_path(run, 0), args.h2h_seeds, 14_000_000))
    return jobs


def start_evals(run, g, args):
    """Starts generation g's missing evaluations one after another in a background process."""
    todo = [j for j in eval_jobs(run, g, args) if not os.path.exists(os.path.join(run, 'evals', f'{j[0]}_g{g:03d}.json'))]
    if not todo:
        return None
    procs = args.eval_procs
    log(run, f'gen {g:03d}: evaluating ({", ".join(j[0] for j in todo)}) with {procs} game processes')
    os.makedirs(os.path.join(run, 'evals'), exist_ok=True)
    script = []
    for kind, opp, seeds, first in todo:
        name = f'{kind}_g{g:03d}'
        base = os.path.join(run, 'evals', name)
        cmd = [PY, os.path.join(HERE, 'selfplay.py'), gen_path(run, g), '--match', opp, '--seeds', str(seeds),
               '--first_seed', str(first), '--results', base + '.tsv', '--procs', str(procs),
               '--slots', str(args.eval_slots), '--port', str(args.port + 100),
               '--save_games', os.path.join(run, 'watch', f'sp_{os.path.basename(run)}_{name}')]
        # a clean start: a half-finished evaluation's lines would be counted twice
        script.append(f"rm -f {base}.tsv; {' '.join(cmd)} > {base}.log 2>&1 && "
                      f"grep '^SUMMARY ' {base}.log | sed 's/^SUMMARY //' > {base}.json.tmp && mv {base}.json.tmp {base}.json")
    return subprocess.Popen(['bash', '-c', ' ; '.join(script)], cwd=HERE)


def record_evals(run, g, args):
    """Appends generation g's finished evaluations to summary.tsv (once each)."""
    path = os.path.join(run, 'summary.tsv')
    have = set()
    if os.path.exists(path):
        have = {tuple(l.split('\t')[:2]) for l in open(path) if not l.startswith('gen\t')}
    else:
        with open(path, 'w') as f:
            f.write('gen\tvs\tgames\twins\trate\tci_low\tci_high\twins_seat1\twins_seat2\tcapped\tmean_turns\tfinished\n')
    for kind, opp, _, _ in eval_jobs(run, g, args):
        j = os.path.join(run, 'evals', f'{kind}_g{g:03d}.json')
        label = {'prev': f'gen{g - 1:03d}', 'crazy': 'crazy', 'gen0': 'gen000'}[kind]
        if not os.path.exists(j) or (f'{g:03d}', label) in have:
            continue
        s = json.load(open(j))
        with open(path, 'a') as f:
            f.write(f"{g:03d}\t{label}\t{s['games']}\t{s['a_wins']}\t{s['a_rate']:.3f}\t{s['ci'][0]:.3f}\t{s['ci'][1]:.3f}\t"
                    f"{s['a_wins_by_seat'][0]}\t{s['a_wins_by_seat'][1]}\t{s['capped']}\t"
                    f"{(s['mean_turns'] or 0):.1f}\t{time.strftime('%Y-%m-%d %H:%M')}\n")
        log(run, f"gen {g:03d} vs {label}: {s['a_wins']}/{s['games']} ({s['a_rate']:.1%}, "
                 f"95% CI {s['ci'][0]:.1%}-{s['ci'][1]:.1%})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run')
    ap.add_argument('--init', required=True, help='generation 0')
    ap.add_argument('--generations', type=int, default=1000)
    ap.add_argument('--games', type=int, default=1500, help='training games per generation')
    ap.add_argument('--self_share', type=float, default=0.5)
    ap.add_argument('--fixed', default='0', help='generations always in the pool, e.g. "0,1,2"')
    ap.add_argument('--recent', type=int, default=7, help='the last N generations before gen(g-1) in the pool')
    ap.add_argument('--bot_share', type=float, default=0.0, help='training games against the game\'s AI')
    ap.add_argument('--bot', type=int, default=3, help='its difficulty (3 = Crazy)')
    ap.add_argument('--turn_cost', type=float, default=0.0, help='py/train_rl.py --turn_cost')
    ap.add_argument('--starts', default='', help='human midgame states (py/human_starts.py) for py/selfplay.py --starts')
    ap.add_argument('--start_share', type=float, default=0.0, help='share of training games that begin from one')
    ap.add_argument('--temperature', type=float, default=0.5)
    ap.add_argument('--lr', type=float, default=3e-5)
    ap.add_argument('--bc', default='/home/greff/polydata/human_full:1')
    ap.add_argument('--bc_weight', type=float, default=0.0,
                    help='weight of a behaviour-cloning anchor on --bc; 0 (default) trains on self-play games only, '
                         'so the human games aren\'t replayed (memorized) generation after generation')
    ap.add_argument('--epochs', type=int, default=2)
    ap.add_argument('--baseline', default='value', help='py/train_rl.py --baseline')
    ap.add_argument('--target_kl', type=float, default=0.05, help='py/train_rl.py --target_kl: the most a generation may move')
    ap.add_argument('--learn_extra', default='', help='more py/train_rl.py arguments, e.g. "--workers 4"')
    ap.add_argument('--h2h_seeds', type=int, default=100)
    ap.add_argument('--crazy_seeds', type=int, default=100)
    ap.add_argument('--crazy_every', type=int, default=1)
    ap.add_argument('--procs', type=int, default=28, help='game processes in the daytime (half at night, when other projects run)')
    ap.add_argument('--slots', type=int, default=6)
    ap.add_argument('--eval_procs', type=int, default=8, help='game processes for the evaluations, which run beside the next generation\'s games')
    ap.add_argument('--eval_slots', type=int, default=4)
    ap.add_argument('--port', type=int, default=9200)
    args = ap.parse_args()
    run = args.run
    for d in ('games', 'learn', 'evals', 'watch'):
        os.makedirs(os.path.join(run, d), exist_ok=True)
    if not os.path.exists(gen_path(run, 0)):
        shutil.copy(args.init, gen_path(run, 0) + '.tmp')
        os.replace(gen_path(run, 0) + '.tmp', gen_path(run, 0))
    json.dump(vars(args), open(os.path.join(run, 'config.json'), 'w'), indent=1)
    g = 1
    while os.path.exists(gen_path(run, g)):
        g += 1
    log(run, f'starting at generation {g} (pid {os.getpid()})')
    for e in range(max(0, g - 3), g - 1):  # older evaluations a restart left unfinished
        p = start_evals(run, e, args)
        if p:
            log(run, f'evaluating gen {e:03d} (left over)')
            p.wait()
        record_evals(run, e, args)
    # each generation's evaluations run in the background beside the next generation's games
    p = start_evals(run, g - 1, args)
    evals = (g - 1, p) if p else None
    record_evals(run, g - 1, args)
    while g <= args.generations and not stopped(run):
        apply_overrides(run, args)
        prev = gen_path(run, g - 1)
        games_dir = os.path.join(run, 'games', f'g{g:03d}')
        pool = [f'self:{args.self_share}'] + ([f'bot:{args.bot}:{args.bot_share}'] if args.bot_share > 0 else [])
        fixed = [int(x) for x in str(args.fixed).split(',') if x.strip()]
        past = sorted({p for p in fixed if p < g - 1} | set(range(max(0, g - 1 - args.recent), g - 1)))  # not gen(g-1)
        if past:
            share = (1 - args.self_share - (args.bot_share if args.bot_share > 0 else 0)) / len(past)
            pool += [f'{gen_path(run, p)}:{share:.4f}' for p in past]
        status(run, generation=g, step='games', pool=pool)
        procs = free_procs(args.procs) - (args.eval_procs if evals and evals[1].poll() is None else 0)
        procs = max(8, procs)
        log(run, f'gen {g:03d}: playing {args.games} games with gen {g - 1:03d} on {procs} game processes, '
                 f'pool {",".join(os.path.basename(p) for p in pool)}')
        t0 = time.time()
        run_cmd(run, [PY, os.path.join(HERE, 'selfplay.py'), prev, '--out_dir', games_dir, '--games', str(args.games),
                      '--pool', ','.join(pool), '--temperature', str(args.temperature),
                      '--first_seed', str(50_000_000 + g * 100_000), '--procs', str(procs),
                      '--slots', str(args.slots), '--port', str(args.port)]
                + (['--starts', args.starts, '--start_share', str(args.start_share)] if args.starts and args.start_share > 0 else []),
                os.path.join(run, 'games', f'g{g:03d}.log'))
        tail = (open(os.path.join(run, 'games', f'g{g:03d}.log')).read().strip().splitlines() or [''])[-1]
        log(run, f'gen {g:03d}: games done in {time.time() - t0:.0f} s: {tail}')
        status(run, step='learning')
        learn_dir = os.path.join(run, 'learn', f'g{g:03d}')
        t0 = time.time()
        apply_overrides(run, args)
        anchor = ['--bc', args.bc, '--bc_weight', str(args.bc_weight)] if args.bc_weight > 0 else []
        if not os.path.exists(os.path.join(learn_dir, 'policy.pt')):
            shutil.rmtree(learn_dir, ignore_errors=True)
            run_cmd(run, [PY, os.path.join(HERE, 'train_rl.py'), prev, games_dir, learn_dir] + anchor +
                         ['--temperature', str(args.temperature),
                          '--lr', str(args.lr), '--epochs', str(args.epochs), '--baseline', args.baseline,
                          '--target_kl', str(args.target_kl), '--turn_cost', str(args.turn_cost)]
                    + args.learn_extra.split(),
                    os.path.join(run, 'learn', f'g{g:03d}.log'))
        last = (open(os.path.join(learn_dir, 'log.jsonl')).read().strip().splitlines() or ['(no log lines)'])[-1]
        log(run, f'gen {g:03d}: learned in {time.time() - t0:.0f} s: {last}')
        shutil.copy(os.path.join(learn_dir, 'policy.pt'), gen_path(run, g) + '.tmp')
        os.replace(gen_path(run, g) + '.tmp', gen_path(run, g))
        if evals is not None:
            evals[1].wait()
            record_evals(run, evals[0], args)
        p = start_evals(run, g, args)
        evals = (g, p) if p else None
        g += 1
    if evals is not None:
        evals[1].wait()
        record_evals(run, evals[0], args)
    log(run, 'stopped' if stopped(run) else 'done')
    status(run, step='stopped')


if __name__ == '__main__':
    main()
