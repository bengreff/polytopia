#!/bin/bash
# Research log S11: track s11x, planner v2 in the loop on half the learner's turns, from s10x gen 95, on the PC.
# - s11x links s10x's generations 0..95, copies its fog critic critic_g095.pt and the evaluations of gens
#   93..95 (no rival ones), and gets its override.json and LAST (107).
# - s10x95 is the frozen rival: generations 0..95 are s10x's, every later one is s10x gen 95, so each new
#   generation of s11x is evaluated against its own start (sp_loop --rival).
# - tracks.list then holds s11x alone (s10c, s10p, s10t and s10x stay stopped at 95).
# Run with the rotation stopped (no tracks.sh running):
#
#   bash ~/poly/py_sp/s11_setup.sh
#   setsid nohup ~/poly/py_sp/tracks.sh >> ~/runs/sp/tracks.out 2>&1 < /dev/null &
set -eu
R=/home/greff/runs/sp
D=$R/s10x
B=95
T=s11x
F=s10x95
R7=/home/greff/runs/r7_human_full/step020000.pt
STARTS=/home/greff/polydata/human_starts_big
for x in $T $F; do [ -e $R/$x ] && { echo "$R/$x exists: not touched"; exit 1; }; done
mkdir -p $R/$T/evals $R/$F
for g in $(seq 0 $B); do
    n=$(printf "gen%03d.pt" $g)
    ln -s "$(readlink -f $D/$n)" $R/$T/$n
    ln -s "$(readlink -f $D/$n)" $R/$F/$n
done
for g in $(seq $((B + 1)) $((B + 40))); do
    ln -s "$(readlink -f $D/gen095.pt)" $R/$F/$(printf "gen%03d.pt" $g)
done
cp $D/critic_g095.pt $R/$T/
for g in $(seq $((B - 2)) $B); do cp $D/evals/*_g$(printf %03d $g).json $R/$T/evals/ 2>/dev/null || true; done
rm -f $R/$T/evals/rival_g*.json $R/$T/evals/hrival_g*.json
cat > $R/$T/override.json <<JSON
{"procs": 28, "eval_procs": 8, "min_procs": 8, "crazy_every": 3, "h_seeds": 100, "shards": 3, "critic": true,
 "script_eval": "crazyplus", "starts": "$STARTS", "start_share": 0.3, "eval_starts": "$STARTS", "port": 9850,
 "learn_extra": "--gae_lambda 0.97 --entropy 0 --anchor $R7 --anchor_weight 0.03 --search_adv 1.0",
 "rival": "$R/$F", "search_share": 0.5, "search_bin": "/home/greff/polylinux/polyhost-cp",
 "search_extra": "--search_plans 16 --search_samples 2 --search_v2 {}"}
JSON
python3 -c "import json; json.load(open('$R/$T/override.json'))"
echo $((B + 12)) > $R/$T/LAST
echo $T > $R/tracks.list
echo "$T: $(ls $R/$T | wc -l) entries, LAST $(cat $R/$T/LAST); $F: $(ls $R/$F | wc -l) generations; tracks.list: $(cat $R/tracks.list)"
