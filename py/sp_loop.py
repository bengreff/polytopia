"""The self-play generation loop (research-log S1). Runs on the PC (WSL), unattended:

    generation g: play --games training games with gen(g-1)   (py/selfplay.py, QEMU game processes + GPU)
                  one PPO step on them -> gen(g)               (py/train_rl.py: value baseline, self-play games only)
                  evaluate gen(g), in the background beside the next generation's games:
                    vs gen(g-1)  greedy, --h2h_seeds x 2 games, seeds from 13,000,000 (the same every generation)
                    vs Crazy     greedy, --crazy_seeds x 2 games, seeds from 10,000,000 (the standard bot seeds)
                    vs gen 0     greedy, --h2h_seeds x 2 games, seeds from 14,000,000
                  (Crazy and gen 0 every --crazy_every generations)
                    from held-out human positions (--eval_starts, every --crazy_every generations), seeds from
                    17,000,000: vs gen 0, and vs the --rival run's same generation

Opponents in the training games: the policy itself (--self_share), the game's AI (--bot_share, at
difficulty --bot), else an earlier generation drawn uniformly from the --fixed ones (always there,
e.g. 0,1,2) and the last --recent ones before gen(g-1). With --turn_cost, faster wins are worth a
little more (py/train_rl.py --turn_cost). With --starts and --start_share, that share of the games
(not those against the bot) begin from human midgame positions (py/human_starts.py).

Research log S9 options:
- --critic: a separate value network gives the advantages (py/train_rl.py --critic). Generation g's
  critic is critic_gNNN.pt, started from the policy at the first generation that uses it; it also
  learns from the --critic_window_gens previous generations' games.
- --anchor_every K: the KL anchor (--anchor in --learn_extra) moves to the run's own generation at
  the last multiple of K (a magnet that follows the run).
- --league "spec:share,...": more opponents, each spec a checkpoint or <run dir>@N (that run's N
  latest generations, the share split among them); their share comes out of the earlier
  generations' share.
- --reset_from <run dir>: an exploiter's reset. Generation g learns from the other run's latest
  generation instead of gen(g-1) once gen(g-1) beat the --rival with at least --reset_rate, or
  --reset_every generations after the last reset (or --branch_gen); logged in resets.tsv.
- Strategy indicators of each generation's training games (py/indicators.py) go to indicators.tsv.

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


def free_procs(cap, floor=8):
    """Game processes for the next step. The PC is shared: Ben's other projects (crucible) run only
    10pm-6am, so the loop takes up to `cap` in the daytime and half of it at night."""
    return cap if 6 <= time.localtime().tm_hour < 22 else max(floor, cap // 2)


RIVAL_KINDS = ('rival', 'hrival')


def eval_jobs(run, g, args):
    """The evaluations of generation g: [(kind, opponent, seeds, first seed)]; kinds starting with h
    begin from the held-out human positions (--eval_starts), seeds = positions."""
    jobs = []
    rival = gen_path(args.rival, g) if args.rival else ''
    diverged = rival and g > 0 and os.path.exists(rival) and not os.path.samefile(rival, gen_path(run, g))
    if diverged:
        jobs.append(('rival', rival, args.h2h_seeds, 15_000_000))  # the same generation of another run (after they diverge)
    if g > 0:
        jobs.append(('prev', gen_path(run, g - 1), args.h2h_seeds, 13_000_000))
    if g % args.crazy_every == 0 or g == 0:
        jobs.append(('crazy', 'bot:3', args.crazy_seeds, 10_000_000))
    if g > 0:
        jobs.append(('gen0', gen_path(run, 0), args.h2h_seeds, 14_000_000))
    if args.eval_starts and g > 0 and g % args.crazy_every == 0:
        jobs.append(('hgen0', gen_path(run, 0), args.h_seeds, 17_000_000))
        if diverged:
            jobs.append(('hrival', rival, args.h_seeds, 17_000_000))
    kinds = set(args.eval_kinds.split(','))
    return [j for j in jobs if j[0] in kinds]


def eval_label(kind, g, args):
    rival = os.path.basename(os.path.normpath(args.rival or '-'))
    return {'prev': f'gen{g - 1:03d}', 'crazy': 'crazy', 'gen0': 'gen000', 'rival': f'{rival}_gen{g:03d}',
            'hgen0': 'h_gen000', 'hrival': f'h_{rival}_gen{g:03d}'}[kind]


def latest_gen(run):
    g = 0
    while os.path.exists(gen_path(run, g + 1)):
        g += 1
    return g


def league_pool(args):
    """--league -> [(checkpoint, share)]."""
    out = []
    for part in [p for p in args.league.split(',') if p.strip()]:
        spec, share = part.rsplit(':', 1)
        if '@' in spec:
            d, n = spec.rsplit('@', 1)
            top = latest_gen(d)
            gens = list(range(top, max(-1, top - int(n)), -1))
            out += [(gen_path(d, e), float(share) / len(gens)) for e in gens]
        else:
            out.append((spec, float(share)))
    return out


def reset_source(run, g, args):
    """--reset_from: the checkpoint generation g learns from instead of gen(g-1), or None. Decided once
    per generation (resets.tsv), so a restart makes the same choice."""
    path = os.path.join(run, 'resets.tsv')
    rows = [l.rstrip('\n').split('\t') for l in open(path)] if os.path.exists(path) else []
    for r in rows:
        if int(r[0]) == g:
            return r[1]
    last = max([int(r[0]) for r in rows] + [args.branch_gen])
    j = os.path.join(run, 'evals', f'rival_g{g - 1:03d}.json')
    rate = json.load(open(j))['a_rate'] if os.path.exists(j) else None
    reason = (f'beat the rival {rate:.1%}' if rate is not None and rate >= args.reset_rate else
              f'{g - 1 - last} generations since the last reset' if g - 1 - last >= args.reset_every else None)
    if reason is None:
        return None
    src = gen_path(args.reset_from, latest_gen(args.reset_from))
    with open(path, 'a') as f:
        f.write(f'{g}\t{src}\t{reason}\n')
    log(run, f'gen {g:03d}: reset: learns from {src} ({reason})')
    return src


def missing(run, g, kind):
    return not os.path.exists(os.path.join(run, 'evals', f'{kind}_g{g:03d}.json'))


def start_evals(run, g, args, rival=True):
    """Starts generation g's missing evaluations one after another in a background process. Also
    catches up on up to two rival evaluations of recent generations: the rival run's generation may
    not have existed yet when they were evaluated."""
    catchup = [(e, j) for e in range(max(1, g - 6), g) for j in eval_jobs(run, e, args)
               if j[0] in RIVAL_KINDS and missing(run, e, j[0])][:2] if rival else []
    todo = catchup + [(g, j) for j in eval_jobs(run, g, args) if missing(run, g, j[0]) and (rival or j[0] != 'rival')]
    if not todo:
        return None
    procs = args.eval_procs
    log(run, f'gen {g:03d}: evaluating ({", ".join(j[0] + (f" of gen {e:03d}" if e != g else "") for e, j in todo)}) '
             f'with {procs} game processes')
    os.makedirs(os.path.join(run, 'evals'), exist_ok=True)
    script = []
    for e, (kind, opp, seeds, first) in todo:
        name = f'{kind}_g{e:03d}'
        base = os.path.join(run, 'evals', name)
        cmd = [PY, os.path.join(HERE, 'selfplay.py'), gen_path(run, e), '--match', opp, '--seeds', str(seeds),
               '--first_seed', str(first), '--results', base + '.tsv', '--procs', str(procs),
               '--slots', str(args.eval_slots), '--port', str(args.port + 100),
               '--save_games', os.path.join(run, 'watch', f'sp_{os.path.basename(run)}_{name}')]
        if kind.startswith('h'):
            cmd += ['--starts', args.eval_starts, '--starts_split', 'val']
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
    gens = [(g, job) for job in eval_jobs(run, g, args)]
    gens = [(e, job) for e in range(max(1, g - 8), g) for job in eval_jobs(run, e, args) if job[0] in RIVAL_KINDS] + gens
    for g, (kind, opp, _, _) in gens:
        j = os.path.join(run, 'evals', f'{kind}_g{g:03d}.json')
        label = eval_label(kind, g, args)
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
    ap.add_argument('--min_procs', type=int, default=8, help='never fewer game processes than this')
    ap.add_argument('--eval_procs', type=int, default=8, help='game processes for the evaluations, which run beside the next generation\'s games')
    ap.add_argument('--eval_slots', type=int, default=4)
    ap.add_argument('--port', type=int, default=9200)
    ap.add_argument('--rival', default='', help='another run directory: also evaluate each generation against its same generation')
    ap.add_argument('--eval_starts', default='', help='human positions (py/human_starts.py) for the h evaluations (held-out split)')
    ap.add_argument('--h_seeds', type=int, default=100, help='held-out human positions per h evaluation (each played twice)')
    ap.add_argument('--eval_kinds', default='rival,prev,crazy,gen0,hgen0,hrival')
    ap.add_argument('--critic', action='store_true', help='a separate value network for the advantages (py/train_rl.py --critic)')
    ap.add_argument('--critic_window_gens', type=int, default=1)
    ap.add_argument('--critic_extra', default='', help='more py/train_rl.py arguments for the critic')
    ap.add_argument('--anchor_every', type=int, default=0, help='the anchor follows the run: its generation at the last multiple of this')
    ap.add_argument('--league', default='', help='more opponents: "checkpoint:share" or "<run dir>@N:share", comma-separated')
    ap.add_argument('--reset_from', default='', help='exploiter: the run whose latest generation it resets to')
    ap.add_argument('--reset_rate', type=float, default=0.65)
    ap.add_argument('--reset_every', type=int, default=8)
    ap.add_argument('--branch_gen', type=int, default=0, help='the generation this run branched at (for --reset_every)')
    ap.add_argument('--indicators', type=int, default=1, help='strategy indicators of each generation\'s games (0: off)')
    args = ap.parse_args()
    run = args.run
    apply_overrides(run, args)  # before anything uses the settings (ports, processes, the evaluations below)
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
    own = lambda e: not os.path.islink(gen_path(run, e))  # a branch's linked generations were evaluated in their own run
    for e in range(max(0, g - 3), g - 1):  # older evaluations a restart left unfinished
        p = start_evals(run, e, args, rival=False) if own(e) else None  # waited for: no rival catch-up here
        if p:
            log(run, f'evaluating gen {e:03d} (left over)')
            p.wait()
        record_evals(run, e, args)
    # each generation's evaluations run in the background beside the next generation's games
    p = start_evals(run, g - 1, args) if own(g - 1) else None
    evals = (g - 1, p) if p else None
    record_evals(run, g - 1, args)
    while g <= args.generations and not stopped(run):
        apply_overrides(run, args)
        prev = gen_path(run, g - 1)
        if args.reset_from:
            prev = reset_source(run, g, args) or prev
        games_dir = os.path.join(run, 'games', f'g{g:03d}')
        pool = [f'self:{args.self_share}'] + ([f'bot:{args.bot}:{args.bot_share}'] if args.bot_share > 0 else [])
        league = league_pool(args)
        fixed = [int(x) for x in str(args.fixed).split(',') if x.strip()]
        past = sorted({p for p in fixed if p < g - 1} | set(range(max(0, g - 1 - args.recent), g - 1)))  # not gen(g-1)
        if past:
            share = (1 - args.self_share - (args.bot_share if args.bot_share > 0 else 0) - sum(s for _, s in league)) / len(past)
            pool += [f'{gen_path(run, p)}:{share:.4f}' for p in past]
        pool += [f'{p}:{s:.4f}' for p, s in league]
        status(run, generation=g, step='games', pool=pool)
        procs = free_procs(args.procs, args.min_procs) - (args.eval_procs if evals and evals[1].poll() is None else 0)
        procs = max(args.min_procs, procs)
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
        if args.indicators:  # a measurement only: a failure doesn't stop the loop
            with open(os.path.join(run, 'games', f'g{g:03d}.indicators.log'), 'w') as f:
                subprocess.call([PY, os.path.join(HERE, 'indicators.py'), f'g{g:03d}={games_dir}', '--max_games', '400',
                                 '--procs', '6', '--tsv', os.path.join(run, 'indicators.tsv')],
                                cwd=HERE, stdout=f, stderr=subprocess.STDOUT)
        status(run, step='learning')
        learn_dir = os.path.join(run, 'learn', f'g{g:03d}')
        t0 = time.time()
        apply_overrides(run, args)
        anchor = ['--bc', args.bc, '--bc_weight', str(args.bc_weight)] if args.bc_weight > 0 else []
        extra = args.learn_extra.split()
        if args.anchor_every > 0:  # after --learn_extra: the last --anchor counts
            extra += ['--anchor', gen_path(run, args.anchor_every * ((g - 1) // args.anchor_every))]
        critic_out = os.path.join(run, f'critic_g{g:03d}.pt')
        if args.critic:
            cin = os.path.join(run, f'critic_g{g - 1:03d}.pt')
            window = [os.path.join(run, 'games', f'g{e:03d}') for e in range(g - args.critic_window_gens, g)]
            window = [d for d in window if os.path.isdir(d)]
            extra += ['--critic', cin if os.path.exists(cin) else prev] + (['--critic_window', ','.join(window)] if window else []) \
                + args.critic_extra.split()
        if not os.path.exists(os.path.join(learn_dir, 'policy.pt')):
            shutil.rmtree(learn_dir, ignore_errors=True)
            run_cmd(run, [PY, os.path.join(HERE, 'train_rl.py'), prev, games_dir, learn_dir] + anchor +
                         ['--temperature', str(args.temperature),
                          '--lr', str(args.lr), '--epochs', str(args.epochs), '--baseline', args.baseline,
                          '--target_kl', str(args.target_kl), '--turn_cost', str(args.turn_cost)]
                    + extra,
                    os.path.join(run, 'learn', f'g{g:03d}.log'))
        last = (open(os.path.join(learn_dir, 'log.jsonl')).read().strip().splitlines() or ['(no log lines)'])[-1]
        log(run, f'gen {g:03d}: learned in {time.time() - t0:.0f} s: {last}')
        if args.critic:
            shutil.copy(os.path.join(learn_dir, 'critic.pt'), critic_out + '.tmp')
            os.replace(critic_out + '.tmp', critic_out)
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
