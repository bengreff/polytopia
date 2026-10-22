#!/bin/sh
# Runs the Mac's evaluation and data jobs one at a time: each line of runs/queue/todo.txt is a shell
# command, run from the repo root; finished lines move to done.txt with start/end times. Append
# lines to todo.txt at any time. Stops when the file `stop` appears.
cd /Users/ben/polytopia
Q=runs/queue
touch $Q/todo.txt $Q/done.txt
while [ ! -f $Q/stop ]; do
    line=$(head -1 $Q/todo.txt)
    if [ -z "$line" ]; then sleep 20; continue; fi
    tail -n +2 $Q/todo.txt > $Q/todo.tmp && mv $Q/todo.tmp $Q/todo.txt
    start=$(date '+%H:%M:%S')
    echo "$start running: $line" > $Q/current.txt
    sh -c "$line"
    echo "$start-$(date '+%H:%M:%S') exit $?	$line" >> $Q/done.txt
    rm -f $Q/current.txt
done
