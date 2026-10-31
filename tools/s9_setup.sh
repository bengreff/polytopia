#!/bin/bash
# Research log S9: five tracks branched from track D (s5k) at its latest generation B, on the PC.
# Each track links D's generations 0..B, copies the evaluations of B-2..B (so none is rerun), gets
# its override.json, and a LAST file (B + 12: the tracks stop at the read). Writes tracks.list.
# Run once, with the old rotation stopped (~/runs/sp/TRACKS_STOP, its last generation finished):
#
#   bash ~/poly/py_sp/s9_setup.sh [B]
#   rm ~/runs/sp/TRACKS_STOP; setsid nohup ~/poly/py_sp/tracks.sh > ~/runs/sp/tracks.out 2>&1 < /dev/null &
set -eu
R=/home/greff/runs/sp
D=$R/s5k
R7=/home/greff/runs/r7_human_full/step020000.pt
STARTS=/home/greff/polydata/human_starts_big
B=${1:-}
if [ -z "$B" ]; then B=0; while [ -e $(printf "$D/gen%03d.pt" $((B + 1))) ]; do B=$((B + 1)); done; fi
echo "branching from $D at generation $B"
COMMON='"procs": 28, "eval_procs": 8, "min_procs": 8, "crazy_every": 3, "h_seeds": 100'
BASE_LEARN="--gae_lambda 0.97 --entropy 0 --anchor $R7 --anchor_weight 0.03"
HUMAN="\"starts\": \"$STARTS\", \"start_share\": 0.3, \"eval_starts\": \"$STARTS\""
declare -A OV
OV[s9c]="{$COMMON, $HUMAN, \"port\": 9500, \"learn_extra\": \"$BASE_LEARN\"}"
OV[s9v]="{$COMMON, $HUMAN, \"port\": 9600, \"learn_extra\": \"$BASE_LEARN\", \"rival\": \"$R/s9c\", \"critic\": true, \"critic_window_gens\": 1}"
OV[s9m]="{$COMMON, $HUMAN, \"port\": 9700, \"learn_extra\": \"$BASE_LEARN\", \"rival\": \"$R/s9c\", \"anchor_every\": 6}"
OV[s9l]="{$COMMON, $HUMAN, \"port\": 9800, \"learn_extra\": \"$BASE_LEARN\", \"rival\": \"$R/s9c\", \"league\": \"$R/s9x@2:0.25\", \"recent\": 3}"
OV[s9x]="{$COMMON, \"port\": 9900, \"learn_extra\": \"--gae_lambda 0.97 --entropy 0\", \"rival\": \"$R/s9l\", \"self_share\": 0.0, \"fixed\": \"\", \"recent\": 0, \"league\": \"$R/s9l@1:1.0\", \"reset_from\": \"$R/s9l\", \"reset_rate\": 0.65, \"reset_every\": 8, \"branch_gen\": $B, \"eval_kinds\": \"rival,prev\"}"
for t in s9c s9v s9m s9l s9x; do
    [ -e $R/$t ] && { echo "$R/$t exists: not touched"; exit 1; }
    mkdir -p $R/$t/evals
    for g in $(seq 0 $B); do
        n=$(printf "gen%03d.pt" $g)
        ln -s "$(readlink -f $D/$n)" $R/$t/$n
    done
    for g in $(seq $((B - 2)) $B); do cp $D/evals/*_g$(printf %03d $g).json $R/$t/evals/ 2>/dev/null || true; done
    rm -f $R/$t/evals/rival_g*.json  # the parent's rival was another run (an exploiter would read its rate)
    echo "${OV[$t]}" > $R/$t/override.json
    python3 -c "import json,sys; json.load(open('$R/$t/override.json'))"
    echo $((B + 12)) > $R/$t/LAST
    echo "$t: $(ls $R/$t | wc -l) entries, LAST $(cat $R/$t/LAST)"
done
printf 's9c\ns9v\ns9m\ns9l\ns9x\n' > $R/tracks.list
echo "tracks.list: $(tr '\n' ' ' < $R/tracks.list)"
