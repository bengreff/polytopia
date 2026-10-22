#!/bin/sh
# Plays a checkpoint against bot difficulties on the fixed evaluation seeds (100 seeds x 2 sides
# each, seeds 10,000,000+) and appends the result lines to runs/evals/summary.tsv.
# usage: tools/eval.sh <checkpoint.pt> <tag> <difficulty...>   (difficulty 0-3, or `teacher<d>`)
cd /Users/ben/polytopia
ck=$1; tag=$2; shift 2
for d in "$@"; do
    case $d in
        teacher*) dd=${d#teacher}; extra=--teacher ;;
        *) dd=$d; extra= ;;
    esac
    POLYHOST=mac .venv/bin/python py/eval_vs_bot.py $ck $extra --seeds 100 --difficulty $dd --procs 6 --slots 6 \
        --port 8520 --out runs/evals/${tag}_d$d.tsv > runs/evals/${tag}_d$d.log 2>&1
    line=$(grep 'checkpoint step' runs/evals/${tag}_d$d.log)
    echo "$(date '+%m-%d %H:%M')	$tag	d$d	$line" >> runs/evals/summary.tsv
done
