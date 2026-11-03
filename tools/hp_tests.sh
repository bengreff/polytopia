#!/bin/bash
# HP1 tests T1 (held-out imitation, z absent and own z; R7 on the same games once) and T3 (head-to-head
# vs R7, 200 games, seats swapped) for one checkpoint, on the PC. Ports 10000-10099; 4 game processes.
#   hp_tests.sh <checkpoint.pt> <out_dir> [t1|t3|all]
CK=$1; OUT=$2; WHAT=${3:-all}
R7=$HOME/runs/r7_human_full/step020000.pt
PY=$HOME/polyvenv/bin/python
cd $HOME/poly/hp
mkdir -p $OUT
if [ $WHAT = t1 ] || [ $WHAT = all ]; then
  nice -n 5 $PY human_bench.py $CK --data $HOME/polydata/human_full --games 150 --batch 64 --device cuda > $OUT/t1.txt 2>&1
  nice -n 5 $PY human_bench.py $CK --data $HOME/polydata/human_full --games 150 --batch 64 --device cuda --own_z >> $OUT/t1.txt 2>&1
  [ -s $HOME/runs/hp1a/t1_r7.txt ] || nice -n 5 $PY human_bench.py $R7 --data $HOME/polydata/human_full --games 150 --batch 64 --device cuda > $HOME/runs/hp1a/t1_r7.txt 2>&1
fi
if [ $WHAT = t3 ] || [ $WHAT = all ]; then
  POLYHOST=linux nice -n 5 $PY selfplay.py $CK --match $R7 --seeds 100 --first_seed 13000000 --results $OUT/t3.tsv \
    --procs 4 --slots 8 --port 10000 --shards 1 > $OUT/t3.log 2>&1
fi
echo done > $OUT/DONE_$WHAT
