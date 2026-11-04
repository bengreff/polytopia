#!/bin/bash
# Self-play watchdog (2026-11-04), on the PC beside tracks.sh. A GPU hang (memory oversubscribed:
# the inference thread stuck in a CUDA graph replay) once froze s10x's games for 17 h with every
# process alive. Every 5 minutes: for each track whose self-play is running, if its newest game file
# is older than LIMIT minutes (and self-play has run that long), kill that self-play. sp_loop then
# retries the step (3 tries), and tracks.sh carries on. Logs to ~/runs/sp/watchdog.log.
#
#   setsid nohup ~/poly/py_sp/sp_watchdog.sh > /dev/null 2>&1 < /dev/null &
#   touch ~/runs/sp/WATCHDOG_STOP      stop it
R=/home/greff/runs/sp
LIMIT=${LIMIT:-45}
while [ ! -f $R/WATCHDOG_STOP ]; do
    for t in $(cat $R/tracks.list); do
        pid=$(pgrep -of "[s]elfplay.py $R/$t/gen" || true)
        [ -z "$pid" ] && continue
        age_run=$(( $(ps -o etimes= -p $pid | tr -d ' ') / 60 ))
        dir=$(ls -dt $R/$t/games/g[0-9]* 2>/dev/null | head -1)
        [ -z "$dir" ] && continue
        newest=$(find "$dir" -maxdepth 1 -name '*.npz' -printf '%T@\n' 2>/dev/null | sort -n | tail -1)
        [ -z "$newest" ] && newest=$(stat -c %Y "$dir")
        idle=$(( ($(date +%s) - ${newest%.*}) / 60 ))
        if [ $idle -ge $LIMIT ] && [ $age_run -ge $LIMIT ]; then
            echo "$(date '+%F %T') $t: no new game in $idle min ($dir, self-play up $age_run min): killing its self-play; GPU $(nvidia-smi --query-gpu=memory.used --format=csv,noheader)" >> $R/watchdog.log
            pkill -f "[s]elfplay.py $R/$t/gen"
            sleep 600  # let sp_loop's retry get going before looking again
        fi
    done
    sleep 300
done
echo "$(date '+%F %T') watchdog stopped" >> $R/watchdog.log
