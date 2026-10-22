#!/bin/sh
# Policy-only RL iterations (docs/design/iteration-plan.md): the Mac plays games with the current
# policy (py/dagger.py, temperature 1), the PC takes one PPO step on them (py/train_rl.py), the new
# policy comes back, and it is evaluated on the fixed seeds. Each iteration continues from the last.
#
#   tools/rl_loop.sh <name> <start.pt on the Mac> <iterations> <games per iteration> <anchor spec on the PC> \
#       [eval difficulties, e.g. "2 3"] [first iteration, default 1]
#
# Files: Mac data/rl/<name>/iNN (games), runs/rl/<name>/iNN.pt (policies), runs/evals/<name>_iNN_d*.
# PC: ~/polydata/rl/<name>/iNN, ~/runs/rl/<name>/iNN (train_rl output). Code: ~/poly/py_rl.
set -e
cd /Users/ben/polytopia
name=$1; start=$2; iters=$3; games=$4; anchor=$5; evals=${6:-"2 3"}; first=${7:-1}
mkdir -p data/rl/$name runs/rl/$name
tar cf - -C py model.py polyenv.py train_bc.py train_rl.py | ssh backhouse "wsl -d Ubuntu-22.04 -- bash -c \"mkdir -p ~/poly/py_rl && tar xf - -C ~/poly/py_rl\"" 2>/dev/null
cur=$start
i=$first
while [ $i -lt $((first + iters)) ]; do
    it=$(printf 'i%02d' $i)
    seed=$((30000000 + i * 100000))
    echo "$(date '+%H:%M:%S') $name $it: playing $games games with $cur"
    POLYHOST=mac .venv/bin/python py/dagger.py $cur data/rl/$name/$it --games $games --first_seed $seed \
        --opponents 1,2,2,3,3,self --procs 6 --slots 6 --port 8900 --temperature 1 > runs/rl/$name/$it.play.log 2>&1
    tail -2 runs/rl/$name/$it.play.log
    (cd data/rl/$name/$it && COPYFILE_DISABLE=1 tar --no-xattrs -cf - .) |
        ssh backhouse "wsl -d Ubuntu-22.04 -- bash -c \"mkdir -p ~/polydata/rl/$name/$it && tar xf - -C ~/polydata/rl/$name/$it\"" 2>/dev/null
    cat $cur | ssh backhouse "wsl -d Ubuntu-22.04 -- bash -c \"mkdir -p ~/runs/rl/$name && cat > ~/runs/rl/$name/$it.init.pt\""
    echo "$(date '+%H:%M:%S') $name $it: learning"
    ssh backhouse "wsl -d Ubuntu-22.04 -- bash -s" <<EOF
cd /home/greff/poly/py_rl
rm -rf /home/greff/runs/rl/$name/$it
setsid nohup /home/greff/polyvenv/bin/python train_rl.py /home/greff/runs/rl/$name/$it.init.pt /home/greff/polydata/rl/$name/$it \
    /home/greff/runs/rl/$name/$it --bc $anchor > /home/greff/runs/rl/$name/$it.log 2>&1 < /dev/null &
EOF
    until ssh -o ConnectTimeout=20 backhouse "wsl -d Ubuntu-22.04 -- test -f /home/greff/runs/rl/$name/$it/policy.pt" 2>/dev/null; do
        if ssh -o ConnectTimeout=20 backhouse "wsl -d Ubuntu-22.04 -- bash -c \"grep -q Traceback /home/greff/runs/rl/$name/$it.log\"" 2>/dev/null; then
            echo "learner failed:"; ssh backhouse "wsl -d Ubuntu-22.04 -- tail -20 /home/greff/runs/rl/$name/$it.log"; exit 1
        fi
        sleep 30
    done
    sleep 5
    ssh backhouse "wsl -d Ubuntu-22.04 -- cat /home/greff/runs/rl/$name/$it/policy.pt" > runs/rl/$name/$it.pt.tmp && mv runs/rl/$name/$it.pt.tmp runs/rl/$name/$it.pt
    ssh backhouse "wsl -d Ubuntu-22.04 -- cat /home/greff/runs/rl/$name/$it/log.jsonl" > runs/rl/$name/$it.learn.jsonl
    tail -1 runs/rl/$name/$it.learn.jsonl
    cur=runs/rl/$name/$it.pt
    if [ -n "$evals" ]; then
        tools/eval.sh $cur ${name}_$it $evals
        grep "${name}_$it" runs/evals/summary.tsv | tail -3
    fi
    i=$((i + 1))
done
echo "$(date '+%H:%M:%S') $name: done"
