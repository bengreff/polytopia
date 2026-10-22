#!/bin/sh
# Waits for a checkpoint on the PC (WSL path), then copies it to the Mac.
# usage: tools/fetch_ckpt.sh <remote path> <local path>
remote=$1; local=$2
until ssh -o ConnectTimeout=20 backhouse "wsl -d Ubuntu-22.04 -- test -f $remote" 2>/dev/null; do sleep 60; done
sleep 20  # let the save finish
mkdir -p "$(dirname "$local")"
ssh backhouse "wsl -d Ubuntu-22.04 -- cat $remote" > "$local.tmp" && mv "$local.tmp" "$local"
