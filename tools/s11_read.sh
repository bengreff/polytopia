#!/bin/bash
# Research log S11 read, on the PC: the games the pre-registered read still needs, one job after another
# (results in ~/runs/s11_read; a finished job is skipped on a rerun):
#   (2) gens 104-107 of s11x vs s10c gen 95, 200 games each (seeds 15,000,000+, as the rival evaluations);
#   gen 107 vs gen 0 (its evaluation was cut off when the loop stopped at LAST: 20 of 200 games);
#   (3) gen 107 + planner v2 (critic_g107) vs s10x gen 95 + planner v2 (critic_g095), 200 games.
# (1), raw gens 104-107 vs s10x gen 95, is the loop's own rival evaluations. The self-play matches run
# from ~/poly/py_sp (the loop's code); the planner match from ~/poly/py_read, a copy of the repo's py/
# (py_sp has no plan_agent.py; py_read's plan_agent, eval_vs_bot and model match the v2 gate's copy).
# A job counts as done only with at least 180 finished games.
#
#   setsid nohup bash ~/poly/py_sp/s11_read.sh > ~/runs/s11_read/read.out 2>&1 < /dev/null &
# The Windows throttle (tools/throttle.ps1, without -Start) should run beside it.
set -u
S=/home/greff/runs/sp
O=/home/greff/runs/s11_read
PY=/home/greff/polyvenv/bin/python
cd /home/greff/poly/py_sp
mkdir -p $O
log() { echo "$(date '+%F %T') $*" >> $O/read.log; }
enough() {  # summary line in $1.log with >= 180 games -> $1.json
    grep '^SUMMARY ' $O/$1.log | sed 's/^SUMMARY //' > $O/$1.json.tmp
    if $PY -c "import json,sys; d=json.load(open('$O/$1.json.tmp')); sys.exit(0 if d.get('games', 0) >= 180 else 1)" 2>/dev/null; then
        mv $O/$1.json.tmp $O/$1.json
    else
        rm -f $O/$1.json.tmp; log "$1: too few games, not done"
    fi
}
match() {  # name, generation, opponent, seeds, first seed
    [ -f $O/$1.json ] && return
    log "$1: start"
    rm -f $O/$1.tsv
    POLYHOST=linux $PY selfplay.py $2 --match $3 --seeds $4 --first_seed $5 --results $O/$1.tsv --procs 8 --slots 4 \
        --port 9950 > $O/$1.log 2>&1
    enough $1
    log "$1: $(tail -1 $O/$1.log)"
}
log "read started"
for g in 104 105 106 107; do
    match c_g$g $S/s11x/gen$g.pt $S/s10c/gen095.pt 100 15000000
done
match gen0_g107 $S/s11x/gen107.pt $S/s11x/gen000.pt 100 14000000
if [ ! -f $O/planner.json ]; then
    log "planner: start"
    cd /home/greff/poly/py_read
    POLYHOST=linux POLYHOST_BIN=/home/greff/polylinux/polyhost-cp $PY -m tactics.match --a v2 --b v2 \
        --net $S/s11x/gen107.pt --scorer $S/s11x/critic_g107.pt --b_net $S/s10x/gen095.pt --b_scorer $S/s10x/critic_g095.pt \
        --seeds 100 --first_seed 43000000 --procs 8 --port 10300 --out $O/planner.jsonl > $O/planner.log 2>&1
    enough planner
    log "planner: $(grep '^SUMMARY' $O/planner.log | cut -c1-300)"
fi
log "read done"
touch $O/DONE
