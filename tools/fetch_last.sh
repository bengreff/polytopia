#!/bin/sh
# Waits until a PC training run has validated at a given step (its last.pt is then that step), and
# copies last.pt to the Mac.  usage: tools/fetch_last.sh <remote run dir> <step> <local path>
run=$1; step=$2; local=$3
until ssh -o ConnectTimeout=20 backhouse "wsl -d Ubuntu-22.04 -- grep -q '\"step\": $step, .*\"val\"' $run/log.jsonl" 2>/dev/null; do sleep 30; done
sleep 15
mkdir -p "$(dirname "$local")"
ssh backhouse "wsl -d Ubuntu-22.04 -- cat $run/last.pt" > "$local.tmp" && mv "$local.tmp" "$local"
