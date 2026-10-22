# Iteration plan: from behaviour cloning to beating Crazy (2026-10-21, night 1)

A working plan for improving the network in measured steps, starting the night the human-replay
download finished. Runs are pre-registered in [research-log.md](research-log.md). The status log at
the bottom is updated as results come in.

## Where we are (21:30)

| Player on our seat | vs Easy | vs Normal | vs Hard | vs Crazy |
|---|---|---|---|---|
| Teacher (the game's AI, person seat) | 96.5% | 65.0% | 31.0% | **10.0%** |
| R3 step 50k (BC on 20k AI games) | 92.5% | 54.0% | — | — |
| R3 step 90k | 89.5% | **57.0%** (50.1–63.7) | 25.0% | 10.5% |
| R3 step 100k | — | 59.0% | 29.0% | 9.0% |

Each cell is 100 seeds × 2 sides, with 95% Wilson intervals in research-log.md.

- **The teacher can't beat Crazy, so imitating it can't either.** The game's AI with a person's
  income wins 10% against Crazy (+3★ per turn) and 31% against Hard (+1★ per turn, same
  brain). The R3 policy is already within a few points of it everywhere. Between two copies of
  the AI, income decides the game. Beating Crazy means **playing better than the AI**, not like it.
- **Human games:** 6,721 championship-format games accepted by the final-state check, 2.76M
  decisions; 6,055 for training and 704 held out by game-id hash.
  - The download is nearly done. The remaining Challengermode IDs are mostly from before 2026, and
    those mostly return "Can't load game".
  - The R3 policy picks the human's move only **19%** of the time (top-5 36%), against 82% on AI
    games. Humans play differently from the AI, which is what we want from them.
  - Seat 1 always moves first and wins **75%** of human games (bot games: 50.8%). The labels check
    out: resignations and the engine's winner agree. So this is a real first-move edge between
    humans, or a tournament seating convention.
- **The value head memorizes games:** training value loss 0.03, held-out 1.1–1.7, worse than a
  coin flip's 0.69.
  - Cause: every game gives ~1,230 positions with one outcome, there are only 20k games, and each
    batch draws from just 24 games per loader worker.
  - Held-out value accuracy rests on only 40 games, so it is noisy.
  - Nothing uses V yet. It matters before search or RL uses it.
- **Simulation runs on the Mac only:** native `polyhost-mac`, 10 cores. A 200-game evaluation takes
  ~7 min. Polytopia isn't installed on the PC, so the PC trains but can't play.

## Strategy

Three levers can take the policy past the teacher. Each keeps the two standing rules: exact game
code, and no hidden information for our agent.

1. **Human imitation:** a stronger teacher, with 9× less data than the AI games. *Tonight.*
2. **Learning from our own games:** policy-only RL against the bots and against itself. The policy
   acts only on its own observation, so it is fog-correct by construction. *Tonight, once the
   plumbing exists.*
3. **Search (M2 turn sampler):** needs a fog-safe clone first. Cloning the true state and
   simulating our own turn shows what's under the fog, which ruin reward comes up and where
   invisible units are, before we commit. That breaks the fog rule.
   - **Fix:** a determinized-state builder (hidden parts resampled from what the agent has seen),
     already in architecture.md's Layer 0 contract.
   - Not tonight: it is polyhost work and a design call for Ben.

**DAgger** (the policy plays, the AI labels the positions it reaches) fixes BC's compounding errors.
It raises the policy toward the teacher (helps vs Normal, the M1 gate), not past it.
`py/dagger.py` is built and tested; it runs when the Mac has spare time.

## The loop

Every iteration is one pre-registered change, judged on fixed seeds.

1. **Train** (PC GPU) from the current champion, or from scratch if the change needs it.
2. **Evaluate** (Mac) on the same seeds every time:
   - vs Normal, Hard and Crazy, 100 seeds × 2 sides each (600 games), seeds 10,000,000+;
   - `py/human_bench.py` on 150 held-out human games (top-1, top-5, value accuracy by game phase).
3. **Promote** if the pooled 600-game win rate beats the champion's on the same seeds by a paired
   sign test (p < 0.05), and Normal hasn't dropped more than 5 points. Otherwise the champion
   stays and the result is logged.
4. **Record** the result in research-log.md and the status log below; commit.

## Queue for tonight (with decision rules)

| # | When | What | Decides |
|---|---|---|---|
| 1 | 21:15–21:50 | Evaluate R3 step 90k vs Easy/Normal/Hard/Crazy; teacher vs Normal/Hard/Crazy | Baseline and teacher ceiling (Ben's question) |
| 2 | 21:25–~23:00 | **R4:** R3 step 100k fine-tuned on human games, 20k steps (GPU shared with R3) | Does human data help the raw policy, especially vs Crazy? |
| 3 | 21:50–22:10 | Evaluate R3 step 100k (R4's base) vs Normal/Hard/Crazy | R4's comparison point |
| 4 | 22:10–23:00 | DAgger round 1: step 100k vs Normal/Hard/self, ~1,000 games (Mac) | Data for #6 |
| 5 | ~23:00 | Evaluate R4 at 5k/10k/20k steps + human bench | Pick R5 (below) |
| 6 | ~23:30 | **R5:** if R4 helped vs Hard/Crazy, train on **human + AI + DAgger mixed**; if R4 hurt vs Normal, mix with more AI/DAgger data; if neither moved, DAgger only | Best supervised policy |
| 7 | done 21:45 | Build policy-only RL (below), test on a small run | Plumbing |
| 8 | ~02:30 | R3 finishes (290k). **M1 gate:** 400 games vs Normal (≥ 60%, Wilson lower bound reported) | M1 pass/fail |
| 9 | 01:30–05:30 | **RL iterations** from the best supervised policy, ~45 min each | Policy improvement past the teacher |
| 10 | 05:40 | Wrap-up: results, HANDOFF.md, commit, morning summary | — |

### Policy-only RL (item 7–9; built: `py/dagger.py --temperature 1` + `py/train_rl.py`)
- **Actors** (Mac, 6 processes): the champion plays, sampling at temperature 1.
  - Opponents: Normal 1/6, Hard 2/6, Crazy 2/6, itself 1/6 (`--opponents 1,2,2,3,3,self`).
  - Recorded per agent decision: observation, legal list, played row, its log-probability, the AI's
    label (so the same games serve DAgger), and the outcome.
  - 400–1,000 games per iteration (~0.5 games/s on the Mac's GPU).
- **Learner** (PC): PPO's clipped objective with A = outcome − the mean outcome against that
  opponent, divided by its standard deviation. No learned baseline: V isn't trustworthy yet.
  - Plus a behaviour-cloning anchor: cross-entropy on batches of the champion's own supervised
    data, so the policy doesn't drift.
  - Plus a value loss on these games and a small entropy bonus.
  - lr 3e-5, 2 epochs.
- **Evaluate and promote** per the loop. Two iterations without gain stop the RL arm for the night
  (do a loss autopsy, don't stack changes).
- **Expectation:** modest. The only reward is the game's result, ~600 decisions share one
  advantage, and the Mac plays ~2,000 games an hour. The value of tonight's RL is a working loop and
  a first measured slope.

### Value-head fix (after the supervised runs, one change)
- Validate value on 400 held-out games × 16 positions, not 40 games.
- Then try one of: value loss on a random 1/8 of each batch's positions, a larger shuffle window
  (more games per batch), or both (one per run). Human, DAgger and RL games add new outcomes too.

## For Ben (morning)
1. **Install Polytopia on the PC through Steam.** All play runs on the Mac's 10 cores today. A native
   Windows host puts 20 cores next to the GPU, the biggest throughput gain available, and it's
   needed for RL and search at any real scale.
2. **Search and fog:** OK to build the determinized-state builder in polyhost next? It's the
   prerequisite for the M2 turn sampler under the fog rule.
3. **The milestone order changes:** RL (M3 arm a) before the M2 sampler. The search path is blocked
   on item 2, and the teacher ceiling means the next gains must come from human data or RL.

## Status log
- 21:16 Started. R3 at step 95k of 290k (ends ~02:30, slower while R4 shares the GPU). Download
  at 7,296 fetched / 16.3k attempted.
- 21:25 R4 launched on the PC (shares the GPU with R3).
- 21:45 Step 90k and the teacher fully evaluated (table above). Built: `py/dagger.py` (also
  records sampled moves and their probabilities for RL), `py/train_rl.py` (PPO learner, tested on
  4 games), and remote GPU inference (correct, but 150–170 ms per batch while the GPU trains, so
  unused for now). DAgger/RL data from step 100k queued on the Mac (1,000 games).
- 21:50 R3 step 100k (R4's base) evaluated: Normal 59.0%, Hard 29.0%, Crazy 9.0%; human top-1
  19.0%. Added `tools/compare_evals.py` (paired sign test) and `train_bc --value_val_games`. R4 at
  step 5k is being evaluated on the same seeds.
