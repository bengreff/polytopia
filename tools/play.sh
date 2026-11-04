#!/bin/sh
# Play our network in the browser: starts py/agent_server.py with the checkpoint and polyhost's
# web UI, then open http://localhost:8123 and pick "Our network" as the bot difficulty.
#   tools/play.sh <checkpoint.pt> [web port, default 8123]
# With SCORER=<critic.pt> the network plays with the planner (py/plan_agent.py: 16 plans, 2 fog-safe
# samples, the opponent's reply, scored by that critic), the strongest player (research log S9 read):
#   SCORER=runs/sp/s9v/critic_g083.pt tools/play.sh runs/sp/s9v/gen083.pt
cd "$(dirname "$0")/.."
ck=${1:?checkpoint}; port=${2:-8123}
[ -x build/polyhost-mac ] || ./polyhost/run.sh mac
name=$(basename "$ck")
if [ -n "$SCORER" ]; then
    [ -x build/polyhost-mac-play ] || clang++ -std=c++20 -O2 -Wall -o build/polyhost-mac-play polyhost/main.cpp
    PYTHONPATH=py .venv/bin/python py/agent_server.py "$ck" --port 8131 --stats_port 8132 --planner --scorer "$SCORER" \
        --plans ${PLANS:-16} --samples ${SAMPLES:-2} &
    serve="build/polyhost-mac-play"; export POLY_AGENT_STATE=1
    name="$(basename "$(dirname "$ck")")_$name+planner"
else
    .venv/bin/python py/agent_server.py "$ck" --port 8131 --stats_port 8132 &
    serve="build/polyhost-mac"
fi
agent=$!
trap 'kill $agent 2>/dev/null' EXIT INT TERM
sleep 3
echo "open http://localhost:$port and choose Bot difficulty: Our network"
POLY_AGENT_PORT=8131 POLY_AGENT_STATS=8132 POLY_AGENT_NAME=$name $serve serve "$port"
