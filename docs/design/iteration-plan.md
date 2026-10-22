# Iteration plan: from behaviour cloning to beating Crazy (2026-10-21, night 1)

A working plan for improving the network in measured steps, starting the night the human-replay
download finished. Runs are pre-registered in [research-log.md](research-log.md). The status log at
the bottom is updated as results come in.

## Headline (22:05)

**Fine-tuning on human games (R4) took the raw policy from 9% to 90% against Crazy** (200 games,
95% CI 85.1–93.4), and from 59% to 100% against Normal, after only 6,000 steps. It wins by early
capital capture, as people do. The rest of this plan is now about pushing that past the M4 gate
(Crazy ≥ 90% lower bound, no matchup cell below 70%) and checking it with Ben.

## Where we were (21:30)

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

## Queue for tonight (revised 22:10, after R4)

| # | When | What | Decides |
|---|---|---|---|
| 1 | done | R3 step 90k/100k and the teacher vs all four bots | Baseline and teacher ceiling |
| 2 | done | **R4** at 6k: 100% / 97.5% / 90% vs Normal / Hard / Crazy | Human data is the lever |
| 3 | 22:10–~23:10 | **R5:** RL from R4 at 6k, 2 iterations of 600 games (Crazy 2/3, self 1/3, T 0.5), each evaluated vs Hard and Crazy | Does RL add to human BC? |
| 4 | ~23:00 | R4 at 20k (end of its run) vs Normal/Hard/Crazy + human bench | Is longer human training better than 6k? |
| 5 | 22:10–~23:45 | **R6:** human fine-tune on winners' moves only (GPU) | Filtered BC vs R4 |
| 6 | 23:10–05:00 | R5 continues (4 more iterations) from whichever of {R5, R4 at 20k, R6} is best, re-planned at each result | Policy improvement |
| 7 | late night | **M4 gate screen:** best checkpoint vs Crazy on every tribe pair × seat (`--cells`), 4 per cell first (1,152 games), then 20 per cell if the screen is clean | Is there a weak matchup? |
| 8 | after R3 ends (~02:30) | RL actors switch to the PC's GPU (`--remote`), several times faster | Throughput |
| 9 | 05:40 | Wrap-up: results, HANDOFF.md, commit, morning summary | — |

Decision rules:
- A candidate replaces the champion when it beats it on the same seeds (paired sign test, p < 0.05
  pooled over Hard + Crazy) and Normal stays ≥ 98%.
- RL stops for the night after 2 iterations with no paired gain vs Crazy (then autopsy).
- Supervised runs are compared at equal steps on the same evaluation.

### Policy-only RL (item 7–9; built: `py/dagger.py --temperature 1` + `py/train_rl.py`)
- **Actors** (Mac, 6 processes): the champion plays, sampling at temperature 0.5 (R5).
  - Opponents for R5: Crazy 2/3, itself 1/3 (`OPPONENTS=3,3,self`). Hard and Normal games are all
    wins for R4, so they carry no signal.
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
- 22:00 **R4 at step 6k (human fine-tune): 193/193 so far vs Normal** (base: 59%). Games end
  around turn 19 instead of 40: it wins by early capital capture, as humans do. Hard and Crazy
  evaluations next. Lesson logged: `pkill -f` must use a `[t]` pattern, or it stops its own shell
  (R3/R4 were paused ~2.5 min by mistake).
- 22:05 **R4 at 6k: 200/200 vs Normal, 180/200 (90.0%) vs Crazy.** DAgger dropped (AI labels would
  pull toward the 10% teacher). RL (R5) from R4 at 6k queued on the Mac, with opponents
  Hard 1/4, Crazy 1/2, self 1/4. Built `tools/play.sh` so Ben can play the network in the browser.
  Stopping R3 (now superseded) to free the GPU was refused by the permission check; it keeps
  running, and stopping it is on Ben's list.
- 22:10 R4 at 6k vs Hard: 195/200 (97.5%). **M2's win-rate thresholds are met by the raw policy**
  (lower bounds 98.1 / 94.3 / 85.1% vs Normal / Hard / Crazy). R6 (winners-only human fine-tune)
  launched on the PC. R5 (RL) amended before its start: Crazy 2/3, self 1/3, temperature 0.5.
- 22:20 R5 iteration 1 is playing: at temperature 0.5 it wins 76% vs Crazy (209/275; greedy: 90%).
  Game processes and the network each take half the time, so `dagger.py` and `eval_vs_bot.py` now
  overlap them over two groups of slots (identical games on the same seeds). `rl_loop.sh` now
  retries every transfer after a 30-s ssh outage at 22:12. R4 at step 9k, R6 at 1k, R3 at 115k.
