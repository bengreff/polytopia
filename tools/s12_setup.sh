#!/bin/bash
# Research log S12 (Ben, 2026-11-18: "Do 1+2+3+4+5+6+7. Go."): from s11x gen 107, on the PC.
# - s12, the main track: planner v2 on every learner turn (search_share 1.0) with exploration lanes
#   (4 lanes sampled hotter from the learner, 4 from gen 0, the human-trained network: planner.json), and
#   the network trained to copy the planner's choices (train_rl --search_ce 1.0, early stop on the exact KL,
#   --kl_exact); capped games count as a loss for both (--cap_loss) and a win at turn T is worth
#   max(0.5, 1 - 0.005 T) (turn_cost); 15% of the training games against the exploiter's 2 latest generations.
# - e12, its exploiter: plays only s12's latest generation, with the same planner (a search-based
#   exploiter), 500 games a generation, no anchor, KL limit 0.1; resets to s12's latest generation after
#   4 generations, or once it beats it by 65%; evaluated against s12's same generation (its "rival").
# - s11x107 is the frozen yardstick: generations 0..107 are s11x's, every later one is s11x gen 107, so each
#   s12 generation is evaluated against S12's start (sp_loop --rival).
# Both tracks link s11x's generations 0..107, so they run in step in the rotation (tracks.sh runs the track
# with the fewest generations, s12 first on a tie): s12 gen g, then e12 gen g against it, then s12 gen g+1.
# Training starts: human_starts_big until the strategy starts (/home/greff/polydata/strategy_starts) are
# built, then those (switched in override.json, pre-registered in the research log).
# Run with the rotation stopped (no tracks.sh running), from ~/poly/py_sp with the S12 code in place:
#
#   bash ~/poly/py_sp/s12_setup.sh
#   then start the rotation: the Windows task (Start-ScheduledTask PolytopiaTrainingStarter), or
#   DELAY=0 bash ~/poly/py_sp/sp_resume.sh
set -eu
R=/home/greff/runs/sp
D=$R/s11x
B=107
M=s12
E=e12
F=s11x107
R7=/home/greff/runs/r7_human_full/step020000.pt
STARTS=/home/greff/polydata/human_starts_big
BIN=/home/greff/polylinux/polyhost-cp
for x in $M $E $F; do [ -e $R/$x ] && { echo "$R/$x exists: not touched"; exit 1; }; done
[ -e $D/gen$B.pt ] && [ -e $D/critic_g$B.pt ] || { echo "no $D/gen$B.pt or its critic"; exit 1; }
mkdir -p $R/$M/evals $R/$E/evals $R/$F
for g in $(seq 0 $B); do
    n=$(printf "gen%03d.pt" $g)
    for x in $M $E $F; do ln -s "$(readlink -f $D/$n)" $R/$x/$n; done
done
for g in $(seq $((B + 1)) $((B + 40))); do
    ln -s "$(readlink -f $D/gen$B.pt)" $R/$F/$(printf "gen%03d.pt" $g)
done
cp $D/critic_g$B.pt $R/$M/
cp $D/critic_g$B.pt $R/$E/
for g in $(seq $((B - 2)) $B); do cp $D/evals/*_g$(printf %03d $g).json $R/$M/evals/ 2>/dev/null || true; done
rm -f $R/$M/evals/rival_g*.json $R/$M/evals/hrival_g*.json
for x in $M $E; do
    cat > $R/$x/planner.json <<JSON
{"hot_lanes": 4, "hot_temp": 1.0, "prior_lanes": 4, "prior_temp": 0.5, "prior": "$R7"}
JSON
done
cat > $R/$M/override.json <<JSON
{"procs": 28, "eval_procs": 8, "min_procs": 8, "crazy_every": 3, "h_seeds": 100, "shards": 3, "critic": true,
 "script_eval": "crazyplus", "starts": "$STARTS", "start_share": 0.3, "eval_starts": "$STARTS", "port": 10500,
 "learn_extra": "--gae_lambda 0.97 --entropy 0 --anchor $R7 --anchor_weight 0.03 --search_adv 1.0 --search_ce 1.0 --kl_exact --cap_loss",
 "turn_cost": 0.005, "target_kl": 0.05,
 "rival": "$R/$F", "search_share": 1.0, "search_bin": "$BIN",
 "search_extra": "--search_plans 16 --search_samples 2 --search_v2 $R/$M/planner.json",
 "league": "$R/$E@2:0.15"}
JSON
cat > $R/$E/override.json <<JSON
{"procs": 28, "eval_procs": 8, "min_procs": 8, "shards": 3, "critic": true, "games": 500, "port": 10700,
 "self_share": 0.0, "fixed": "", "recent": 0, "league": "$R/$M@1:1.0",
 "starts": "$STARTS", "start_share": 0.3,
 "learn_extra": "--gae_lambda 0.97 --entropy 0 --search_adv 1.0 --search_ce 1.0 --kl_exact --cap_loss",
 "turn_cost": 0.005, "target_kl": 0.1,
 "rival": "$R/$M", "eval_kinds": "rival", "indicators": 0,
 "reset_from": "$R/$M", "reset_rate": 0.65, "reset_every": 4, "branch_gen": $B,
 "search_share": 1.0, "search_bin": "$BIN",
 "search_extra": "--search_plans 16 --search_samples 2 --search_v2 $R/$E/planner.json"}
JSON
for x in $M $E; do
    python3 -c "import json; json.load(open('$R/$x/override.json')); json.load(open('$R/$x/planner.json'))"
    echo $((B + 12)) > $R/$x/LAST
done
printf '%s\n%s\n' $M $E > $R/tracks.list
echo "$M, $E: $(ls $R/$M | wc -l), $(ls $R/$E | wc -l) entries, LAST $(cat $R/$M/LAST); $F: $(ls $R/$F | wc -l) generations; tracks.list: $(tr '\n' ' ' < $R/tracks.list)"
