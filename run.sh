#!/usr/bin/env bash
# Build and launch the game. Open http://localhost:8080
set -e
export PATH="/opt/homebrew/bin:$PATH"
cmake -S . -B build -G Ninja > /dev/null
cmake --build build
# The server holds ONE long-lived poly-cli child, so a rebuild has no effect until it is
# restarted. That cost a debugging cycle; kill any stale server rather than leave the trap.
pkill -f "web/server.js" 2>/dev/null || true
sleep 0.3

echo
echo "  Polytopia engine ready -> http://localhost:8080"
echo
exec node web/server.js "$@"
