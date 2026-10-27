#!/bin/bash
# Self-play tracks in rotation (research log S7), on the PC: the tracks share the GPU by taking turns.
# Each turn runs py/sp_loop.py for exactly one generation (its games, learning and evaluations) of
# the track with the fewest generations, so the tracks stay in step and a track's rival generation
# exists before the track is evaluated against it. Tracks are run directories under ~/runs/sp, each
# with its own override.json, listed one per line in ~/runs/sp/tracks.list (read every turn). A
# track with a STOP file is skipped.
#
#   setsid nohup ~/poly/py_sp/tracks.sh > ~/runs/sp/tracks.out 2>&1 < /dev/null &
#   touch ~/runs/sp/TRACKS_STOP        stop after the current generation
R=/home/greff/runs/sp
cd /home/greff/poly/py_sp
fails=0
while [ ! -f $R/TRACKS_STOP ]; do
    best=; bestg=999999
    for t in $(cat $R/tracks.list); do
        [ -f $R/$t/STOP ] && continue
        g=0
        while [ -e $(printf "$R/$t/gen%03d.pt" $((g + 1))) ]; do g=$((g + 1)); done
        if [ $g -lt $bestg ]; then best=$t; bestg=$g; fi
    done
    [ -z "$best" ] && break
    echo "$(date '+%F %T') $best: generation $((bestg + 1))" >> $R/tracks.log
    if POLYHOST=linux /home/greff/polyvenv/bin/python sp_loop.py $R/$best --init /home/greff/runs/r7_human_full/step020000.pt \
            --generations $((bestg + 1)) >> $R/$best.out 2>&1 < /dev/null; then
        fails=0
    else
        fails=$((fails + 1))
        echo "$(date '+%F %T') $best: sp_loop failed ($fails in a row)" >> $R/tracks.log
        [ $fails -ge 3 ] && { touch $R/$best/STOP; echo "$(date '+%F %T') $best: stopped after 3 failures" >> $R/tracks.log; fails=0; }
        sleep 60
    fi
done
echo "$(date '+%F %T') tracks stopped" >> $R/tracks.log
