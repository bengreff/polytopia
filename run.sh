#!/usr/bin/env bash
# Build and launch the game. Open http://localhost:8080
set -e
export PATH="/opt/homebrew/bin:$PATH"
cmake -S . -B build -G Ninja > /dev/null
cmake --build build
echo
echo "  Polytopia engine ready -> http://localhost:8080"
echo
exec node web/server.js "$@"
