#!/bin/bash
# HP1 arm A: runs the tests as stage 2's checkpoints appear (research log HP1). Detached; log in
# ~/runs/hp1a/tests/after.log. T1 at s2 steps 8000 and 16000 (beside training, small GPU batches);
# after ALL_DONE: T1 on best.pt and last.pt, T3 (vs R7), T2 + T4 (controllability, vs Crazy).
R=$HOME/runs/hp1a; T=$R/tests; mkdir -p $T
log() { echo "$(date '+%F %T') $*" >> $T/after.log; }
gpu_ok() {  # free GPU memory >= 2.5 GB (1.5 GB reserve + the test)
  while true; do
    used=$(nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits | head -1)
    total=$(nvidia-smi --query-gpu=memory.total --format=csv,noheader,nounits | head -1)
    [ $((total - used)) -ge 2560 ] && return
    log "waiting for GPU memory ($((total - used)) MiB free)"; sleep 120
  done
}
waitfor() { while [ ! -f "$1" ]; do [ -f $R/STOP ] && exit 0; sleep 120; done; sleep 30; }
for s in 008000 016000; do
  waitfor $R/s2/step$s.pt
  [ -s $T/s2_$s/t1.txt ] && continue
  gpu_ok; log "T1 on s2 step $s"
  bash $HOME/poly/hp/hp_tests.sh $R/s2/step$s.pt $T/s2_$s t1
  log "T1 s2 step $s: $(head -1 $T/s2_$s/t1.txt | cut -c1-200)"
done
waitfor $R/ALL_DONE
for c in best last; do
  [ -s $T/s2_$c/t1.txt ] || { gpu_ok; log "T1 on $c"; bash $HOME/poly/hp/hp_tests.sh $R/s2/$c.pt $T/s2_$c t1; }
done
[ -f $T/s2_best/DONE_t3 ] || { gpu_ok; log "T3 on best"; bash $HOME/poly/hp/hp_tests.sh $R/s2/best.pt $T/s2_best t3; log "T3: $(grep SUMMARY $T/s2_best/t3.log | cut -c1-300)"; }
gpu_ok; log "T2 + T4 on best"
HOSTKIND=linux PY=$HOME/polyvenv/bin/python H=$HOME/polydata/human_full PORT=10010 DEVICE=cuda CODE=$HOME/poly/hp \
  bash $HOME/poly/hp/tools/hp_control.sh $R/s2/best.pt $T/ctrl_best > $T/ctrl_best.out 2>&1
log "T2 + T4 done"
touch $T/ALL_TESTS_DONE
