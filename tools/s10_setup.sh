#!/bin/bash
# Research log S10: tracks branched from the critic track (s9v) at generation B (default 83), on the PC.
# Each track links s9v's generations 0..B and copies its fog critic (critic_gB.pt) and the evaluations
# of B-2..B (no rival ones), gets its override.json and a LAST file (B + 12). Writes tracks.list.
# Run with the rotation stopped (no tracks.sh running):
#
#   bash ~/poly/py_sp/s10_setup.sh [B]
#   setsid nohup ~/poly/py_sp/tracks.sh >> ~/runs/sp/tracks.out 2>&1 < /dev/null &
set -eu
R=/home/greff/runs/sp
D=$R/s9v
R7=/home/greff/runs/r7_human_full/step020000.pt
STARTS=/home/greff/polydata/human_starts_big
B=${1:-83}
echo "branching from $D at generation $B"
# The scripted evaluation bots read the game's data file from the repo root (py/rulebot/data.py).
mkdir -p /home/greff/poly/game/dump/gamedata
[ -e /home/greff/poly/game/dump/gamedata/GameLogicData28.txt ] || \
    ln -s /home/greff/polylinux/gamedata/GameLogicData28.txt /home/greff/poly/game/dump/gamedata/GameLogicData28.txt
COMMON="\"procs\": 28, \"eval_procs\": 8, \"min_procs\": 8, \"crazy_every\": 3, \"h_seeds\": 100, \"shards\": 7, \"critic\": true, \"script_eval\": \"rulebot\", \"starts\": \"$STARTS\", \"start_share\": 0.3, \"eval_starts\": \"$STARTS\""
BASE_LEARN="--gae_lambda 0.97 --entropy 0 --anchor $R7 --anchor_weight 0.03"
declare -A OV
OV[s10c]="{$COMMON, \"port\": 9500, \"learn_extra\": \"$BASE_LEARN\"}"
OV[s10p]="{$COMMON, \"port\": 9600, \"learn_extra\": \"$BASE_LEARN\", \"rival\": \"$R/s10c\", \"pcritic\": true}"
OV[s10t]="{$COMMON, \"port\": 9700, \"learn_extra\": \"$BASE_LEARN --turn_lambda 0.9\", \"rival\": \"$R/s10c\"}"
TRACKS=${TRACKS:-"s10c s10p s10t"}
for t in $TRACKS; do
    [ -e $R/$t ] && { echo "$R/$t exists: not touched"; exit 1; }
    mkdir -p $R/$t/evals
    for g in $(seq 0 $B); do
        n=$(printf "gen%03d.pt" $g)
        ln -s "$(readlink -f $D/$n)" $R/$t/$n
    done
    cp $(printf "$D/critic_g%03d.pt" $B) $R/$t/
    for g in $(seq $((B - 2)) $B); do cp $D/evals/*_g$(printf %03d $g).json $R/$t/evals/ 2>/dev/null || true; done
    rm -f $R/$t/evals/rival_g*.json $R/$t/evals/hrival_g*.json
    echo "${OV[$t]}" > $R/$t/override.json
    python3 -c "import json; json.load(open('$R/$t/override.json'))"
    echo $((B + 12)) > $R/$t/LAST
    echo "$t: $(ls $R/$t | wc -l) entries, LAST $(cat $R/$t/LAST)"
done
printf '%s\n' $TRACKS > $R/tracks.list
echo "tracks.list: $(tr '\n' ' ' < $R/tracks.list)"
