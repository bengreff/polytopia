#!/bin/bash
# HP1 arm A (research log HP1): stage 1 on the AI-teacher games (m1v2), then stage 2 on the
# championship human games (human_full), both with strategy conditioning z. Resumable: rerun this
# script and each stage resumes from its last.pt; a finished stage has a DONE file. Stop: touch STOP.
# Usage: hp1_chain.sh <run_dir>   (env: MB micro-batch, ACC accumulation, CAP GPU cap in GB)
# Research log S12, item 7 (HP1 finished, run hp1d): stage 1 continues arm A's stage 1 to R7's budget, then a
# fresh stage 2, then the HP1 tests:
#   S1_INIT=~/runs/hp1a/s1/last.pt S1_STEPS=88000 S2_STEPS=40000 TESTS=1 hp1_chain.sh ~/runs/hp1d
# Lives on the PC as ~/poly/hp/hp1_chain.sh (with copies of py/ in ~/poly/hp); this is the repo copy.
R=${1:-$HOME/runs/hp1a}
MB=${MB:-64}; ACC=${ACC:-4}; CAP=${CAP:-2.5}
PY=$HOME/polyvenv/bin/python
cd $HOME/poly/hp
mkdir -p $R
COMMON="--z --z_drop 0.1 --d 512 --layers 6 --batch $MB --accum $ACC --mem_cap_gb $CAP --grad_ckpt --workers 3 --window 24"
# GPU memory log (free must stay >= 1.5 GB), every 5 minutes while the chain runs
( while [ ! -f $R/ALL_DONE ] && [ ! -f $R/STOP ]; do
    echo "$(date '+%F %T') $(nvidia-smi --query-gpu=memory.used,memory.total,utilization.gpu --format=csv,noheader)" >> $R/gpu.log
    sleep 300; done ) &
wait_gpu() {  # wait until the GPU has CAP + 1.5 GB free
  while true; do
    used=$(nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits | head -1)
    total=$(nvidia-smi --query-gpu=memory.total --format=csv,noheader,nounits | head -1)
    free=$((total - used))
    need=$(python3 -c "print(int(($CAP + 1.5) * 1024))")
    [ $free -ge $need ] && return
    echo "$(date '+%F %T') waiting for GPU memory: free $free MiB < $need" >> $R/chain.log
    sleep 120
  done
}
run_stage() {  # name data steps lr extra...
  local name=$1 data=$2 steps=$3 lr=$4; shift 4
  for try in 1 2 3 4 5 6; do
    [ -f $R/STOP ] && return 1
    [ -f $R/$name/DONE ] && return 0
    wait_gpu
    echo "$(date '+%F %T') stage $name try $try" >> $R/chain.log
    nice -n 5 $PY train_bc.py $data $R/$name --steps $steps --lr $lr $COMMON --resume "$@" >> $R/$name.log 2>&1
    if [ $? -eq 0 ] && grep -q '^done' $R/$name.log; then touch $R/$name/DONE; return 0; fi
    echo "$(date '+%F %T') stage $name exited with an error; retrying in 2 min" >> $R/chain.log
    sleep 120
  done
  return 1
}
run_stage s1 $HOME/polydata/m1v2 ${S1_STEPS:-12000} 2e-4 --val_games 10 --val_every 2000 --save_every 5000 ${S1_INIT:+--init $S1_INIT} || exit 1
run_stage s2 $HOME/polydata/human_full ${S2_STEPS:-24000} 1e-4 --init $R/s1/last.pt --val_games 40 --val_every 1000 --save_every 4000 --value_val_games 400 || exit 1
touch $R/ALL_DONE
if [ -n "$TESTS" ] && [ ! -f $R/tests/DONE_all ]; then  # T1 (held-out imitation) and T3 (vs R7), tools/hp_tests.sh
    echo "$(date '+%F %T') tests on $R/s2/best.pt" >> $R/chain.log
    bash $HOME/poly/hp/hp_tests.sh $R/s2/best.pt $R/tests all
fi
echo "$(date '+%F %T') all stages done" >> $R/chain.log
