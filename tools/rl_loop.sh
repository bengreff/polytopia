#!/bin/sh
# Policy-only RL iterations (docs/design/iteration-plan.md): the Mac plays games with the current
# policy (py/dagger.py), the PC takes one PPO step on them (py/train_rl.py), the new policy comes
# back, and it is evaluated on the fixed seeds. Each iteration continues from the last.
#
#   tools/rl_loop.sh <name> <start.pt on the Mac> <iterations> <games per iteration> <anchor spec on the PC> \
#       [eval difficulties, e.g. "2 3"] [first iteration, default 1]
#
# Env OPPONENTS (default 1,2,2,3,3,self) sets the opponent mix, as py/dagger.py --opponents; TEMP
# (default 1) the sampling temperature, used by both the actors and the learner; LR (default 3e-5)
# and BCW (default 0.5) the learner's learning rate and behaviour-cloning anchor weight; PAIRS a
# py/dagger.py --tribe_pairs file.
# Files: Mac data/rl/<name>/iNN (games), runs/rl/<name>/iNN.pt (policies), runs/evals/<name>_iNN_d*.
# PC: ~/polydata/rl/<name>/iNN, ~/runs/rl/<name>/iNN (train_rl output). Code: ~/poly/py_rl.
set -e
cd /Users/ben/polytopia
name=$1; start=$2; iters=$3; games=$4; anchor=$5; evals=${6:-"2 3"}; first=${7:-1}
mkdir -p data/rl/$name runs/rl/$name

# The link to the PC drops now and then: each transfer is a function, retried 6 times 30 s apart.
retry() { n=0; until "$@"; do n=$((n + 1)); [ $n -ge 6 ] && return 1; echo "retrying $1 ($n)"; sleep 30; done; }
pc() { ssh -o ConnectTimeout=30 backhouse "wsl -d Ubuntu-22.04 -- $1"; }
push_code() { tar cf - -C py model.py polyenv.py train_bc.py train_rl.py 2>/dev/null | pc "bash -c \"mkdir -p ~/poly/py_rl && tar xf - -C ~/poly/py_rl\""; }
push_games() { (cd data/rl/$name/$it && COPYFILE_DISABLE=1 tar --no-xattrs -cf - . 2>/dev/null) | pc "bash -c \"mkdir -p ~/polydata/rl/$name/$it && tar xf - -C ~/polydata/rl/$name/$it\""; }
push_init() { pc "bash -c \"mkdir -p ~/runs/rl/$name && cat > ~/runs/rl/$name/$it.init.pt\"" < $cur; }
launch() { pc "bash -s" < runs/rl/$name/$it.launch.sh; }
fetch_policy() { pc "cat /home/greff/runs/rl/$name/$it/policy.pt" > runs/rl/$name/$it.pt.tmp; }
fetch_log() { pc "cat /home/greff/runs/rl/$name/$it/log.jsonl" > runs/rl/$name/$it.learn.jsonl; }

retry push_code 2>/dev/null
cur=$start
i=$first
while [ $i -lt $((first + iters)) ]; do
    it=$(printf 'i%02d' $i)
    seed=$((30000000 + i * 100000))
    echo "$(date '+%H:%M:%S') $name $it: playing $games games with $cur"
    POLYHOST=mac .venv/bin/python py/dagger.py $cur data/rl/$name/$it --games $games --first_seed $seed \
        --opponents ${OPPONENTS:-1,2,2,3,3,self} ${PAIRS:+--tribe_pairs $PAIRS} --procs 6 --slots 12 --port 8900 --temperature ${TEMP:-1} > runs/rl/$name/$it.play.log 2>&1
    tail -2 runs/rl/$name/$it.play.log
    retry push_games 2>/dev/null
    retry push_init
    echo "$(date '+%H:%M:%S') $name $it: learning"
    cat > runs/rl/$name/$it.launch.sh <<EOF
cd /home/greff/poly/py_rl
if [ ! -f /home/greff/runs/rl/$name/$it/policy.pt ] && ! pgrep -f "[r]l/$name/$it --bc" > /dev/null; then
    rm -rf /home/greff/runs/rl/$name/$it
    setsid nohup /home/greff/polyvenv/bin/python train_rl.py /home/greff/runs/rl/$name/$it.init.pt /home/greff/polydata/rl/$name/$it \
        /home/greff/runs/rl/$name/$it --bc $anchor --temperature ${TEMP:-1} --lr ${LR:-3e-5} --bc_weight ${BCW:-0.5} > /home/greff/runs/rl/$name/$it.log 2>&1 < /dev/null &
fi
sleep 5  # without this, WSL can end the session before the background job has started
EOF
    retry launch
    waited=0
    until pc "test -f /home/greff/runs/rl/$name/$it/policy.pt" 2>/dev/null; do
        if pc "bash -c \"grep -q Traceback /home/greff/runs/rl/$name/$it.log\"" 2>/dev/null; then
            echo "learner failed:"; pc "tail -20 /home/greff/runs/rl/$name/$it.log"; exit 1
        fi
        waited=$((waited + 30))
        if [ $waited -ge 3600 ]; then echo "learner produced nothing in an hour"; exit 1; fi
        sleep 30
    done
    sleep 5
    retry fetch_policy
    mv runs/rl/$name/$it.pt.tmp runs/rl/$name/$it.pt
    retry fetch_log
    tail -1 runs/rl/$name/$it.learn.jsonl
    cur=runs/rl/$name/$it.pt
    if [ -n "$evals" ]; then
        tools/eval.sh $cur ${name}_$it $evals
        grep "${name}_$it" runs/evals/summary.tsv | tail -3
    fi
    i=$((i + 1))
done
echo "$(date '+%H:%M:%S') $name: done"
