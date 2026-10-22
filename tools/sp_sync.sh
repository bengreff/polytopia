#!/bin/sh
# Copies a self-play run's results from the PC to the Mac (py/sp_loop.py) into runs/sp/<run>/:
# by default only the small files (summary.tsv, loop.log, status.json, config.json, evaluation json).
#   --games   also the evaluation games, into runs/games/ for the web app's "Watch recorded games"
#             (~10 MB per generation)
#   --ckpt    also every generation's checkpoint not yet on the Mac (24 MB each)
# On Ben's phone hotspot the data is metered: use the flags at home. Retries while the link is down.
#
#   tools/sp_sync.sh s1 [--games] [--ckpt]
set -u
cd "$(dirname "$0")/.."
run=$1; shift
games=0; ckpt=0
for a in "$@"; do case $a in --games) games=1 ;; --ckpt) ckpt=1 ;; esac; done
remote=/home/greff/runs/sp/$run
mkdir -p runs/sp/$run runs/games
pc() { ssh -o ConnectTimeout=30 -o BatchMode=yes backhouse "wsl -d Ubuntu-22.04 -- $1" 2>/dev/null; }
once() {
    pc "bash -c \"cd $remote && tar czf - summary.tsv loop.log status.json config.json evals/*.json 2>/dev/null\"" > runs/sp/$run/.small.tgz \
        && tar xzf runs/sp/$run/.small.tgz -C runs/sp/$run && rm -f runs/sp/$run/.small.tgz || return 1
    if [ $ckpt = 1 ]; then
        for g in $(pc "bash -c \"cd $remote && ls gen*.pt\"" | tr -d '\r'); do
            [ -f runs/sp/$run/$g ] && continue
            pc "cat $remote/$g" > runs/sp/$run/$g.tmp && mv runs/sp/$run/$g.tmp runs/sp/$run/$g || { rm -f runs/sp/$run/$g.tmp; return 1; }
        done
    fi
    if [ $games = 1 ]; then
        pc "bash -c \"cd $remote/watch && tar czf - .\"" > runs/sp/$run/.watch.tgz && tar xzf runs/sp/$run/.watch.tgz -C runs/games \
            && rm -f runs/sp/$run/.watch.tgz || return 1
    fi
    echo "$(date '+%H:%M:%S') synced $run"
    column -t -s "$(printf '\t')" runs/sp/$run/summary.tsv 2>/dev/null | tail -12
}
n=0; until once; do n=$((n + 1)); [ $n -ge 10 ] && { echo "PC unreachable"; exit 1; }; sleep 30; done
