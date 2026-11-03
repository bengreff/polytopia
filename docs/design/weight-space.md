# Weight-space programming for the agent (plan, 2026-11-02)

**Verdict: worth a scoped try, but as a tool-driven loop of analysis and edits, measured in games.
Reading our network's raw numbers is out.** Weight arithmetic (extrapolating along training's own
direction, adding the S10 tracks' changes) is the cheapest and most likely to pay. Targeted edits of
the habits no training run fixed are the real test of the MIT idea and may well fail. Writing new
circuits by hand comes last. Ben's rule (2026-11-02, decision log) governs all of it: an edit goes
into the agent only if it wins ≥ 55% against the unedited network over 400 games, and it is logged.

## 1. What the MIT work showed, and what it didn't

Takehi, Teoh, Zhang and Khattab (MIT CSAIL), "Sparks of Weight-Space Programming: LLMs can natively
read & write neural network weights", a blog post of 2026-11-02 with full logs on GitHub. It is not
peer-reviewed. The model was GPT-6 Astra, with Codex's own tools switched off; in some experiments it
could run Python in a sandbox, with a budget of forward passes. All numbers below are as reported.

| Experiment | Network | What the LLM had | Result |
|---|---|---|---|
| Read: which game? | MLP, 17,253 weights, PPO 1B steps on 10×10 grids | all weights as text, no tools | named the right game for all 4 |
| Edit: fix a car policy | 171,664 weights | weights, scene, one recorded episode; no forward passes | 0/256 successes → 1,024/1,024, changing 3.5% of weights |
| Write from scratch: MinAtar | ~19k weights, zero init | rules and source code; one reply, no tools | Breakout 517.4 vs PPO-500M's 25.3; Seaquest 53.1 vs 88.9; Asterix 53.2 vs 55.2 |
| Write from scratch: 9×9 Go | 1,480,274 weights | Python sandbox, 0–50 forward passes (128 rollouts each) | at 50 passes, 74.2 / 70.0 / 78.5% over 3 seeds vs PPO baselines trained on ~20M games |
| Edit: Qwen3.5-0.8B math | 131,072 weights (0.016%) in layers 15 and 19 | sandbox, 50 forward-pass requests, 158 turns, 20.3 h | MATH-500 7.9 → 48.1%, GSM8K 13.8 → 67.1%, IFEval 21.0 → 62.3% |

- **The Qwen edit** removed one "repetition direction" u from 64 output columns of two MLP layers:
  W ← W − 6·u(uᵀW) on those columns. Responses hitting the 32,768-token cap fell from 93.0% to 33.1%.
  It's the same move as subtracting a steering vector, made permanent in the weights.
- **Failures:**
  - Gemma 3 270M didn't improve (6.8% → 7.0–7.7%): its errors were ordinary arithmetic and algebra
    slips, with no single component behind them.
  - Telling a sycophantic model from an honest one by its weights failed until the authors supplied
    attention measurements.
- **The authors' rule:** an edit can remove an obstacle that hides an ability; it can't add an
  ability the model lacks. The forward-pass budget mattered: Qwen's gain came at 50 passes, not 10.
- **Weak spots:** earlier prompt trials aren't in the logs (selection effects); it's unshown that
  gains compound with further training; the "20×" chart Ben saw is a third-party replication.

Prior art that points the same way (all on small networks):
- the "cheese vector": subtracting one activation difference from a maze-solving RL policy makes it
  ignore the goal (Mini et al. 2023);
- ExPO: extrapolating from the start checkpoint through the trained one improves aligned LLMs from 7B
  to 70B (Zheng et al. 2024);
- task arithmetic and model soups. Our four-track soup already reached 55% against the control
  (S9 read).

## 2. What carries over to our network

**Our network** (`py/model.py`, measured: 5,917,069 parameters):
- a 6-layer transformer, width 256, over 197 tokens (the global token and the 196 tiles);
- a pointer head that scores each legal command: its type-and-argument embedding (`cmd_emb`), plus
  projections of its source and target tiles and the global token, through a small MLP (`cmd_out`);
- a value head on the global token.

It is 4× the Go network and 340× the 17k-weight ones.

**Reading the raw numbers is out.** 5.9M numbers as text is tens of millions of tokens. The mode that
scaled in the MIT work is the Go/Qwen one: a Python sandbox, tensor summaries and forward passes on
chosen inputs. Here that means diffs, SVDs, probes, activation contrasts and per-command score tables,
with game batches as the "forward-pass budget".

**Measured tonight: where self-play changed the network** (s9v gen 0 = R7 → gen 83, PC, CPU):

| Tensor | Change ‖ΔW‖/‖W‖ | Share of ΔW in its top 1 / top 8 directions |
|---|---|---|
| Whole network | 12.2% | — |
| Value head, first layer | 42.7% | 46% / 90% |
| Last trunk layer, MLP output (`layers.5.linear2`) | 27.2% | 40% / 74% |
| Pointer head MLP (`cmd_out.1`) | 26.2% | 20% / 63% |
| Early trunk layers (0–2), each matrix | 25–31% | 8–29% / 33–52% |

- Reading: self-play's change is **low-rank at the output end** (a few directions in the value head
  and the last layer) and diffuse in the early layers. That is the structure the Qwen edit relied on.
- So the late layers are the place to look first, for both extrapolation and direction-level edits.
  The early layers are left alone.

**The obstacles to target** (gen 83's training games, research-log S9 read, against human
championship games):

| Indicator | Gen 83 | Humans |
|---|---|---|
| Techs by turn 20 | 6.6–7.2 | 9.2 |
| Stars banked | 5.6–6.5 | 3.1 |
| Catapults (share of units) | 16–20% | 5% |

- Plus the M4 weak matchups: Oumaji vs Vengir and vs Xin-xi, and Xin-xi vs Vengir.
- No S9 track moved these gaps. Whether any of them is localized (one direction, or a few command
  embeddings) is unknown. That is the first thing to measure.

## 3. The plan

**Phase 0: kit and diagnosis (read-only, about half a day of agent time, light compute).**
- A kit, `py/wsp/`:
  - `diff`: per-tensor change and SVD, as above, for any two checkpoints;
  - `probe`: activations of every layer's global token and of each chosen command's z (the vector
    `cmd_out` scores), on ~20k held-out positions from self-play and human games, labelled with the
    indicators (`py/indicators.py`);
  - `prior`: each command's score by type and argument on real positions: gen 0, gen 83, and the human
    frequency;
  - `direction`: difference of means or a linear probe between contrast sets (the cheese-vector and
    Qwen recipe);
  - `edit`: applies a JSON edit spec and writes a checkpoint plus a manifest (hash, edits, weights
    touched);
  - `gate`: the measurement below.
- **Questions:**
  1. Is each habit linearly readable (probe accuracy) and steerable? I.e. does removing one direction
     move the habit while the policy elsewhere changes little (KL)?
  2. What do the top change directions of `layers.5.linear2` and the value head do: which positions
     and commands do they move?
- **Output:** a diagnosis section in this doc.
- **Kill phase 2 if** no habit has a direction whose removal closes half its gap to humans at
  KL < 0.05 (a guess, to be set from the data) on unrelated positions.

**Phase 1: weight arithmetic (no judgement in the weights; at the S10 read).**
- **1a. Extrapolation (ExPO):** θ = θ₈₃ + α(θ₈₃ − θ₀) for α in {0.1, 0.25, 0.5}. Also a restricted
  version that extrapolates only the top change directions of the late layers.
- **1b. Track arithmetic at the S10 read:** gen 83 plus a scaled sum of the tracks' changes
  (gen 95 − gen 83 for `s10p`, `s10t`, `s10x`). Compare it with the plain soup.
- Cheap: an offline screen drops broken candidates before any games.

**Phase 2: targeted habit edits (the real test of the idea).**
- One habit at a time, in this order:
  1. star banking (my guess at the most localized: End Turn's score against the stars feature);
  2. catapult share;
  3. research tempo;
  4. the weak tribe cells (tribe embedding rows in the global token).
- **Method:**
  1. Build contrast sets, e.g. turns ended with ≥ 5 stars and a useful buy available, against turns
     that spent.
  2. Find the direction u at the layer where the probe peaks.
  3. Make a permanent rank-1 edit of that layer's output weights, W ← W − α·u(uᵀW), as in the Qwen
     edit. For a command prior, shift that command's embedding row instead.
  4. Sweep α offline by its KL budget, then check the indicators on 200 self-play games, then gate.
- **The trap:** a habit that's bad against humans may be right against self-play, and the 55% bar is
  a self-play bar.
  - So every candidate also reports its results against crazyplus, from human positions and against
    gen 0.
  - An edit that fixes a habit but only draws head-to-head (45–55%) fails Ben's bar for the agent. It
    could still be offered for his games: his call.

**Phase 3: critic edits for the planner.**
- The planner's +30 points rest on the fog critic ranking plans correctly.
- A global recalibration of the value logit doesn't change which plan wins a turn. So edits here
  target ranking: error analysis by game phase and fog, then the critic's low-rank change directions.
- **Measures:**
  1. pairwise ordering of positions against outcomes on held-out games;
  2. held-out log loss;
  3. the planner's win rate.

**Phase 4 (not now): writing circuits by hand.**
- For example, exact one-hit-kill detection from the verified damage formula, written into an
  attention head.
- The Go result says writing at 1.5M weights is possible. But it is the furthest from learned
  judgement.
- Only if phases 1–2 show that edits survive RL.

## 4. Measurement (the gate)

1. **Offline screen:**
   - KL to the unedited network on 5k held-out positions;
   - human top-1 (R7 43.7%);
   - value log loss on held-out games.
   - Drop anything over a KL limit set in phase 0, or losing > 1 point of human top-1.
2. **Indicators** on 200 self-play games.
3. **Games:**
   - 400 against the unedited network: paired seeds, sides swapped, planner off;
   - 200 against gen 0;
   - 100 against crazyplus (once validated);
   - 200 from human positions.
   - **Bar:** ≥ 55% against the unedited network (Ben). Proposed in addition: no drop of more than
     3 points on the others.
4. **Hygiene:**
   - tune on one seed block and confirm on a fresh one, as crazyplus does;
   - count every gate run as part of the budget;
   - log every edit tried, failures included, in the research log, since selection effects were the
     MIT post's weak spot.
5. **Survival:** an accepted edit becomes the base of a 2-generation RL continuation. Does the habit
   stay fixed and the gain hold? The MIT post didn't test compounding.

## 5. Who, where, when, cost

- **Who:** one Opus agent, or the lead, once tonight's builds (s10x, the human pass, crazyplus) are
  done. No swarm.
- **Where:**
  - phase 0 on the PC's CPU or the Mac's GPU at night (a 5.9M network and 20k positions are light);
  - gates on the PC beside S10, at 8 game processes.
- **When (guessed):**
  - phase 0 from tomorrow night;
  - phase 1 at the S10 read (gen 95, about 1.5 days out);
  - phase 2 after that.
- **Cost (guessed):**
  - phase 0, 4–6 agent hours;
  - about 1 PC-hour per gated candidate;
  - 3–6 candidates per phase.

## Sources

- Takehi, Teoh, Zhang, Khattab, [Sparks of Weight-Space Programming](https://rikiyat.github.io/blog/weight-space-programming/) (blog, 2026-11-02);
  logs: [RikiyaT/lm_understand-logs](https://github.com/RikiyaT/lm_understand-logs).
- [VentureBeat summary](https://venturebeat.com/technology/like-gene-editing-for-neural-networks-mit-researchers-used-gpt-6-astra-to-edit-qwens-open-weights-directly-boosting-math-performance);
  [kingy.ai summary](https://kingy.ai/blog/weight-space-programming-ai-agent-model-weights/) (the
  Qwen edit's formula).
- [weight-surgery](https://github.com/lorenzofamiglini/weight-surgery): an open agent skill after the
  post (read weights, find a direction, closed-form edit, paired bootstrap on held-out data).
- Mini et al. 2023, [Understanding and Controlling a Maze-Solving Policy Network](https://arxiv.org/abs/2310.08043)
  (the cheese vector).
- Zheng et al. 2024, [Weak-to-Strong Extrapolation Expedites Alignment](https://arxiv.org/abs/2404.16792) (ExPO).
- The weight diff: `py/wsp/diff.py` (run on the PC with `s9v/gen000.pt s9v/gen083.pt`).
