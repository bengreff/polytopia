#!/bin/bash
# HP1 arm B data: download the Might Continents Small games listed in data/replays/cm_ids_cont.tsv
# (py/cm_replays.py --format 1,2,1 --tag _cont), newest first, and convert them with the final-state
# check into data/human_cont, then copy them to the PC (~/polydata/human_cont).
#   TOKEN_FILE=<the game client's saved jwtToken file> tools/hp_continents.sh
# Needs a fresh game login (the token expires). Polite: 5 workers, ~4 s between one worker's requests.
# Converter: 2 game processes on ports 8960-8961 (Mac rules). Resumable: rerun it.
set -u
cd "$(dirname "$0")/.."
: "${TOKEN_FILE:?set TOKEN_FILE to the token file of the game client}"
LOG=runs/agents/hp_continents.log
OK_ROW=$(printf '\tok\t')
mkdir -p data/replays/raw_cont data/human_cont runs/agents
echo "$(date '+%F %T') start" >> $LOG
.venv/bin/python -u py/fetch_replays.py --token-file "$TOKEN_FILE" --ids data/replays/cm_ids_cont.tsv \
  --out data/replays/raw_cont --workers 5 --delay 4 --newest-first >> $LOG 2>&1 &
FETCH=$!
convert() {  # converts the games downloaded so far (games already in games.tsv are skipped)
  local pids=""
  for i in 0 1; do
    POLYHOST=mac .venv/bin/python -u py/convert_replays.py --raw data/replays/raw_cont --out data/human_cont \
      --port 896$i --shard $i/2 --preset 3 >> runs/agents/hp_convert$i.log 2>&1 &
    pids="$pids $!"
  done
  wait $pids
}
while kill -0 $FETCH 2>/dev/null; do
  sleep 600
  convert
  echo "$(date '+%F %T') raw $(ls data/replays/raw_cont | grep -c json) converted $(grep -c "$OK_ROW" data/human_cont/games.tsv)" >> $LOG
done
convert
echo "$(date '+%F %T') fetch done; raw $(ls data/replays/raw_cont | grep -c json) converted $(grep -c "$OK_ROW" data/human_cont/games.tsv)" >> $LOG
# copy to the PC (retry: the link drops now and then)
for try in 1 2 3 4 5; do
  (cd data/human_cont && COPYFILE_DISABLE=1 tar cf - --exclude='*.tmp.npz' . | gzip -1 |
    ssh -o ConnectTimeout=20 backhouse 'wsl -d Ubuntu-22.04 -- bash -c "mkdir -p ~/polydata/human_cont && tar xzf - -C ~/polydata/human_cont"') && break
  sleep 30
done
echo "$(date '+%F %T') copied to the PC" >> $LOG
