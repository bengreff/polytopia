#!/bin/sh
# Decompile methods of the game's libil2cpp.so by name, e.g.
#   tools/ghidra/decompile.sh 'GameRules$$LoadPreset/1' 'GameStateUtils$$CreateLocalGame/4'
# Output goes to game/dump/decompiled/<first name>.c and is printed.
set -e
cd "$(dirname "$0")/../.."
export JAVA_HOME=${JAVA_HOME:-/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home}
GHIDRA=/opt/homebrew/opt/ghidra/libexec/support/analyzeHeadless
PROJ=game/ghidra
BIN=game/android/arm64/lib/arm64-v8a/libil2cpp.so
mkdir -p $PROJ game/dump/decompiled
OUT=game/dump/decompiled/$(echo "$1" | tr -c 'A-Za-z0-9_.\n' '_').c
if [ ! -d $PROJ/poly.rep ]; then
    $GHIDRA $PROJ poly -import $BIN -noanalysis -scriptPath tools/ghidra \
        -postScript Decompile.java "$PWD/game/dump/symbols.tsv" "$PWD/$OUT" "$@" > game/ghidra/import.log 2>&1
else
    $GHIDRA $PROJ poly -process libil2cpp.so -noanalysis -scriptPath tools/ghidra \
        -postScript Decompile.java "$PWD/game/dump/symbols.tsv" "$PWD/$OUT" "$@" > game/ghidra/run.log 2>&1
fi
python3 tools/ghidra/names.py game/dump/symbols.tsv < "$OUT" > "$OUT.named" && mv "$OUT.named" "$OUT"
cat "$OUT"
