# Self-play throughput with planner v2 in the loop (2026-11-18)

Goal: more training games per hour with planner v2 on every learner turn (S12: `sp_loop --search_share
1.0`), without changing what gets played. Every number is labelled: **measured** (on the PC, this day),
**derived** (arithmetic on measured numbers) or **guessed**.

**Verdict:** the inference-loop change is exact (same outputs bit for bit, same plans on 60 real states) and
gave 1.11x-1.19x decisions/s in self-play (measured, two pairs of runs), about a third lower request latency
for one planner (23.9 -> 16.4 ms, mean of two rounds), and about 0.8 GB less GPU memory per shard. Duplicate-lane sharing is exact on the same 60
states but showed no throughput gain in self-play, so it was not merged (see 2). `--groups 6` is slower and changes play.
The real bottleneck is the one inference thread per shard (74-91% busy), plus a wall-clock `budget` that
makes the plans depend on how loaded the machine is.

## Conditions

- No number here is from an idle PC or GPU (measured):
  - 17:25-19:33, the VM was pinned to the 12 E-cores at BelowNormal (Roblox open, then the 17:00-22:00 cap);
  - the GPU was always shared with the `hp1d` train_bc job (55-90% utilisation);
  - until about 18:30 it was also shared with the S11 read's planner match (8 game processes, 8 GPU
    workers) and another session's `s12test` self-play (3 game processes);
  - load average 43-50 on 12 cores during the profile (17:32-17:45, CPU idle 0.2%), about 30 at 18:10,
    about 20 at 18:50, and about 9-10 from 19:20 (quietest: the last base run, 19:23-19:33);
  - S12 had not started: no number includes its load.
- At about 17:52 one of my harness runs found CUDA unavailable at start and ran on the CPU (I killed it).
  The coordinator reports a planner-match worker crashing at 18:00 with "CUDA error: unspecified launch
  failure", with my runs, hp1d and the read all on the GPU. That was a sign of GPU trouble around then.
- Test setup, as `s11x` except the search share: gen 107 and `critic_g107`, pool self 0.5 plus gen 0 and
  gens 100-106, 30% human starts, temperature 0.5, `--search_v2 {}`, `polyhost-cp`, share **1.0**.
  8 game processes x 6 slots, one shard (production: 28 or 14 processes, 3 shards).
- Tools (scratch, on the PC in `~/poly/py_fast`; outputs in `~/runs/fast`):
  - `prof_sp.py` wraps selfplay and times every engine request, lock wait, inference request and
    planner phase;
  - `infer_check.py` and `infer_bench.py` test the inference path alone;
  - py-spy (in the venv) for the GIL and the inference thread.

## Where the time goes (baseline, measured)

Profile of the current code, 317 s of self-play (`~/runs/fast/prof_base1`), 426 planned turns:

- **Planned turns:** median 5.3 s, mean 8.9 s, p90 20.2 s. 53 of 426 (12%) went past planner v2's 18 s
  `budget`, after which repairs are skipped.
- **The 16 planner threads' time** (2 thread groups per game process; at share 1.0 a group plans one of
  its slots' turns at a time while the others wait, so the groups are the planners):

  | Waiting for | Share |
  |---|---|
  | inference (queue + GPU) | 41% |
  | engine requests | 32% |
  | the engine lock (the other group on the same game process) | 18% |
  | Python (planner, tactics, parsing) | 9% |

- **The one inference thread is the bottleneck:** busy 76% of wall time. A learner call took 49 ms for
  43 positions, and a planner request waited 168 ms on average. For comparison, a graphed call took
  2.6-3.0 ms for 2-8 positions in October (research log "Self-play speed"). py-spy on that thread (90 s):
  - 31% waiting for the GPU's results (`.cpu()` after each group);
  - about 30% CPU work: `np.stack` in `batch_inputs` 10%, the ten copy and zero ops per graph call 13%,
    and the eager (non-graph) forward for batches over 128 positions about 5%;
  - about 15% sampling and post-processing; 14% idle.
- **Not the GIL:** held 23% of the time (py-spy `--gil`). The Python process used 1.2 cores.
- **The engines were mostly idle:** 32% CPU each.
  - Per planned turn in the first minutes (`prof_base2`): 71 loads (5.6 ms each, client side), 90 saves
    (1.9 ms), 41 step requests (31 ms, about 9 slots each), 40 observe requests (4.8 ms), 2 determinize
    (86 ms).
  - Later in the games (the A/B runs below): 120-190 loads, 130-190 saves, 740-1,040 slot-steps and
    670-960 observations per planned turn, mostly from the tactical searches.
- **Planner phases** (share of planner wall time, `prof_base2`): network lanes 32%, greedy replies 25%,
  threat searches 9%, replay on the second sample 8%, tactical lanes 6%, playing the chosen lane 5%,
  offence search 4%, threat replies 4%, End Turn 4%, samples 3%.
- **One planner alone** (the harness below, one thread): early-game turns take 0.4-1.6 s, against 4-6 s
  inside self-play. Most of a turn's time in self-play is queueing.
- **Duplicate lanes:** on 60 early states, 7.4 of the 16 network lanes are distinct on average
  (temperature 0.5 is sharp). In later turns most of them differ (13-15 lanes part ways per turn on
  several of those states).

## What changed (in the worktree, not deployed)

1. **Inference loop** (`py/selfplay.py` `Inference`, `py/fast_infer.py` `FastPolicy`). On by default; this
   is the speedup:
   - the requesting thread builds its own network inputs (`batch_inputs`), so the one inference thread
     only merges them (`merge_inputs`: the same arrays as `batch_inputs` over all the positions);
   - per batch, every (network, temperature) group is launched on the GPU, then the thread waits once and
     copies the results back (before: one wait per group);
   - a graph's inputs go into one pinned host buffer per graph bucket, allocated once, and travel in one
     copy (before: ten copy and zero ops from pageable memory per call). A first try that pinned fresh
     buffers on every call was slower than the old code (below) and was dropped;
   - a batch over 128 positions runs as graph-sized chunks instead of the slow eager forward;
   - same kernels in the same order and the same random draws, so the same numbers for the same batch.
2. **Duplicate lanes share their engine work** (option `dedup`, off; **not merged**: the planner changed
   the same day (S12's exploration lanes), it showed no gain, and it is kept on the branch
   `worktree-agent-a67fb6d390cd0d5b7` (`py/tactics/planner2.py`) in case it is wanted later):
   - lanes whose commands are identical are the same game state on every sample, so only the first of them
     is replayed on the second sample, ended, replied to and scored in the engine;
   - in `plan_lanes`, the 16 network lanes start as one engine slot and part ways when their moves
     differ, each new group taking a copy of the state at that point;
   - the networks still get one row per lane, in the same order, with the representative's observation
     (the same bytes the lane's own slot would give), and each lane takes its own row's output. If that
     ever differs from its representative's, the lane gets its own copy first.
   - Why off: in later turns the lanes nearly all part ways, and each parting costs a save and a load,
     about what loading every lane cost before. No throughput gain showed in self-play (below), and the
     check on later-turn states was cancelled.
3. **`selfplay --groups G`** (default 2, the old layout): thread groups per game process; with
   `--groups 6` every slot has its own planner thread. Group g's planner slots start at 1000 x (g + 1), as
   before for g = 0 and 1. Measured below: not worth using with one inference thread per shard.
4. **`py/tactics/plan_check.py`** (new): planner v2 on saved states, deterministically (one thread, a fixed
   seed per state, no time budget), with `--compare a.jsonl b.jsonl`.

## Same games? How equivalence was checked

Self-play can't be compared move for move, not even two runs of the old code:
- the game seeds are taken from a shared list by whichever thread asks first;
- the game loop's temperature draws (`torch.multinomial`) come from one GPU random stream, in whatever
  order the batches happen to form;
- a planner's requests are merged with other threads' requests, and the batch size picks the graph bucket,
  which changes the learner's bf16 rounding;
- planner v2's 18 s `budget` is wall-clock time (below).

So each change was checked where the code is deterministic:

- **Inference, request by request** (`infer_check.py`): the same requests through the old and the new
  `Inference`, the same torch seed. First one request at a time, then seven requests merged into one batch
  (learner, critic and an opponent network; temperatures none, 0 and 0.5; forced rows).
  - **43 of 45 output arrays bit-identical** (measured), for the first try and for the final code.
  - The two that differ come from one 150-position batch, which the old code ran eagerly and the new
    code runs as graph chunks: largest logit difference 0.06, value difference 0.012 (bf16 rounding), the
    same choice on 150 of 150 rows.
  - In self-play that batch size only occurs when many threads' requests merge, and then the batch
    composition varies from run to run anyway.
- **Planner v2, turn by turn** (`plan_check.py`): 60 real states saved at the start of planned turns
  (game turns 0-23, 4 of them building repair lanes), old code against new code (measured):
  - the inference change (first try): **60 of 60 identical in every field**: commands played, the
    planner's info, every threat-checked lane's score and every lane's greedy-reply score;
  - the same plus dedup: **60 of 60 identical**; engine loads per turn 84.2 -> 67.1 and saves
    102.3 -> 74.4.
- **Not done** (cancelled when S12 launched): the planner harness on the final inference code, which is
  bit-identical to the first try at the request level above, and on 150 later-turn states (game turn 10
  and up) dumped at `~/runs/fast/states2`. To run it:
  `cd py && POLYHOST=linux POLYHOST_BIN=~/polylinux/polyhost-cp python -m tactics.plan_check --states
  ~/runs/fast/states2 --net ~/runs/sp/s11x/gen107.pt --scorer ~/runs/sp/s11x/critic_g107.pt --out new.jsonl`
  under each code version, then `--compare`. Pass `--opts '{"dedup": true}'` to check dedup.
- **What gets played in self-play depends on speed** (measured): repairs (turns with a kept repair) were
  1.4%, 2.0% and 10.6% of planned turns in the three base runs, rising as the machine emptied. The same
  ordering held across arms with planner speed: `--groups 6` 0.0%, base 1.4-2.0% under load,
  inference change 1.7-4.4%, dedup 4.0-8.1%. Repairs run only if a turn is still under the 18 s
  `budget` when it gets there, and the turns that need them (threats, many units) are the slow ones.
  15-23% of planned turns passed 18 s at load 9-30 (47% with `--groups 6`). So any speedup, or a quieter
  machine, plays more repairs. The planner function itself is unchanged (the harness runs with the
  budget off).

## Games per hour, before and after (measured)

Same seeds (first seed 70,000,000) in every run, 9 minutes each, rates over the steady part (from about
165 s; the first ~2 minutes load networks and capture graphs).
- Games/hour is derived as decisions/s x 3600 / 720 (S11 measured 715-732 decisions per game), for these 8
  game processes. Few games finish in 9 minutes, so they aren't counted directly.
- A faster arm is further into its games at the same clock time, where turns cost more, so the window
  slightly favours the slower arm.

| Run | Time | Load (start -> end) | Decisions/s | Planned turns/s | Games/h (derived) | Engine CPU s / planned turn | GPU MB reserved / peak |
|---|---|---|---|---|---|---|---|
| base 1 | 18:10 | 33 -> 30 | 14.65 | 0.844 | 73 | 2.82 | 2,298 / 2,660 |
| inference change 1 | 18:20 | 30 -> 18 | 17.48 | 0.930 | 87 | 2.87 | 1,524 / 1,926 |
| + dedup 1 | 18:30 | 18 -> 16 | 18.38 | 0.908 | 92 | 2.52 | 1,546 / 1,938 |
| + dedup + `--groups 6` | 18:40 | 16 -> 20 | 16.03 | 0.741 | 80 | 2.62 | 1,526 / 2,044 |
| base 2 | 18:50 | 20 -> 20 | 15.75 | 0.828 | 79 | 3.00 | 2,126 / 2,570 |
| inference change 2 | 19:00 | 20 -> 15 | 17.53 | 0.899 | 88 | 2.69 | 1,526 / 1,926 |
| + dedup 2 | 19:10 | 15 -> 10 | 22.81 | 1.077 | 114 | 2.13 | 1,482 / 1,916 |
| base 3 (also dumped states) | 19:23 | 9 -> 9 | 23.86 | 1.123 | 119 | 1.99 | 2,578 / 3,050 |

Reading (derived from the table):
- **The load moved as much as the changes did**, so only neighbouring runs compare fairly.
- **Inference change against the base run just before it:** 17.48 / 14.65 = **1.19x** (round 1, the load
  falling during the second run, which favours it) and 17.53 / 15.75 = **1.11x** decisions/s (round 2,
  both starting at load 20). Planned turns/s: 1.10x and 1.09x. The two inference runs agree within 0.3%.
- **Dedup:** +5% over the inference change in round 1. In round 2 it ran on an emptier machine, and base 3,
  on a still emptier one, was faster than it. No gain shown.
- **`--groups 6`:** planned turns/s 0.741, the slowest. The inference thread was busy 91% of wall time
  (125 positions per batch). Its median planned turn took 16.1 s (7.0 s in the inference-change runs),
  and 47% of turns passed the budget (repairs 0%), so it also changes play. Rejected.
- **GPU memory per shard:** 1.48-1.55 GB reserved (1.92-2.04 GB peak) with the new code, against
  2.13-2.58 GB (2.57-3.05 GB peak) for the old one: about 0.8 GB less on the means, reserved and peak.
- **Inference benchmark, one planner-shaped request at a time** (`infer_bench.py`, two rounds, mean
  latency): old 27.2 / 20.6 ms; first try (fresh pinned buffers per call) 33.3 / 34.7 ms; final 15.0 /
  17.7 ms. Learner on 16 positions: old 17.2 / 9.5 ms, final 7.6 / 8.8 ms.

**For S12 (derived, rough):** S11 took 15,000-16,000 s per generation's games at share 0.5 (measured).
At share 1.0, roughly twice that (guessed, as the coordinator said). With 1.1-1.2x, about 25,000-27,000 s
instead of about 30,000 s, if production shards behave like this one (guessed). Production shards have
~19 planners each against 16 here, so their inference thread is at least as busy.

## Suggestions

- **Deploy change 1** (copy `py/selfplay.py` and `py/fast_infer.py`; no setting). Exact; also frees about
  0.8 GB of GPU memory per shard.
- **More shards is the next lever, untested** (I could run only one GPU process): the inference thread
  is the bottleneck (74-91% busy), and each shard now takes about 0.8 GB less. For example, 4 new shards
  peak at about 7.8 GB against about 8.3 GB for 3 old ones (derived from the mean peaks), but the gain
  is a guess.
- **Optional trade-off, changes play: `"budget": 1e9`** in `--search_v2` for self-play. Plans then stop
  depending on machine load (repairs every time they're due, as in the harness), at an unmeasured cost
  in time on the slow turns. Ben's call.
- **Not worth it:** `--groups` above 2 with one inference thread per shard; dedup as it stands.
- **Later levers** (not done):
  - a polyhost op that copies one slot into another inside the engine, without serialising, sending
    and re-listing the state: 120-190 loads and 130-190 saves per planned turn later in games (measured);
  - cutting the inference thread's fixed cost per group: each batch carries one small group per opponent
    network (8 in this pool), each paying the full launch and copy cost.
