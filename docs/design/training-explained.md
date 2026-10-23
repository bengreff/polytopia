# How the network is trained: algorithms and decisions

The whole pipeline as it runs on 2026-10-23, with every decision and why it was made. Numbers come
from [research-log.md](research-log.md), which has the pre-registrations and full results.

## 0. Overview

| Stage | What | Result vs Crazy |
|---|---|---|
| 1. Behaviour cloning of the game's AI (R3) | Learn the rules' "grammar" from 20k AI-vs-AI games | 9% (the AI itself: 10%) |
| 2. Human imitation (R4, R7) | Fine-tune on 7,732 human tournament games | 94% |
| 3. Policy RL against bots (R5, R5b, R5c) | PPO on outcomes vs Crazy and itself | no gain; abandoned |
| 4. Self-play generations (S1 → S2 → S3) | PPO on self-play outcomes, KL-limited, past-generation pool | 98–99% (gen 17: 198/200); 59–65% vs gen 0 |

Each line of the table is 200 games on the same seeds: every map is played twice with the seats
swapped, the network plays greedily, there is no search, and the network sees only its own fogged
view. The early numbers (stages 1–2) were measured before the seat-2 fix (section 7), so half of
those games were mirror matches.

## 1. The environment the network acts in

**Decision: the rules are the game's own compiled code, never a reimplementation.**
- `polyhost` loads the shipped `GameLogicAssembly` (il2cpp) and calls the game's own:
  - match creation and map generator;
  - `ActionManager.ExecuteCommand`;
  - legal-move generators;
  - AI.
- It is checked byte for byte against the real app.
- *Why:* a reimplementation drifts from the real game, and everything trained on it inherits the
  errors. An earlier version of this project was a reimplementation and was thrown away for that
  reason.

**Observation** (what the player's client shows, nothing more):
- **Per tile** (14 × 14), 32 features: terrain, climate, owner, resource, improvement and its
  level/population/xp, unit, its owner/health/xp/flags, roads, borders, etc.
- **20 global features:** turn, seat, tribes, stars, income, score, cities, kills, relation, the
  pending trigger…
- **The viewer's own 64 tech bits.**
- *Decision: fog-correct by allowlist.* Hidden tiles are blank. The opponent's stars and techs and
  enemy units' xp/age/home city are withheld. A fog-leak test checks it.
- *Why:* the goal is a bot that could play a real game, where it can't see through fog.

**Actions:** the legal-move list for the current position, each row `(command type, source tile,
target tile, argument)`, built from the game's generators.
- *Decision:* the network chooses a **row of that list** (a pointer), not a fixed action vector.
- *Why:* the set of legal moves changes size every decision (from 1 to over 1,000 rows), and taking
  it from the game guarantees every choice is legal.
- One turn is many decisions: every move, attack, build, research…, then End Turn.

## 2. The network (`py/model.py`, 5.9M parameters)

**Tile tokens (196):**
- each tile's 19 categorical features go through one summed embedding table (`EmbeddingBag`);
- 4 small sets of effects become 16-bit vectors;
- 7 numbers are scaled (health/100, population/10, …);
- 2 pointer features (the ruling city, the unit's home) become a relative offset plus an "is this
  tile" flag;
- the dense part goes through one linear layer, and a learned position embedding is added.

**Global token (1):** embeddings of seat, tribes, relation, embassies and trigger, plus a linear
layer over the scaled numbers, log(stars), log(income) and the 64 tech bits.

**Trunk:** a 6-layer pre-norm Transformer encoder over the 197 tokens: width 256, 8 heads, feed-forward
1024, GELU, no dropout, final LayerNorm.
- *Why a transformer over tiles:* Polytopia's interactions are long-range (roads, ships, borders),
  and 197 tokens are cheap at this width.

**Policy head (pointer):** for each legal row, the input is the sum of:
- an embedding of (type, argument) and of type;
- a projection of the source tile's token and of the target tile's token (a learned "no tile"
  vector when absent);
- a projection of the global token.

That goes through LayerNorm → Linear → GELU → Linear to one score, and a softmax over the rows.

**Value head:** an MLP on the global token gives the logit of P(the player to move wins).

**Ownership head:** per tile, 3 classes: who owns the tile at the end of the game (nobody, me, the
opponent). It is an auxiliary target that gives a dense learning signal per position.

*Decisions:*
- **No hand-written evaluation anywhere** (a project rule): every judgement is learned.
- **Small model** (2 GFLOP per position), so the GPU never bounds self-play; the CPU (the game
  engine) does.

## 3. Stage 1: behaviour cloning of the game's AI (R3)

- **Data (`py/gen_data.py`):** 19,956 games of the game's own AI playing both seats, created exactly
  as the real app creates a pass-and-play championship game. That is 24.6M decisions; per decision
  the AI's chosen row is the label.
  - Seats are persons, so the AI makes no deliberate mistakes (`GetStupidity` = 0).
- **Loss:** cross-entropy on the AI's row + 0.5 × binary cross-entropy of value vs the outcome +
  0.2 × cross-entropy of ownership.
- **Optimiser:**
  - AdamW, lr 3e-4, weight decay 0.01, β = (0.9, 0.95);
  - 1,000 warmup steps, then cosine decay over the planned 290k steps;
  - batch 256, gradient-norm clip 1.0, bf16 autocast.
  - Data is streamed in shuffled windows of 24 games per loader worker.
- **Held-out split:** games whose seed is a multiple of 50.
- **Result:** at step 100k the policy wins 59% vs Normal, 29% vs Hard and 9% vs Crazy, the same as
  its teacher (the AI on our seat: 10% vs Crazy).
  - The value head memorized the training games (train loss ~0.03, held-out 1.1–1.7).
- *Why this stage:* the teacher is free and unlimited, and it teaches the "grammar" of legal,
  sensible play before scarcer data.
- *Lesson:* cloning caps at the teacher. The game's AI can't beat its own Crazy level, because
  Crazy gets +3★ per turn.

## 4. Stage 2: human imitation (R4, R6, R7)

- **Data:**
  - 7,732 human championship-format games (1v1 Might Lakes Small), 3.17M decisions, from tournament
    records (Challengermode) and share links;
  - downloaded through the game's API and **replayed through the engine**. A game is accepted only
    if the replay reaches the server's own recorded final state; 7,623 matched exactly apart from
    cosmetic fields;
  - games decided by a timeout train the policy but give no value target;
  - held-out split: CRC32(game id) is a multiple of 10.
- **R4:** start from R3 at 100k; the same three losses on human games; 20k steps, lr 1e-4 (1,000
  warmup, cosine), batch 256.
  - **Result: 94% vs Crazy** (from 9%), 99.5% vs Hard; top-1 agreement with held-out human moves
    43%.
  - It wins by early capital capture (~22 turns), as people do.
- **R6 (winners-only imitation):** labels only on the winning side's moves. Identical to R4: no
  gain.
- **R7:** 20k more steps on all 7,732 games, lr 5e-5.
  - Imitation plateaued: held-out policy loss 2.1352 → 2.1324 → 2.1325 over the last 4k steps.
  - Value improved (log loss 0.46, accuracy 78.5%); play unchanged (191 vs 188/200).
- *Decision (Ben):* "We should not be memorizing a training set." R7, trained once to a plateau on
  every human game, is the base. Human games are not replayed again.

## 5. Stage 3: PPO against the bots (R5, R5b, R5c) — abandoned

- PPO on game outcomes:
  - 600 games per iteration, two-thirds vs Crazy and one-third vs itself;
  - baseline = that opponent's mean win rate;
  - a 20–50% behaviour-cloning anchor on human games;
  - acting at temperature 0.5.
- **Result:** greedy play flat over 6 learner steps (KL 0.01–0.02 each). The sampled policy improved
  (84% → 91% in its own games), but that didn't show in greedy play.
- *Lessons:*
  - vs Crazy, a 94%-winning policy gets almost no signal per game;
  - a per-opponent constant baseline gives every move of a game the same credit;
  - the step was tiny.

## 6. Stage 4: self-play generations (`py/sp_loop.py`, `py/selfplay.py`, `py/train_rl.py`)

### The algorithm (S3, as running)

Generation g is produced from generation g−1 as follows.

**1. Play 1,500 training games with π_{g−1}:**
- **Opponents:**
  - half the games: the policy against itself, both seats recorded;
  - half: against an earlier generation drawn uniformly from {gen 0} ∪ {the last 7 generations}.
    Only the learner's seat is recorded.
- Tribes uniform over the 12 regular tribes; seat random; seeds 50,000,000 + 100,000·g.
- **Moves are sampled** from softmax(logits / T) with **T = 0.5**.
- **Caps:** 150 game turns; 400 commands per player-turn (End Turn is forced after that).
- **Recorded per learner decision:** observation, legal list, sampled row a,
  log π_{g−1}(a | s) at T, and the network's own win estimate v_old(s).

**2. Targets:**
- z ∈ {0, 1} is the final result for the player who moved; capped games are dropped.
- **Advantage A = z − v_old(s)**, centred and divided by its standard deviation over the
  generation's decisions.
- There is no discounting and no bootstrapping: the reward is terminal only, and v_old is the
  baseline.

**3. One PPO update** (two passes over the decisions, minibatches of 256):
- r = π_θ(a | s) / π_{g−1}(a | s), both at T = 0.5;
- L = −mean(min(r·A, clip(r, 0.8, 1.2)·A)) − 0.003·entropy + 0.25·BCE(v_θ(s), z);
- AdamW, **lr 3e-5** (100-step warmup, cosine), weight decay 0.01, grad clip 1.0, bf16;
- **Early stop:** stop when the mean of (log π_old(a) − log π_θ(a)) over the last 50 steps
  exceeds **0.05**.
- **No human-data term.**

**4. Evaluate π_g** while generation g+1 plays: greedy, 200 games each vs π_{g−1}, vs Crazy (the
standard seeds) and vs gen 0.

### Why each choice

- **Self-play rather than bots:** against Crazy the policy already wins 94%, so the outcomes carry
  almost no information. Against itself, games are ~50/50, which gives the most information per
  game, and the opponent improves with it.
- **A pool of past generations:** playing only the latest self can cycle (A beats B beats C beats
  A) or forget how to beat older styles. Gen 0 always stays in the pool.
- **T = 0.5:** the human-imitation policy is flat (top-1 ~43%), so at T = 1 its games are much
  weaker than its greedy play; T = 0.5 keeps exploration while the games stay realistic.
- **The value baseline (z − v_old):** a per-game constant credits every move of a game equally. The
  network's own position estimate credits moves made from bad positions that still won, and so on.
  It cut the advantage variance by 30% (squared error 0.174 vs 0.250 for the outcome alone), and a
  baseline that depends only on the state adds no bias.
- **No human anchor:** Ben's no-memorization rule. The anchor would have replayed ~0.65 passes of
  the human set per generation.
- **The KL limit and lr 3e-5:** S2 (no anchor, lr 1e-4) moved the policy to KL ~1.6 within one
  generation, and gen 1 lost 41% to gen 0. The early stop bounds each generation's change.
  - Gen 1 stopped at 2,200 steps.
  - From gen 2 on, the KL stayed ~0.015 and the full pass ran.
- **1,500 games per generation:** ~1M decisions, about an hour of CPU at full speed. It is a
  compromise between signal per step and the number of evaluated generations per day.
- **Evaluation of every generation, on fixed seeds with seats swapped:** paired comparisons on the
  same maps (sign tests) are far more sensitive than independent samples.

### Results so far (S3)

| | Gen 0 | Gen 4 | Gen 7 | Gen 13 | Gen 16 | Gen 17 |
|---|---|---|---|---|---|---|
| vs Crazy | 188 | 197 | 196 | 192 | 195 | **198** |
| vs gen 0 | — | 49.5% | 58.5% | 64.0% | **65.5%** | 59.0% |
| vs previous | — | 48% | 49.5% | 51% | 54.5% | 48.5% |

- Single steps are within noise against their parent (43–55%), but they add up.
- From gen 7 on, every generation beats gen 0 with a 95% lower bound above 50% (gen 16: 65.5%,
  58.7–71.7).
- Against Crazy: 94% → 96–99%.

## 7. Evaluation, and the bugs it caught

- **Protocol:** fixed seeds (10,000,000+ vs bots, 13M/14M for network-vs-network), each map twice
  with the seats swapped, greedy play, 95% Wilson intervals, paired sign tests on identical games.
  - The rules are deterministic, so on the same host the same policy replays the same games.
- **Seat-2 bug (found 2026-10-22):** with a bot in seat 1, the person in seat 2 was given the bot's
  tribe, so half of every bot evaluation was a mirror match.
  - It was found while exporting replays, when a "vs Oumaji" game showed two Imperius players.
  - Fixed, and the baselines were re-measured.
- **Display bug:** the city bar drew total population instead of level progress (the field `xp`).
  The engine was right.

## 8. Throughput decisions

- **The PC has no Steam login**, so it runs the game's ARM Android build under QEMU. It is
  bit-identical to the Mac on every legal list, observation and final state, but ~22× slower per
  thread.
- **Profiling** found 75–80% of late-game time was our per-target move check: one path search per
  target. The game's `MoveCommand.IsValid` is per-unit checks plus a path search, and targets come
  from the unit's own movement options. One full check per unit gives identical lists:
  - 0 of 6,558 states differ;
  - recorded games replay row-identical;
  - **3.4× faster.**
- Game processes: 28 in the daytime, 14 at night, when another project uses the PC.
- A watchdog on the Mac restarts the loop, and a Windows task keeps WSL alive across restarts.

## 9. Could more training on the human recordings help? (assessment, 2026-10-23)

Measured on 150 held-out human games (62,905 decisions):

| | Gen 0 (R7) | Gen 7 | Gen 17 |
|---|---|---|---|
| Top-1 agreement with the human move | 43.7% | 43.2% | 42.5% |
| Policy cross-entropy | 2.109 | 2.153 | 2.219 |
| Value accuracy (all / early game) | 77.2% / 69.9% | 68.5% / 54.8% | 69.5% / 56.7% |

- **The policy stays human-like** while it gets stronger: −1.2 points of agreement over 17
  generations.
- **The value head has drifted badly on human games.** Trained only on ~50/50 self-play outcomes, its
  early-game accuracy on human games fell from 70% to ~56%, close to a coin flip.
  - It doesn't hurt PPO: the baseline only needs to be right on self-play positions.
  - It would hurt search later (the planned turn search uses the value), and play against real
    opponents.

**Options, from most to least promising:**

1. **KL to the human policy, not the human data** (AlphaStar's approach): add
   β·KL(π_θ ‖ π_R7) on the self-play positions.
   - The human games aren't replayed (no memorization); the frozen R7 network is the prior.
   - Would allow larger steps without S2's runaway, and keeps human strategic knowledge.
   - Cost: one more forward pass per batch (~+30% learner time).
   - Evidence it's needed is weak: the policy isn't drifting much. Its main benefit would be
     faster progress at a higher lr.
2. **Self-play from human positions:** start part of the training games from positions sampled from
   the human recordings (replayed to a random turn), not from turn 0.
   - Uses the recordings as states, not labels, so no memorization.
   - Broadens the state distribution beyond what the policy itself reaches. That matters for the
     real goal of beating people, and for the value head on human-like positions.
   - Cost: moderate (load a replay to turn k, save, start the game from there). Older-version
     replays carry their own rule version.
3. **Monitoring only:** run the human benchmark each generation (top-1 and value on held-out human
   games) to catch forgetting early. Cheap; recommended regardless.
4. **More imitation passes, winners-only or advantage-weighted imitation:** **not recommended**.
   - R6 (winners-only) and R7 (more passes) gave no play gain.
   - It is exactly the memorization Ben ruled out.

**Recommendation:** keep S3 running, since it is still improving. Add option 3 now. The next
experiment is option 2 (or 1), pre-registered as its own run, so S3 stays the control.
