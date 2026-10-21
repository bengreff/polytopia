#!/bin/sh
# Build polyhost with the Android NDK, push it and the game files to the emulator, run it.
#   ./polyhost/run.sh push            copy the game's binary, metadata and data (once)
#   ./polyhost/run.sh build           build and push polyhost only (py/polyenv.py launches it)
#   ./polyhost/run.sh <polyhost args> build, push polyhost and its web UI, run it
#   ./polyhost/run.sh serve           then open http://localhost:8123 (port is forwarded)
set -e
cd "$(dirname "$0")/.."
GAME=game/android
NDK=$(ls -d game/tools/android-ndk-r* | head -1)
CXX=$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android30-clang++
ADB=${ADB:-$HOME/Library/Android/sdk/platform-tools/adb}
DEV=/data/local/tmp/poly

if [ "$1" = push ]; then
    $ADB shell mkdir -p $DEV/Managed/Metadata $DEV/gamedata
    $ADB push $GAME/arm64/lib/arm64-v8a/libil2cpp.so $DEV/
    $ADB push $GAME/base/assets/bin/Data/Managed/Metadata/global-metadata.dat $DEV/Managed/Metadata/
    $ADB push game/dump/gamedata/. $DEV/gamedata/
    exit 0
fi

mkdir -p build
$CXX -std=c++20 -O2 -Wall -static-libstdc++ -o build/polyhost polyhost/main.cpp -ldl
$ADB push -q build/polyhost $DEV/ >/dev/null
$ADB push -q polyhost/web/. $DEV/web/ >/dev/null
[ "$1" = build ] && exit 0
[ "$1" = serve ] && $ADB forward tcp:${2:-8123} tcp:${2:-8123} >/dev/null
$ADB shell "cd $DEV && ./polyhost $*"
