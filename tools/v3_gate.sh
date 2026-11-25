#!/bin/bash
# Planner v3's test run (research log "Planner v3", pre-registered 2026-11-24 22:15): two 100-game screens, a
# 400-game gate for the better reply model, then 100 games each vs raw gen 0 and CrazyPlus.
# Resumable: rerun it and finished games are skipped. Each stage retries (up to 10 times) until all its games are in.
# Workers per stage fit the GPU's free memory at that moment (about 1.2 GB each), so it shares the card with
# whatever else is loaded; a guard stops one worker when GPU memory passes MEMMAX and restarts a stage that finishes
# no game for STALL seconds. Statistics: $O/stats.md and stats.json (py/tactics/v3_stats.py), refreshed every 30 min.
# Stop: kill -- -<driver pid> (everything runs in the driver's process group).
set -u
S=/home/greff/runs/sp; O=/home/greff/runs/v3; mkdir -p $O
PY=/home/greff/polyvenv/bin/python
STATS=/home/greff/poly/v3/py/tactics/v3_stats.py
export POLYHOST=linux POLYHOST_BIN=/home/greff/polylinux/polyhost-v3
export OMP_NUM_THREADS=2 MKL_NUM_THREADS=2  # many workers: torch would give each all 28 threads
cd /home/greff/poly/v3/py
NETS="--net $S/s11x/gen107.pt --scorer $S/s11x/critic_g107.pt"
ENS="--a_samples 4 --a_scorers $S/s11x/critic_g104.pt,$S/s11x/critic_g105.pt,$S/s11x/critic_g106.pt,$S/s12/critic_g108.pt,$S/s12/critic_g109.pt"
R7=/home/greff/runs/r7_human_full/step020000.pt
CAP=${CAP:-12}  # most workers at once (12 = 14.7 GB measured 2026-11-25)
MEMMAX=${MEMMAX:-14500}  # GPU MiB: above this one worker is stopped (2026-11-25: 8 h with almost no games at 14.7 GB)
STALL=${STALL:-10800}    # s with no finished game before a stage is restarted
log() { echo "$(date '+%F %T') $*" >> $O/driver.log; }
kill_tree() { local p c; for p in "$@"; do for c in $(pgrep -P $p); do kill_tree $c; done; kill $p 2>/dev/null; done; }
guard() {  # guard <stage> <match pid> <results file>: shed a worker when GPU memory is nearly full; restart a stalled stage
    local name=$1 m=$2 f=$3 t=$(date +%s) s=-1 now cur used w
    while kill -0 $m 2>/dev/null; do
        sleep 60
        now=$(date +%s); cur=$(stat -c %s $f 2>/dev/null || echo 0)
        [ "$cur" != "$s" ] && { s=$cur; t=$now; }
        if [ $(( now - t )) -gt $STALL ]; then
            log "$name: no game finished in $(( STALL / 60 )) min; restarting the stage"; kill_tree $m; return
        fi
        used=$(nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits 2>/dev/null)
        if [ "${used:-0}" -gt $MEMMAX ] && [ $(pgrep -P $m -f spawn_main | wc -l) -gt 1 ] && mkdir $O/.shedlock 2>/dev/null; then
            echo $BASHPID > $O/.shedlock/pid  # (one guard at a time sheds; the lock is released after memory settles)
            w=$(pgrep -P $m -f spawn_main | tail -1)
            log "$name: GPU memory $used MiB > $MEMMAX; stopping worker $w (its games are replayed later)"
            kill_tree $w; sleep 120; rm -rf $O/.shedlock
        fi
    done
}
count() { [ -f $1 ] && python3 -c "import json,sys; print(len({(r['seed'],r['seat']) for r in map(json.loads, open(sys.argv[1])) if r.get('res')!='error'}))" $1 || echo 0; }
rate() { python3 -c "import json,sys; r={(x['seed'],x['seat']):x for x in map(json.loads, open(sys.argv[1])) if x.get('res')!='error'}.values(); print(sum(x['res']=='win' for x in r), len(r))" $1; }
fit() {  # workers that fit in the GPU's free memory now, at most $1, at least 2
    local used total n
    read used total <<< "$(nvidia-smi --query-gpu=memory.used,memory.total --format=csv,noheader,nounits | tr -d ',')"
    n=$(( (total - used - 1200) / 1300 )); [ $n -gt $1 ] && n=$1; [ $n -lt 2 ] && n=2; echo $n
}
rm -rf $O/.shedlock
stage() {  # stage <name> <games> <cap> <port> <match args...>
    local name=$1 games=$2 cap=$3 port=$4 n try m g; shift 4
    for try in 1 2 3 4 5 6 7 8 9 10; do
        [ "$(count $O/$name.jsonl)" -ge $games ] && return 0
        n=$(fit $cap)
        log "$name: try $try, $(count $O/$name.jsonl) of $games games done, $n processes"
        $PY -m tactics.match "$@" --procs $n --port $port --out $O/$name.jsonl >> $O/$name.log 2>&1 &
        m=$!
        guard $name $m $O/$name.jsonl &
        g=$!
        wait $m
        kill_tree $g
        [ "$(cat $O/.shedlock/pid 2>/dev/null)" = "$g" ] && rm -rf $O/.shedlock
    done
    log "$name: still $(count $O/$name.jsonl) of $games games after 10 tries"
}
( while kill -0 $$ 2>/dev/null && [ ! -f $O/DONE ]; do python3 $STATS $O > /dev/null 2>&1; sleep 1800; done ) &

N=$(fit $CAP)
log "driver pid $$: screens (2 x 100 games), $N processes in all"
stage screen_bot 100 $(( (N + 1) / 2 )) 10800 --a v3 --b v2 $NETS $ENS --seeds 50 --first_seed 44000000 &
a=$!
stage screen_r7 100 $(( N / 2 )) 10900 --a v3 --b v2 $NETS $ENS --a_model $R7 --seeds 50 --first_seed 44000000 &
b=$!
wait $a $b
read wb nb <<< "$(rate $O/screen_bot.jsonl)"; read wr nr <<< "$(rate $O/screen_r7.jsonl)"
log "screens: gen 107 reply $wb/$nb, R7 reply $wr/$nr"
python3 $STATS $O >> $O/driver.log 2>&1
if python3 -c "import sys; wb,nb,wr,nr=map(int,sys.argv[1:]); sys.exit(0 if wr/max(nr,1) > wb/max(nb,1) else 1)" $wb $nb $wr $nr; then
    MODEL="--a_model $R7"; V=r7
else
    MODEL=""; V=bot
fi
log "gate: v3 with the $V reply, 400 games"
stage gate_$V 400 $(cat $O/P2 2>/dev/null || echo $CAP) 11000 --a v3 --b v2 $NETS $ENS $MODEL --seeds 200 --first_seed 44100000
log "gate done: $(grep SUMMARY $O/gate_$V.log | tail -1 | cut -c1-400)"
python3 $STATS $O >> $O/driver.log 2>&1
N=$(fit $CAP)
log "checks: 2 x 100 games, $N processes in all"
stage vs_gen0_$V 100 $(( (N + 1) / 2 )) 11100 --a v3 --b net:$R7 $NETS $ENS $MODEL --seeds 50 --first_seed 44300000 &
a=$!
stage vs_crazyplus_$V 100 $(( N / 2 )) 11200 --a v3 --b crazyplus $NETS $ENS $MODEL --seeds 50 --first_seed 44400000 &
b=$!
wait $a $b
log "checks: vs gen 0 $(rate $O/vs_gen0_$V.jsonl), vs CrazyPlus $(rate $O/vs_crazyplus_$V.jsonl)"
if [ "$(count $O/gate_$V.jsonl)" -ge 400 ] && [ "$(count $O/vs_gen0_$V.jsonl)" -ge 100 ] && [ "$(count $O/vs_crazyplus_$V.jsonl)" -ge 100 ]; then
    touch $O/DONE; log "all done"
else
    touch $O/INCOMPLETE; log "finished with missing games (see above)"
fi
python3 $STATS $O >> $O/driver.log 2>&1
