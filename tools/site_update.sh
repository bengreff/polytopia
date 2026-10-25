#!/bin/sh
# Updates the public replay site (GitHub Pages, ~/polytopia-site -> bengreff/polytopia-ai) from a
# self-play run on the PC, whenever a newer generation has finished its evaluations:
#   1. copy summary.tsv and the generation's evaluation games (vs Crazy, vs the previous generation;
#      ~2 MB) from the PC,
#   2. rank them by how back-and-forth they were (py/rank_games.py),
#   3. export replays of the best wins, the biggest comebacks and a loss, plus the results
#      (py/export_site.py),
#   4. commit and push the site.
# It is also the self-play loops' watchdog: for each run in $WATCH_RUNS (default: the published run),
# if its py/sp_loop.py isn't running on the PC (e.g. after someone restarted Windows) and <run>/STOP
# doesn't exist, it starts it again (the loop resumes; its settings are in <run>/override.json).
#
#   tools/site_update.sh s3            once
#   tools/site_update.sh s3 loop       every 30 minutes
#   WATCH_RUNS="s4a s4b" tools/site_update.sh s3 loop
set -u
cd "$(dirname "$0")/.."
run=$1
site=$HOME/polytopia-site
remote=/home/greff/runs/sp/$run
pc() { ssh -o ConnectTimeout=30 -o BatchMode=yes backhouse "wsl -d Ubuntu-22.04 -- $1" 2>/dev/null; }

ensure_running() {
    for w in ${WATCH_RUNS:-$run}; do
        r=/home/greff/runs/sp/$w
        pc "bash -s" <<EOF2
if ! pgrep -f '[s]p_loop.py $r ' > /dev/null && [ ! -f $r/STOP ]; then
    echo "\$(date '+%Y-%m-%d %H:%M:%S') restarted by the Mac's watchdog (loop not running)" >> $r/loop.log
    cd /home/greff/poly/py_sp
    POLYHOST=qemu setsid nohup /home/greff/polyvenv/bin/python sp_loop.py $r --init /home/greff/runs/r7_human_full/step020000.pt >> $r.out 2>&1 < /dev/null &
    sleep 8
    echo "restarted $w"
fi
EOF2
    done
}

once() {
    r=$(ensure_running) && [ -n "$r" ] && echo "$(date '+%H:%M') self-play loop was not running: $r"
    tools/sp_sync.sh "$run" > /dev/null || return 1
    # the newest generation with its Crazy evaluation finished
    gen=$(ls runs/sp/$run/evals/crazy_g*.json 2>/dev/null | sed 's/.*crazy_g\([0-9]*\).json/\1/' | sort | tail -1)
    [ -n "$gen" ] || { echo "$(date '+%H:%M') no evaluated generation yet"; return 0; }
    published=$(cat $site/.generation 2>/dev/null)
    [ "$gen" = "$published" ] && { echo "$(date '+%H:%M') generation $gen already published"; return 0; }
    sets=""
    for kind in crazy prev; do
        tag=sp_${run}_${kind}_g$gen
        [ -f runs/sp/$run/evals/${kind}_g$gen.json ] || continue
        if [ ! -f runs/games/$tag/games.jsonl ]; then
            pc "bash -c \"cd $remote/watch && tar czf - $tag\"" > runs/sp/$run/.w.tgz && tar xzf runs/sp/$run/.w.tgz -C runs/games \
                && rm -f runs/sp/$run/.w.tgz || return 1
        fi
        sets="$sets runs/games/$tag"
    done
    POLYHOST=mac .venv/bin/python py/rank_games.py $sets --rename "network (gen$gen.pt)=network (generation $((10#$gen)))" \
        --rename "network (gen$(printf %03d $((10#$gen - 1))).pt)=network (generation $((10#$gen - 1)))" > /dev/null || return 1
    best=$(python3 -c "
import json
gs = [json.loads(l) for l in open('runs/games/sp_${run}_crazy_g$gen/games.jsonl') if l.strip()]
w = sorted((g for g in gs if g.get('result') == 'win' and 'swing' in g), key=lambda g: -g['swing'])
print(w[0]['path'] if w else '')")
    POLYHOST=mac .venv/bin/python py/export_site.py "$site" --summary runs/sp/$run/summary.tsv --sets $sets \
        ${best:+--featured $best} || return 1
    echo "$gen" > $site/.generation
    (cd "$site" && git add -A && git commit -q -m "Generation $((10#$gen)): results and replays" && git push -q origin main) || return 1
    echo "$(date '+%H:%M') published generation $gen"
}

if [ "${2:-}" = loop ]; then
    while true; do once || echo "$(date '+%H:%M') update failed; retrying later"; sleep 1800; done
else
    once
fi
