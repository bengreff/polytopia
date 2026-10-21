#!/bin/sh
# Copies finished game files from the Mac to the training PC (WSL on `ssh backhouse`), every few
# minutes, remembers what was sent, and deletes the local copy once the PC has it (games can be
# regenerated from their seeds). Set KEEP=1 to keep local copies.
#   py/sync_data.sh data/m1 polydata/m1 [interval seconds]
SRC=$1
DST=$2
EVERY=${3:-300}
SENT=$SRC/.synced
touch "$SENT"
while true; do
    ls "$SRC" | grep -E '^g[0-9]+\.npz$' | sort > "$SRC/.all"  # finished games only (.tmp.npz excluded)
    comm -23 "$SRC/.all" "$SENT" > "$SRC/.todo"
    if [ -s "$SRC/.todo" ]; then
        if (cd "$SRC" && COPYFILE_DISABLE=1 tar --no-xattrs -cf - -T .todo games.tsv) |
            ssh backhouse "wsl -d Ubuntu-22.04 -- bash -c \"mkdir -p ~/$DST && tar xf - -C ~/$DST\""; then
            sort -m "$SENT" "$SRC/.todo" > "$SENT.new" && mv "$SENT.new" "$SENT"
            [ "$KEEP" = 1 ] || (cd "$SRC" && xargs rm -f < .todo)
            echo "$(date '+%H:%M:%S') sent $(wc -l < "$SRC/.todo") games, $(wc -l < "$SENT") total"
        fi
    fi
    sleep "$EVERY"
done
