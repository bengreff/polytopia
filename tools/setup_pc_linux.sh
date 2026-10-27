#!/bin/sh
# Sets up the PC (ssh backhouse, WSL) to run the game natively: polyhost built for x86-64 Linux
# against the Steam Linux build's GameAssembly.so (depot 874393, game build 2.17.3.16375, the same
# build as the Android oracle and the Mac's GameAssembly.dylib). No emulation: ~1 ms per decision for
# the legal list instead of ~22 ms under QEMU. py/polyenv.py launches it with POLYHOST=linux.
#
# The depot was downloaded on the Mac (Steam console: download_depot 874390 874393). Only
# GameAssembly.so and Polytopia_Data/il2cpp_data are copied (~125 MB, never committed), with the
# rules data (game/dump/gamedata) and polyhost's source, which is compiled on the PC: with g++ if
# WSL has it, else with Zig's C++ compiler from the PC's Python environment (WSL has no internet).
#
#   tools/setup_pc_linux.sh            copy everything and build ~/polylinux/polyhost
#   tools/setup_pc_linux.sh build      copy the source and rebuild only
set -e
cd "$(dirname "$0")/.."
DEPOT=${DEPOT:-"$HOME/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/steamapps/content/app_874390/depot_874393"}
kit=$(mktemp -d)
mkdir -p $kit/src $kit/gamedata
cp polyhost/*.hpp polyhost/main.cpp $kit/src/
if [ "$1" != build ]; then
    mkdir -p $kit/game/Polytopia_Data
    cp "$DEPOT/GameAssembly.so" $kit/game/
    cp -R "$DEPOT/Polytopia_Data/il2cpp_data" $kit/game/Polytopia_Data/
    cp -R game/dump/gamedata/. $kit/gamedata/
fi
(cd $kit && COPYFILE_DISABLE=1 tar --no-xattrs -czf - .) | ssh -o ConnectTimeout=30 backhouse \
    'wsl -d Ubuntu-22.04 -- bash -c "mkdir -p ~/polylinux && tar xzf - -C ~/polylinux && echo copied"'
rm -rf $kit
ssh -o ConnectTimeout=30 backhouse 'wsl -d Ubuntu-22.04 -- bash -s' <<'EOF'
cd ~/polylinux
if command -v g++ > /dev/null; then CXX="g++"; else CXX="$HOME/polyvenv/bin/python -m ziglang c++ -target x86_64-linux-gnu.2.35"; fi
echo "compiler: $CXX"
# a new name first: overwriting a running binary can kill its games
$CXX -std=c++20 -O2 -Wall -o polyhost.new src/main.cpp -ldl -lpthread && mv polyhost.new polyhost && chmod +x polyhost
ls -la polyhost
EOF
