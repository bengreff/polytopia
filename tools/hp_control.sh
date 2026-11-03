#!/bin/bash
# HP1 tests T2 (controllability) and T4 (vs Crazy at z = 0) for one z checkpoint.
# All sets use the same standard seeds (10,000,000+), sides swapped; only the z differs. Each game's z is
# drawn from all human games (training and held-out) of the agent's tribe that pass the set's filter.
#   tools/hp_control.sh <checkpoint.pt> <out_dir> [seeds_per_set=50] [base_seeds=100]
# Then: python3 tools/z_control.py <out_dir>
# Mac (default): 2 game processes on ports 8980-8981, data/human1, .venv.
# PC: HOSTKIND=linux PY=~/polyvenv/bin/python H=~/polydata/human_full PORT=10010 DEVICE=cuda, run from a
# copy of the repo's py/ (CODE=~/poly/hp).
set -u
CK=$1; OUT=$2; N=${3:-50}; NB=${4:-100}
HOSTKIND=${HOSTKIND:-mac}; PORT=${PORT:-8980}; PROCS=${PROCS:-2}
REPO="$(cd "$(dirname "$0")/.." && pwd)"
CODE=${CODE:-$REPO/py}; PY=${PY:-$REPO/.venv/bin/python}; H=${H:-$REPO/data/human1}
DEV=${DEVICE:+--device $DEVICE}
mkdir -p "$OUT"
run() {  # name seeds [z options...]
  local name=$1 seeds=$2; shift 2
  [ -s "$OUT/$name.done" ] && return
  rm -f "$OUT/$name.jsonl"
  (cd "$CODE" && POLYHOST=$HOSTKIND nice -n 5 "$PY" eval_vs_bot.py "$CK" --seeds "$seeds" --difficulty 3 --procs "$PROCS" \
    --slots 8 --port "$PORT" $DEV --facts_out "$OUT/$name.jsonl" "$@") > "$OUT/$name.log" 2>&1 &&
    tail -4 "$OUT/$name.log" > "$OUT/$name.done"
}
run base "$NB"
run fast "$N" --z_from "$H" --z_split all --z_filter "techs_t20 >= 9 and T >= 20"
run slow "$N" --z_from "$H" --z_split all --z_filter "techs_t20 <= 5 and T >= 20"
run nocat "$N" --z_from "$H" --z_split all --z_filter "catapult_t20 == 0 and T >= 20"
run manycat "$N" --z_from "$H" --z_split all --z_filter "catapult_t20 >= 3"
run nonavy "$N" --z_from "$H" --z_split all --z_filter "has_port == 0 and has_naval == 0 and T >= 20"
run earlyport "$N" --z_from "$H" --z_split all --z_filter "has_port == 1 and port_turn <= 8"
"$PY" "$REPO/tools/z_control.py" "$OUT" > "$OUT/summary.txt" 2>&1 || python3 "$CODE/z_control.py" "$OUT" > "$OUT/summary.txt" 2>&1
cat "$OUT/summary.txt"
