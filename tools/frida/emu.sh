#!/bin/sh
# Small helpers for driving the reference emulator.
#   emu.sh shot <out.png>      screenshot, scaled to 540 px wide
#   emu.sh tap <x> <y>         tap in full-resolution (1080x2400) coordinates
ADB=${ADB:-$HOME/Library/Android/sdk/platform-tools/adb}
case "$1" in
  shot) $ADB exec-out screencap -p > "$2.full.png" && sips -Z 1200 "$2.full.png" --out "$2" >/dev/null && rm "$2.full.png" ;;
  tap) $ADB shell input tap "$2" "$3" ;;
esac
