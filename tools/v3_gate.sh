#!/bin/bash
# Planner v3's overnight test (research log "Planner v3", pre-registered 2026-11-24 22:15).
set -u
S=/home/greff/runs/sp; O=/home/greff/runs/v3; mkdir -p $O
PY=/home/greff/polyvenv/bin/python
export POLYHOST=linux POLYHOST_BIN=/home/greff/polylinux/polyhost-v3
cd /home/greff/poly/v3/py
NETS="--net $S/s11x/gen107.pt --scorer $S/s11x/critic_g107.pt"
ENS="--a_samples 4 --a_scorers $S/s11x/critic_g104.pt,$S/s11x/critic_g105.pt,$S/s11x/critic_g106.pt,$S/s12/critic_g108.pt,$S/s12/critic_g109.pt"
R7=/home/greff/runs/r7_human_full/step020000.pt
P1=${P1:-8}; P2=${P2:-16}
log() { echo "$(date '+%F %T') $*" >> $O/driver.log; }
rate() { python3 -c "import json,sys; r=[json.loads(l) for l in open(sys.argv[1])]; r=[x for x in r if x.get('res')!='error']; print(sum(x['res']=='win' for x in r), len(r))" $1; }
log "driver pid $$: screens (2 x 100 games, $P1 processes each)"
$PY -m tactics.match --a v3 --b v2 $NETS $ENS --seeds 50 --first_seed 44000000 --procs $P1 --port 10800 --out $O/screen_bot.jsonl > $O/screen_bot.log 2>&1 &
a=$!
$PY -m tactics.match --a v3 --b v2 $NETS $ENS --a_model $R7 --seeds 50 --first_seed 44000000 --procs $P1 --port 10900 --out $O/screen_r7.jsonl > $O/screen_r7.log 2>&1 &
b=$!
wait $a $b
read wb nb <<< "$(rate $O/screen_bot.jsonl)"; read wr nr <<< "$(rate $O/screen_r7.jsonl)"
log "screens: gen 107 reply $wb/$nb, R7 reply $wr/$nr"
if python3 -c "import sys; wb,nb,wr,nr=map(int,sys.argv[1:]); sys.exit(0 if wr/max(nr,1) > wb/max(nb,1) else 1)" $wb $nb $wr $nr; then
    MODEL="--a_model $R7"; V=r7
else
    MODEL=""; V=bot
fi
P2=$(cat $O/P2 2>/dev/null || echo $P2)  # (can be changed while the screens run)
log "gate: v3 with the $V reply, 400 games, $P2 processes"
$PY -m tactics.match --a v3 --b v2 $NETS $ENS $MODEL --seeds 200 --first_seed 44100000 --procs $P2 --port 11000 --out $O/gate_$V.jsonl > $O/gate_$V.log 2>&1
log "gate done: $(grep SUMMARY $O/gate_$V.log | cut -c1-400)"
$PY -m tactics.match --a v3 --b net:$R7 $NETS $ENS $MODEL --seeds 50 --first_seed 44300000 --procs $P1 --port 11100 --out $O/vs_gen0_$V.jsonl > $O/vs_gen0_$V.log 2>&1 &
a=$!
$PY -m tactics.match --a v3 --b crazyplus $NETS $ENS $MODEL --seeds 50 --first_seed 44400000 --procs $P1 --port 11200 --out $O/vs_crazyplus_$V.jsonl > $O/vs_crazyplus_$V.log 2>&1 &
b=$!
wait $a $b
log "checks: vs gen 0 $(rate $O/vs_gen0_$V.jsonl), vs CrazyPlus $(rate $O/vs_crazyplus_$V.jsonl)"
log "all done"
touch $O/DONE
