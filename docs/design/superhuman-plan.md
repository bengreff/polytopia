# The road to superhuman (v6, 2026-10-30)

A restrategy after 72 self-play generations, 8 branches and 4 parallel tracks. It covers what the
evidence says holds the network back, what "superhuman and versatile" has to mean here, the order
of work, and the next experiment (S9, five tracks). Numbers come from [research-log.md](research-log.md).

## 1. What the evidence says

| What we did | Effect on strength | Source |
|---|---|---|
| Imitation of the game's AI (R3) | 9% vs Crazy (= the teacher) | R3 |
| **Imitation of 7.7k human games (R4, R7)** | **9% → 94% vs Crazy**: the only large jump so far | R4, R7 |
| PPO self-play from R7 (S3 → S5 → S7) | 64–73% vs gen 0 (+100 to +170 Elo) from gen ~16 on, and inside that band for the 50 generations since, on every branch | S3–S7 |
| Credit per move (GAE λ 0.97) | won 58% head-to-head vs outcome-only credit: the one clear algorithmic win | S5 |
| KL anchor to gen 0 (track D) | 55.5% vs the base over gens 65–71, 73% vs gen 0: the current best | S7 |
| Value weight 1.0 on the shared network (B) | collapsed: 18% vs the base | S7 |
| GAE λ 0.9 (C) | 72% vs gen 0 but 45% vs the base (non-transitive) | S7 |
| Human-position starts, 1,715 / 53,272 states | 46% / 49% vs the base (hurts / neutral) | S4b, S8 |
| Turn planning scored by the value head | **30%** vs the raw network | S6 |
| Ben vs gen 24 (98% vs Crazy) | Ben won easily at turn 29, ten cities to one | 2026-10-24 |

Reading:
- **Tuning PPO has stopped paying.** Every tweak since S5 lands within ±5% of its control, except
  the ones that break the network. Gen-to-gen results sit at ~50%, so each generation's step is
  close to zero.
- **Our numbers don't measure what matters.** Ben (an intermediate player) beat a network that wins 98%
  against Crazy. We measure the network against itself, its own ancestors and a bot it has already
  beaten. Nothing measures how exploitable it is, or how it plays in human-style positions.
- **The value function is the bottleneck for search.** It explains ~14% of outcome variance in
  self-play. On human games, its early-game accuracy drifted from 70% to ~56% by gen 17. It
  overrated the plans the planner found (S6), and training it harder on the shared trunk damaged
  the policy (B).
- **Self-play narrows into its own meta:** catapult-heavy armies (17% vs humans' 5%), riders
  20% → 6%, slow research (7.2 techs by turn 20 vs humans' 9.2), banked stars (5.9 vs 3.1).
  Non-transitive results (C) confirm it: each branch gets good at beating its own family.

## 2. What the target means

"Superhuman and versatile" for the championship format, made measurable:
1. **Unexploitable:** an exploiter trained only to beat a frozen checkpoint gains little against
   it. This is the measure that best stands in for "a strong human finds its hole"; Ben did that
   to gen 24 in one game.
2. **Versatile:** strong in every tribe pair, from either seat, and from human-style positions.
   It should handle any army mix or plan, not only the ones its own self-play produces.
3. **Strong:** beats Ben best-of-5, then stronger human players, locally.

## 3. Strategy: five pillars, in order

**Pillar 1. Measure what matters (now, cheap).**
- **Exploitability:** a fresh exploiter trained against a frozen checkpoint for 4 generations
  (initialized from it, playing only it), then 200 games. A lower score means a more robust
  checkpoint.
- **Human-position matches:** 100 held-out human positions, each played from both seats, so the
  networks meet the positions humans actually reach.
  - Hold out 5% of the games behind `human_starts_big` (CRC32 % 20 == 0) and never use them as
    training starts.
- **Indicators every generation** (`py/indicators.py`): techs by turn 20, stars banked, army mix,
  ports.
- Ben's games, when he has time.

**Pillar 2. A critic good enough to search with.**
- Polytopia combat is deterministic, and one turn is 15–30 moves whose order decides the outcome
  (kill chains, retaliation, healing). A single forward pass per move can't calculate this; a
  search through the engine can. Superhuman play in comparable games came from search
  (AlphaZero, Pluribus, Student of Games) or from far more compute than we have.
- Search needs an evaluation it can trust, and S6 showed ours isn't one yet.
- **Approach:**
  - a separate value network, so value training no longer competes with the policy (track B's
    failure);
  - bootstrapped targets (TD(λ)) instead of the raw result alone;
  - trained on the last few generations' games, not just the newest 1,500.
  - Later option, Ben's call: an asymmetric critic that sees hidden information during training
    only (as in AlphaStar). It would never act, so the fog principle holds for the agent itself.

**Pillar 3. Search as the improvement operator (needs pillar 2).**
- **Expert iteration:**
  - on part of the turns, the planner (several whole turns from the policy, each scored by the
    critic after the opponent's reply) picks the turn;
  - the policy then learns toward the planner's choice.
- This is the original **M3 A/B** (expert iteration vs PPO at equal PC time).
- **Then per-decision Gumbel search** (architecture.md, Layer 2), and **heavy search at play
  time**.
- **Fog:** both need the determinized-state builder, so that search never uses hidden tiles,
  ruins or invisible units. Ben's go-ahead has been pending since 2026-10-22.

**Pillar 4. Robustness: a league and a magnet.**
- **League:**
  - exploiter lines train only against the current main network;
  - their checkpoints join the main network's opponent pool, so its holes get punished and
    patched;
  - that is AlphaStar's answer to non-transitive self-play.
- **Magnet:** regularization toward a reference policy, which D's anchor to gen 0 already does
  (and it helps). Two ways to continue:
  - keep the fixed human prior;
  - move the reference forward every few generations (DeepNash's R-NaD, which converges toward
    an unexploitable strategy).
  - S9 asks which.

**Pillar 5. Scale, once the loop improves.**
- **Throughput:**
  - A generation takes ~47 min: games ~17 min (GPU-bound, at only ~24 positions per batch),
    learning ~22 min, evaluations ~7 min.
  - Larger inference batches and overlapping one track's learning with another's games are the
    cheap wins.
- **A larger network** (the 5.9M model's human imitation plateaued at 43.7% top-1), trained once
  on all human games. This needs Ben's OK (the no-memorizing rule).
- **More human data** as pretraining (the 14k Might Continents games), also with Ben's OK.

**What we stop doing:**
- PPO hyperparameter tweaks (λ, value weight, entropy) as the main line;
- small human-start sets;
- several changes bundled into one run (S3b/S3c).

## 4. S9: the five-track experiment (draft pre-registration)

Approved by Ben on 2026-10-30 and launched from track D's gen 71 (read at gen 83). The final
settings and the deviations from this draft are in the research log's S9 entry.

**Base:**
- Track D (`s5k`) at its latest generation (gen 72 if launched tonight):
  - GAE λ 0.97, entropy 0;
  - KL anchor to gen 0 (R7) at 0.03, value weight 0.25;
  - lr 3e-5, KL limit 0.05, temperature 0.5;
  - 1,500 games per generation;
  - pool: self 0.5, plus gen 0 and the last 7 generations.
- **Plus 30% of non-bot games from `human_starts_big`** (Ben's S8 instruction that human starts
  stay in training), minus the held-out positions, with the zero-move crash fixed.
- The base also runs as the control, so every comparison stays valid.

| Track | Run | Change from the base | Question |
|---|---|---|---|
| 1 Control | `s9c` | none | — |
| 2 Critic | `s9v` | A separate value network (own 6-layer trunk, initialized from the base's weights) replaces the value head. TD(λ = 0.95) targets, value weight 1.0 on its own optimizer. Trained on the last 4 generations' games. The learner recomputes every value with it before GAE | Does a better critic give better credit, and an evaluation search can use? |
| 3 Magnet | `s9m` | The anchor reference moves: every 6 generations it becomes the track's own current generation (same weight 0.03) | Did D gain from the human prior or from the damping? |
| 4 League main | `s9l` | 25% of games against the exploiter line's 2 latest checkpoints (taken from the past-generation share: self 0.5, exploiters 0.25, gen 0 + 3 recent 0.25) | Does facing exploiters make it robust without making it weaker? |
| 5 Exploiter | `s9x` | Starts from the base. Plays only `s9l`'s latest generation. No anchor, no human starts. Reset to `s9l`'s current generation when it wins ≥ 65% against it, or after 8 generations | Part of track 4; its results against `s9l` show `s9l`'s exploitability live |

**Evaluations each generation** (200 games, seats swapped, greedy, as now):
- vs the previous generation and vs gen 0; Crazy every third generation;
- tracks 2–4 vs the control's same generation (seeds from 15,000,000);
- track 5 vs `s9l`'s same generation;
- every third generation: tracks 1–4 vs gen 0 from the 100 held-out human positions × 2 seats.

**Read at 12 generations after the branch** (gen 84 from gen 72), then the read-time measurements:
1. **Head-to-head:**
   - tracks 2–4 vs the control over the last 4 generations (800 games);
   - better if > 55%, worse if < 45%.
2. **Exploitability:**
   - a fresh exploiter for 4 generations against each of tracks 1–4's final checkpoints, then
     200 games;
   - a difference of ≥ 10 points between tracks counts.
3. **Human positions:** tracks 2–4 vs the control from the held-out positions (200 games each),
   plus the pooled vs-gen-0 readings.
4. **Indicators** vs the 400 human games (Part 0 of the strategy book).
5. **Critic quality:**
   - value log loss and accuracy by game phase on held-out control games and on 150 held-out human
     games;
   - the critic vs the control's value head.
6. **Planner re-test (the S6 protocol, 60 games):**
   - plans at temperature 0.5, each scored after the opponent's reply;
   - once with `s9v`'s critic, once with the control's value head;
   - the planner vs the same raw network.

**Expected, and the falsifier for each track:**
- **Critic (s9v):**
  - Expected:
    - held-out log loss at least 0.02 below the control's value head;
    - ≥ 55% vs the control;
    - the planner with the critic ≥ 60% vs raw.
  - Falsifier: log loss no better, or planner ≤ 55%.
    - Then capacity and data aren't what limits the value.
    - Next: the asymmetric critic (Ben's call) or many more games per update.
- **Magnet (s9m):**
  - Expected: below or equal to the control (my guess: the human prior is what helps).
  - Readings:
    - ≥ 55%: the moving magnet is the way to keep improving without a ceiling at gen 0;
    - ≤ 45%: keep the fixed human anchor, and test a stronger one next;
    - in between: keep the fixed anchor (simpler).
- **League (s9l):**
  - Expected:
    - head-to-head ≥ 50% vs the control;
    - exploitability ≥ 10 points lower than the control's;
    - better from human positions.
  - Falsifier: exploitability not lower, or head-to-head < 45%.
- **Exploiter (s9x):**
  - Expected: above 60% vs `s9l` within a few generations of each reset, falling as `s9l` patches
    its holes.
  - If it never passes 55%, exploiters find nothing at this budget. The read-time exploiters
    then show whether every track is already that robust or the exploiter is too weak.

**Cost:**
- ~50 min per track-generation (the critic adds ~10 min of learning), so a round of five takes
  ~4.2 h: ~5.7 generations a day per track.
- 12 generations take ~2.1 days. The read-time exploiters (16 track-generations) add ~13 h, so the
  read lands ~2.7 days after launch.

**Engineering before launch** (mine, about a day; each piece tested on one short generation):
1. **Zero-move games:** `py/selfplay.py` skips games that record no decisions (track E's crash on
   2026-10-29).
2. **Held-out human positions:** split them off, plus `selfplay.py --match` from start states.
3. **Critic:** `train_rl.py --critic`. It loads a second network, recomputes values, trains it on
   TD(λ) targets over a 4-generation window, and saves `critic_gNNN.pt` per generation.
4. **Magnet:** `sp_loop.py --anchor_every K`.
5. **League:**
   - pool entries taken from another run (`run:latest:N`);
   - the exploiter's reset rule;
   - `tools/tracks.sh` runs `s9l` before `s9x` in each round.
6. **Exploitability:** `tools/exploit.sh <checkpoint> <generations>`.
7. **Indicators per generation**, appended to `summary.tsv`.

## 5. After S9

| S9 outcome | Next |
|---|---|
| Critic passes the planner re-test | **S10 = M3:** expert iteration (planner targets on 25% of turns) vs PPO at equal PC time. Needs the determinized-state builder first (fog) |
| Critic fails | Asymmetric critic (Ben's call), or a larger value network with more games per update; re-test the planner |
| League passes | The league becomes the base; add a second exploiter that targets the whole pool |
| Magnet | Decides the anchor policy for the base |
| Any track ≥ 55% | It merges into the base for S10 |

Then **scale**: throughput first, then the larger network with one fresh human pass (Ben's OK).
Then **play-time search**, then Ben's best-of-5.

**Milestones toward the target:**
- **M5:** beats Ben best-of-5.
- **M6:** a fresh exploiter with 8 generations of training stays under 60% against it; no M4
  cell under 80% vs Crazy; better than gen 0 from human positions.
- **M7:** stronger human players, locally (Ben's call).

## 6. For Ben

1. **Approve S9:**
   - stop tracks A–D at their current generation (B has already failed its test);
   - the base becomes D.
2. **Human starts in every track** (your S8 instruction): confirm, or keep them out of the base.
3. **The determinized-state builder** (fog-safe search; pending since 2026-10-22): OK to build it
   now, while S9 runs? Pillar 3 needs it.
4. **Later:**
   - the asymmetric critic (it sees hidden information while training, never while playing);
   - a larger network trained once on the human games.
