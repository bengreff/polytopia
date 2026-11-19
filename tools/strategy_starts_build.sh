#!/bin/bash
# Builds the strategy-tagged start states (py/strategy_positions.py -> py/human_starts.py) on the PC's
# native engine, in K shards on ports 11000.., then joins the shards' indexes into index.tsv and
# writes DONE (docs/design/strategy-starts.md). Run from a copy of py/ that holds
# strategy_positions.tsv (e.g. ~/poly/py_strat):
#   cd ~/poly/py_strat && setsid nohup bash strategy_starts_build.sh > ~/polydata/strategy_starts.log 2>&1 < /dev/null &
set -u
OUT=${OUT:-/home/greff/polydata/strategy_starts}
K=${K:-6}
PY=${PY:-/home/greff/polyvenv/bin/python}
mkdir -p "$OUT"
rm -f "$OUT/DONE"
pids=()
for i in $(seq 0 $((K - 1))); do
  POLYHOST=linux "$PY" human_starts.py --positions strategy_positions.tsv --raw /home/greff/polydata/replays_raw \
    --data /home/greff/polydata/human_full --out "$OUT" --port $((11000 + i)) --shard "$i/$K" > "$OUT/shard_$i.log" 2>&1 &
  pids+=($!)
done
echo "shard pids: ${pids[*]}" > "$OUT/pids.txt"
for p in "${pids[@]}"; do wait "$p"; done
head -1 "$OUT/index_0.tsv" > "$OUT/index.tsv"
for i in $(seq 0 $((K - 1))); do tail -n +2 "$OUT/index_$i.tsv" >> "$OUT/index.tsv"; done
tail -n 1 "$OUT"/shard_*.log
echo "$(($(wc -l < "$OUT/index.tsv") - 1)) states in $OUT/index.tsv"
touch "$OUT/DONE"
