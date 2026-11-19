#!/bin/bash
# Copies new python files into ~/poly/py_sp at a moment between self-play runs of the rotation (while a
# generation's learning step runs: train_rl on a run under ~/runs/sp, and no selfplay.py anywhere), so no
# generation's games mix two versions of the code. Files go in the order given, each as a new file then mv
# (never edit a file a running job may read). Order them so any prefix is a working mix: a file whose old
# callers still work with it first. The old versions are kept as <file>.prev. Logs to ~/runs/sp/tracks.log.
#
#   setsid nohup bash deploy_between_gens.sh <staging py dir> fast_infer.py tactics/plan_check.py selfplay.py \
#       > /dev/null 2>&1 < /dev/null &
set -u
R=/home/greff/runs/sp
P=/home/greff/poly/py_sp
S=$1; shift
log() { echo "$(date '+%F %T') deploy: $*" >> $R/tracks.log; }
for f in "$@"; do [ -f $S/$f ] || { log "no $S/$f: nothing deployed"; exit 1; }; done
log "waiting for a learning step with no self-play running, to deploy $* from $S (pid $$)"
while true; do
    if pgrep -af "[t]rain_rl.py" | grep -q "/runs/sp/" && ! pgrep -f "[s]elfplay.py" > /dev/null; then
        for f in "$@"; do
            [ -f $P/$f ] && cp -a $P/$f $P/$f.prev
            cp $S/$f $P/$f.new && mv $P/$f.new $P/$f
        done
        if (cd $P && /home/greff/polyvenv/bin/python -c "import selfplay, fast_infer, search_v2, train_rl, sp_loop" > /dev/null 2>&1); then
            log "deployed $* (old versions: *.prev)"
        else
            for f in "$@"; do [ -f $P/$f.prev ] && { cp -a $P/$f.prev $P/$f.new && mv $P/$f.new $P/$f; }; done
            log "imports failed after the deploy: the old files are back"
        fi
        exit 0
    fi
    sleep 30
done
