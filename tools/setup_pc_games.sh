#!/bin/sh
# Sets up the PC (ssh backhouse, WSL) to play games without Polytopia installed there: the Android
# build's own libil2cpp.so (the oracle binary) runs under QEMU user-mode emulation (qemu-aarch64,
# static, from Debian's qemu-user 10.0.13), with the Android 14 runtime (linker64, libc, libm, libdl,
# liblog, libc++) taken from the PolyRef emulator image. py/polyenv.py launches it with POLYHOST=qemu.
# The PC's WSL has no internet of its own, so everything is copied from the Mac (game/pc holds the
# QEMU binary and the runtime; game/ is never committed).
#
#   tools/setup_pc_games.sh           build polyhost for Android, copy the kit to ~/polyarm on the PC
#   tools/setup_pc_games.sh runtime   first pull the Android runtime from the running PolyRef emulator
#
# Speed: ~22 ms per decision for the legal list under QEMU (0.9 ms natively on the Mac); the same
# games, legal lists, observations and final states as the Mac (py/parity_hosts.py).
set -e
cd "$(dirname "$0")/.."
ADB=${ADB:-$HOME/Library/Android/sdk/platform-tools/adb}
if [ "$1" = runtime ]; then
    mkdir -p game/pc/root/system/bin game/pc/root/system/lib64
    $ADB pull /apex/com.android.runtime/bin/linker64 game/pc/root/system/bin/linker64
    for l in libc.so libm.so libdl.so libdl_android.so; do
        $ADB pull /apex/com.android.runtime/lib64/bionic/$l game/pc/root/system/lib64/$l
    done
    for l in liblog.so ld-android.so libc++.so; do $ADB pull /system/lib64/$l game/pc/root/system/lib64/$l; done
fi
NDK=$(ls -d game/tools/android-ndk-r* | head -1)
$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android30-clang++ -std=c++20 -O2 -Wall -static-libstdc++ \
    -o build/polyhost-android polyhost/main.cpp -ldl
kit=$(mktemp -d)
mkdir -p $kit/poly/Managed/Metadata $kit/poly/gamedata
cp -R game/pc/root $kit/root
cp game/pc/qemu-aarch64 $kit/
cp build/polyhost-android $kit/poly/polyhost.new
cp game/android/arm64/lib/arm64-v8a/libil2cpp.so $kit/poly/
cp game/android/base/assets/bin/Data/Managed/Metadata/global-metadata.dat $kit/poly/Managed/Metadata/
cp -R game/dump/gamedata/. $kit/poly/gamedata/
# polyhost goes in under a new name and is renamed: overwriting a running binary can kill its games
(cd $kit && COPYFILE_DISABLE=1 tar --no-xattrs -czf - .) | ssh -o ConnectTimeout=30 backhouse \
    'wsl -d Ubuntu-22.04 -- bash -c "mkdir -p ~/polyarm && tar xzf - -C ~/polyarm && cd ~/polyarm/poly && mv polyhost.new polyhost && chmod +x polyhost ../qemu-aarch64 && echo installed"'
rm -rf $kit
