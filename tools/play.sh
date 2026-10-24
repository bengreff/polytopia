#!/bin/sh
# Play our network in the browser: starts py/agent_server.py with the checkpoint and polyhost's
# web UI, then open http://localhost:8123 and pick "Our network" as the bot difficulty.
#   tools/play.sh <checkpoint.pt> [web port, default 8123]
cd "$(dirname "$0")/.."
ck=${1:?checkpoint}; port=${2:-8123}
[ -x build/polyhost-mac ] || ./polyhost/run.sh mac
.venv/bin/python py/agent_server.py "$ck" --port 8131 &
agent=$!
trap 'kill $agent 2>/dev/null' EXIT INT TERM
sleep 3
echo "open http://localhost:$port and choose Bot difficulty: Our network"
POLY_AGENT_PORT=8131 POLY_AGENT_NAME=$(basename "$ck") build/polyhost-mac serve "$port"
