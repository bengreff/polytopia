#!/bin/bash
# Research log S10 addendum: track s10x (search in the loop), branched from s9v at generation B (default 83),
# on the PC, beside the running S10 rotation. Like tools/s10_setup.sh for one track: links s9v's generations
# 0..B, copies its fog critic (critic_gB.pt) and the evaluations of B-2..B (no rival ones), writes the override
# (s10c's plus the search keys, and 4 inference shards instead of 7: with the planner a shard holds ~2.4 GB of GPU
# memory instead of ~1.1, measured) and LAST (B + 12). Then adds s10x to tracks.list (after s10c; written to a
# temporary file and moved into place, since tracks.sh reads the list every turn). Never touches other tracks.
#
#   bash ~/poly/py_sp/s10x_setup.sh [B]
set -eu
R=/home/greff/runs/sp
D=$R/s9v
R7=/home/greff/runs/r7_human_full/step020000.pt
STARTS=/home/greff/polydata/human_starts_big
B=${1:-83}
T=s10x
TAU=${TAU:-0.02}
[ -e $R/$T ] && { echo "$R/$T exists: not touched"; exit 1; }
COMMON="\"procs\": 28, \"eval_procs\": 8, \"min_procs\": 8, \"crazy_every\": 3, \"h_seeds\": 100, \"shards\": 7, \"critic\": true, \"script_eval\": \"rulebot\", \"starts\": \"$STARTS\", \"start_share\": 0.3, \"eval_starts\": \"$STARTS\""
BASE_LEARN="--gae_lambda 0.97 --entropy 0 --anchor $R7 --anchor_weight 0.03"
SEARCH="\"search_share\": 0.25, \"search_bin\": \"/home/greff/polylinux/polyhost-det\", \"search_extra\": \"--search_plans 16 --search_samples 2 --search_tau $TAU\""
mkdir -p $R/$T/evals
for g in $(seq 0 $B); do
    n=$(printf "gen%03d.pt" $g)
    ln -s "$(readlink -f $D/$n)" $R/$T/$n
done
cp $(printf "$D/critic_g%03d.pt" $B) $R/$T/
for g in $(seq $((B - 2)) $B); do cp $D/evals/*_g$(printf %03d $g).json $R/$T/evals/ 2>/dev/null || true; done
rm -f $R/$T/evals/rival_g*.json $R/$T/evals/hrival_g*.json
echo "{$COMMON, \"port\": 9850, \"learn_extra\": \"$BASE_LEARN --search_adv 1.0\", \"rival\": \"$R/s10c\", $SEARCH}" > $R/$T/override.json
python3 - <<EOP
import json
p = '$R/$T/override.json'
o = json.load(open(p))
o['shards'] = 4
json.dump(o, open(p, 'w'))
EOP
echo $((B + 12)) > $R/$T/LAST
echo "$T: $(ls $R/$T | wc -l) entries, LAST $(cat $R/$T/LAST)"
if ! grep -qx $T $R/tracks.list; then
    awk -v t=$T '{print} $0 == "s10c" {print t; done = 1} END {if (!done) print t}' $R/tracks.list > $R/tracks.list.tmp
    mv $R/tracks.list.tmp $R/tracks.list
fi
echo "tracks.list: $(tr '\n' ' ' < $R/tracks.list)"
