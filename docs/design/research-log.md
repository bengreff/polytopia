# Research log

Every run that counts gets an entry **before** it starts: hypothesis, the single change,
expected result, falsifier, config and seeds. Results are filled in afterwards from the
evaluator's output, never edited to fit. See training-and-data.md, "Research log".

Common setup unless stated:
- **Games:** championship format (1v1 Might, Lakes, Small 14×14), built as the real app builds
  pass & play (`env::Match`, byte-identical to the real app).
  - **Seats:** tribes uniform over the 12 regular tribes on both seats.
  - **Seed sets:** training seeds 0–19,999, generated from the top down (seeds ≡ 0 mod 50 held out
    for validation);
    evaluation seeds from 10,000,000.
- **Interface:** obs version 1, legal-list version 2 (tile menus only on explored tiles).
- **Teacher:** the game's AI on both person seats (handicap 1, `GetStupidity` 0). It keeps its full
  view of the map; our agent never gets hidden information (fog-leak test).
- **Model:** `py/model.py`, d=256, 6 layers, 8 heads (5.6M parameters).
  - Pointer policy over the legal list, value head, ownership head.
  - Losses: policy CE + 0.5 value BCE + 0.2 ownership CE.
  - AdamW 3e-4, warmup 1,000 steps, cosine decay, batch 256, bf16.
- **Evaluation:** `py/eval_vs_bot.py`.
  - The raw policy, greedy, no search, plays each seed twice on the same map and tribes, with sides
    swapped against the game's bot.
  - Turn cap 150 counts as not a win. Results carry a 95% Wilson interval.

## M0 — interface checks (2026-10-21)

| Test | Result |
|---|---|
| Fog leak (`polyhost fogtest 8 5 20`) | 379 states. Every hidden fact perturbed (15,713 hidden tiles, enemy stars and techs, seed, unit id, invisible units, enemy unit XP/age/home): observation byte-identical every time; positive control fired every time; **0** legal rows act on an unexplored tile. Build offers on visible tiles may still depend on fogged neighbours (232 rows), as in the client's own menus |
| Legality, coverage, encoding, determinism (`polyhost envtest 8 41 10 100`) | 6,558 decisions: 0 mismatches against the brute-force sweep (655 states), every AI command covered except its StayCommand (191), 0 duplicate encodings, 655/655 clones exact |
| Found and fixed by the fog test | The generators offered roads and bridges on unexplored tiles; the client never opens a hidden tile's menus (`ClientInteraction.SelectTile` → `Tile.get_IsHidden`). Legal list now version 2; the 1,750 games recorded with version 1 are set aside (`data/m1_v1`, used only for the first-move statistic) |
| Training/inference parity (`py/test_parity.py data/m1v2 --games 3`) | 1,603 recorded decisions replayed in a fresh process: identical observations, legal lists and AI choices; evaluation inputs equal training inputs array for array; CPU vs MPS logits within 3e-7 |

## R1 — 1-hour smoke test (pre-registered 2026-10-21)

- **Hypothesis:** behaviour cloning on 2,000 teacher games, trained for at most 1 hour, gives a
  policy that beats **Easy** in ≥ 80% of games.
- **Hardware (amended before the run):**
  - The 1 hour runs on a Modal cloud **L4** GPU (`py/modal_train.py`, free credits).
  - The PC is offline, and the Mac trains ~30× slower (105 samples/s), so an hour there would
    not be a fair smoke test.
- **Change:** none (first run).
- **Config:** the first 2,000 games generated, seeds 18,000–19,999 of `data/m1v2` (the generator
  works down from the top of the range), `--min_seed 18000`; the common setup above.
- **Evaluation:** vs Easy (difficulty 0), 100 seeds × 2 sides = 200 games, seeds 10,000,000+.
- **Expected:** ≥ 80% wins.
- **Falsifier:** < 80% means the pipeline or the encoding is broken. Stop and debug; don't add
  layers.
- **Result (2026-10-21): FAIL.**
  - **Games:** 127/200 won vs Easy (**63.5%**, 95% Wilson CI 56.6–69.9%); seat 1 67/100, seat 2 60/100;
    0 capped. Mean length: 35 turns for wins, 52 for losses.
  - **Training:** 24,000 steps (6.1M samples, ~2.5 passes) in 57 min on a Modal L4. Held-out policy
    top-1 66.5% (training 67.2%); value accuracy 74%.
  - **Diagnosis** (`py/diag_confusion.py`, 29,545 held-out decisions), agreement with the AI by
    command type:

    | Command type | Agreement |
    |---|---|
    | Capture | 99% |
    | Promote | 100% |
    | ExamineRuins | 98% |
    | EndTurn | 89% |
    | Attack | 79% |
    | Move | 67% |
    | Train | 61% |
    | Research | 55% |
    | Upgrade | 41% |
    | Build | 18% |

    - The biggest disagreement is "Move → a different Move" (15% of decisions), much of which may
      be the order of equivalent moves.
    - The model ends its turn while the AI still acted in 1.6% of decisions.
  - **Reading:** with R2 passing, this points to too little learning, especially of economic
    choices, rather than broken plumbing.
  - **Teacher reference** (`eval_vs_bot.py --teacher`, the same 200 games): the game's AI on our seat
    beats Easy 193/200 (**96.5%**, CI 93.0–98.3%), mean 30 turns. The bar was reachable by imitation;
    the model won 123 of the teacher's 193 wins, plus 4 the teacher lost.

## R2 — canary (pre-registered 2026-10-21)

- **Hypothesis:** the evaluation detects a broken learner.
- **Change from R1:** each policy label replaced by a uniformly random row of that decision's legal
  list (`--random_labels`), so the model learns no move preference.
- **Same as R1:** budget, data and evaluation.
- **Expected:** far below R1 (≤ 20% vs Easy).
- **Falsifier:** within 10 points of R1 means the evaluator isn't measuring the policy.
- **Result (2026-10-21): PASS.**
  - **Games:** 3/200 won vs Easy (**1.5%**, 95% CI 0.5–4.3%), 62 points below R1; 1 capped.
  - **Training:** the same budget (24,000 steps on a Modal L4); held-out top-1 5.9%.

## R3 — M1 behaviour cloning (pre-registered 2026-10-21)

- **Hypothesis:** behaviour cloning on ~20,000 teacher games gives a raw policy that beats
  **Normal**.
- **Change from R1:** the full dataset and a longer schedule.
- **Exact config (fixed before launch, 2026-10-21):**
  - **Hardware:** the PC's RTX 4070 Ti SUPER. Other programs (games) may share it, which changes
    speed only.
  - **Data:** `~/polydata/m1v2`, which is still filling to 20,000 games (11,500 done at launch,
    complete within about 3 hours). The loader samples uniformly over the games present, so early
    games are seen somewhat more often.
  - **Schedule:** 290,000 steps × 256 ≈ 74M samples, about 3 passes over ~24.6M decisions.
  - **Checks:** validation on 40 held-out games every 10,000 steps; checkpoints every 10,000
    steps.
  - **Learning curve:** checkpoints are also played against Easy and Normal as the emulator frees
    up.
- **Incident (2026-10-21):**
  - **Crash:** the first launch died at step 2,400 (16:48). A loader worker opened a game file the
    sync was still writing (`BadZipFile`), and it went unnoticed for two hours.
  - **Fixes:**
    - the loader now skips unreadable files;
    - every file was checked: 4 of 19,960 were truncated by interrupted transfers and moved to
      `m1v2_bad` (seeds 10243, 10306, 10551, 15152);
    - a watcher now reports a stalled or crashed run.
  - **Relaunch (19:05):** from scratch on the complete **19,956 games**, same config. Generation
    stopped at 19,954 finished games (24.6M decisions); human replays come next.
- **Evaluation:** vs Normal (difficulty 1), 200 seeds × 2 sides = 400 games.
- **Gate (training-and-data.md M1):** win rate ≥ 60% with the 95% Wilson lower bound reported.
- **Also reported:** held-out policy top-1, value accuracy, results by seat and by tribe.
- **Falsifier/kill:** not met within 2 weeks of agent time → re-scope (rules access, encoding,
  data), don't add layers.

### R3 — learning curve and teacher reference (2026-10-21)

Every cell is 100 seeds × 2 sides (200 games), seeds 10,000,000+, with the 95% Wilson interval.

| Player on our seat | vs Easy | vs Normal | vs Hard | vs Crazy |
|---|---|---|---|---|
| Teacher (`--teacher`: the game's AI) | 96.5% (93.0–98.3) | 65.0% (58.2–71.3) | 31.0% (25.0–37.7) | 10.0% (6.6–14.9) |
| R3 step 10k | 53.0% | 22.0% | — | — |
| R3 step 50k | 92.5% (88.0–95.4) | 54.0% (47.1–60.8) | — | — |
| R3 step 90k | 89.5% (84.5–93.0) | 57.0% (50.1–63.7) | 25.0% (19.5–31.4) | 10.5% (7.0–15.5) |
| R3 step 100k | — | 59.0% (52.1–65.6) | 29.0% (23.2–35.6) | 9.0% (5.8–13.8) |

- **The policy is already close to its teacher at every level.** The M1 gate (≥ 60% vs Normal) sits
  just under the teacher's own 65%.
- **The teacher can't beat Hard or Crazy:** the same brain with +1★ per turn (Hard) or +3★ (Crazy)
  wins most AI-vs-AI games. Cloning the AI therefore can't reach the M2 target, whatever the
  data volume.
- **Human-move benchmark** (`py/human_bench.py`, 150 held-out human games, 62,069 decisions), step
  100k: top-1 19.0%, top-5 35.9%, policy cross-entropy 13.6; value accuracy 68.4% (early third of
  each game 56.6%, middle 66.8%, late 81.9%).
- **Value head (R3 log):** training value loss 0.02–0.07, held-out 1.08–1.67, accuracy 72–77%.
  It memorizes each game's outcome; see iteration-plan.md.

## R4 — fine-tune on human games (pre-registered 2026-10-21, 21:25)

- **Why now:** the teacher (the game's AI on our seat) wins only 6/100 games against **Crazy**
  (runs/teacher/eval_d3), so cloning the AI caps us far below the M2 target. Humans beat Crazy.
  On held-out human games the R3 policy (step 90k) agrees with the human's move only 19% of the
  time (top-5 36%, `py/human_bench.py`, 20 games), against 82% on held-out AI games.
- **Hypothesis:** fine-tuning the AI-cloned policy on human championship games makes the raw policy
  stronger against the bots, most of all against Crazy.
- **Change:** R3's step-100,000 weights, then trained on `human1` only (6,055 training games,
  704 held out by CRC32; accepted by the final-state check), same losses and model.
  - **Schedule:** 20,000 steps × 256 (≈ 2 passes), lr 1e-4 with 1,000 warmup steps and cosine
    decay; checkpoints every 5,000 steps.
  - **Hardware:** the PC's GPU, shared with R3 (speed only).
- **Evaluation:** vs Normal and vs Crazy, 100 seeds × 2 sides each, the same seeds as R3's
  step-100k base, plus `human_bench.py` on 150 held-out human games.
- **Expected:** human top-1 rises from ~19% to ≥ 35%; vs Crazy above the base; vs Normal within
  10 points of the base.
- **Falsifier:** vs Normal drops more than 15 points with no gain vs Crazy → human-only fine-tuning
  hurts the raw policy; next try mixed data (human + AI games) instead.

- **Final result, step 20,000 (2026-10-21, 23:50):**

  | | vs Normal | vs Hard | vs Crazy | Human top-1 / top-5 | Value log loss |
  |---|---|---|---|---|---|
  | R4 step 6k | 100.0% | 97.5% | 90.0% (85.1–93.4) | 41.8% / 71.2% | 0.944 |
  | **R4 step 20k** | 99.5% (97.2–99.9) | 99.5% (97.2–99.9) | **94.0%** (89.8–96.5) | 43.2% / 72.9% | 0.607 |

  - **Paired, 20k vs 6k:** vs Crazy only 20k won 16 games, only 6k won 8 (p = 0.15). Pooled over
    the three bots: 21 vs 10 (p = 0.07). Likely better, but not significant at 0.05.
  - **Seats:** 93/100 and 95/100. Mean game length vs Crazy 22.3 turns.
  - **Held-out human validation** improved steadily: top-1 42.3% at 6k → 43.6% at 20k, policy
    cross-entropy 2.23 → 2.14; no overfitting by the end.
  - **Value head:** held-out log loss 0.56 at the end (0.94–1.16 mid-run), accuracy 76%. Far better
    than R3's AI-game value (1.1–1.7): human games' outcomes are less predictable from map identity,
    and the lr had decayed.
  - R4 at 20k is the reference policy from here.

- **M4 gate screen, step 20k (2026-10-22, 00:30):** vs Crazy, 4 games for every (agent tribe, bot
  tribe, agent seat) cell, 288 cells, seeds from 11,000,000 (`--cells 4`).
  - **Pooled: 1,092/1,152 = 94.8%** (Wilson 93.4–95.9). The gate's pooled condition (lower bound
    ≥ 90%) is met. By seat: 550/576 and 542/576.
  - **Cells:** 238 won 4/4, 41 won 3/4, 8 won 2/4, 1 won 1/4. A true 95% cell scores ≤ 2/4
    with probability ~1.4%, about 4 of 288 by chance; 9 did.
  - **Weakest tribes:** as the bot's tribe, Zebasi 82/96 and Vengir 84/96; as ours, Xin-xi 86/96,
    Aimo and Quetzali 88/96.
  - **Re-test of the 9 cells below 70%**, 20 fresh games each (seeds from 12,000,000,
    `--cell_list`): **164/180 = 91.1%** (86.0–94.5). Every cell is ≥ 85%:
    - 17/20: Aimo vs Vengir (seat 2), Oumaji vs Luxidoor (seat 1);
    - 18/20: Oumaji vs Zebasi, Xin-xi vs Hoodrick, Xin-xi vs Vengir;
    - 19/20: the other four.

    The screen's low cells were noise.
  - **Full M4 gate** (2026-10-22, 02:10; `--cells 20`: 20 games per cell, 5,760 games vs Crazy,
    seeds from 11,000,000):
    - **Pooled: 5,325/5,760 = 92.4%** (Wilson 91.7–93.1): **pass** (≥ 90%). By seat 2,680/2,880
      and 2,645/2,880. Mean length 22.7 turns.
    - **Cells: 3 of 288 below 70% → the gate as written fails, narrowly.**
      - Oumaji vs Vengir, seat 1: 11/20
      - Oumaji vs Xin-xi, seat 1: 11/20
      - Xin-xi vs Vengir, seat 2: 12/20 (18/20 in the earlier re-test on other seeds)
    - **Distribution of cells by wins out of 20:** 91 at 20, 88 at 19, 49 at 18, 27 at 17, 14 at
      16, 10 at 15, 6 at 14, 1 at 12, 2 at 11. At a uniform 92%, a cell at ≤ 13/20 has
      probability ~1e-4, so these are real weak matchups, not noise.
    - **By tribe:** our weakest tribe is Oumaji (86.0%); the bot's hardest are Vengir (84.6%),
      Xin-xi (86.0%) and Aimo (87.3%).
    - **Next:** aim training at the weak matchups (RL with tribe pairs drawn from the low cells; more
      weight on human games with those tribes), then re-run the failing cells.

## R5 — policy-only RL from the human-tuned policy (pre-registered 2026-10-21, 22:05)

- **Why:** R4 at 6k steps (human fine-tune) wins 200/200 vs Normal and ~75% vs Crazy (evaluation
  running). That leaves room above it, and enough wins and losses against Hard and Crazy to learn
  from.
- **Hypothesis:** PPO on the game's result, from R4's step-6,000 weights, raises the raw policy's
  win rate against Crazy.
- **Change:** RL iterations (`tools/rl_loop.sh r5`):
  - **Games:** 600 per iteration, sampled at temperature 1. Opponents: Crazy 2/3, itself 1/3
    (amended before the start: the first draft had Hard 1/4, but R4 at 6k wins every Hard game,
    which gives no learning signal). Seeds 30,000,000 + 100,000 × iteration.
  - **Temperature 0.5** for both the actors' sampling and the learner's policy (also amended before
    the start). R4 imitates humans, so its distribution is far flatter than the AI clone's (top-1
    41%), and temperature-1 games would be much weaker than the greedy policy being evaluated.
  - **Learner:** `py/train_rl.py` defaults. Clip 0.2, lr 3e-5 with cosine decay over 2 epochs,
    entropy 0.003, value weight 0.25, BC anchor weight 0.5 on `human1` (R4's own data).
  - Each iteration starts from the previous one's policy.
- **Evaluation:** every iteration vs Hard and Crazy (100 seeds × 2 sides each, the fixed seeds),
  paired against R4 step 6k with `tools/compare_evals.py`.
- **Expected:** vs Crazy rises over 6 iterations; vs Hard does not drop.
- **Falsifier:** no gain vs Crazy after 4 iterations (paired sign test p > 0.2 and point estimate
  not higher), or a drop of more than 5 points → stop, look at the learner's logs (ratio, KL,
  clip fraction, entropy), don't stack changes.
- **Interim result (2026-10-21, 22:05), step 6,000** (`last.pt` at its 6k validation; step files
  exist only every 10,000 steps):

  | | vs Normal | vs Hard | vs Crazy |
  |---|---|---|---|
  | R3 step 100k (the base) | 59.0% (52.1–65.6) | 29.0% (23.2–35.6) | 9.0% (5.8–13.8) |
  | **R4 step 6k** | **100.0%** (200/200, 98.1–100) | **97.5%** (195/200, 94.3–98.9) | **90.0%** (180/200, 85.1–93.4) |

  - **M2's thresholds** (training-and-data.md: Normal ≥ 90%, Hard ≥ 70%, Crazy ≥ 50%, all as Wilson
    lower bounds) **are met by the raw policy**: 98.1%, 94.3% and 85.1%. Only M2's "search gain"
    item doesn't apply yet (no search).

  - **vs Crazy, paired with the base on the same 200 games:** only R4 won 162, only the base won 0
    (sign test p = 3e-49). By seat: 91/100 and 89/100.
  - **Games are short:** 19 turns on average vs Normal and 24 vs Crazy, against 40 and 29 for the
    base. The policy wins by early capital capture, as the human players do.
  - **Human held-out agreement** (`human_bench.py`, 150 games): top-1 41.8%, top-5 71.2%, policy
    cross-entropy 2.23 (base: 19.0%, 35.9%, 13.6). Value accuracy 74.0% (base 68.4%).
  - **Pre-registered expectations:** all exceeded. Against Crazy the policy is far past the
    teacher's 10%. The falsifier did not fire.
  - **Reading:** imitating humans works, by a wide margin, and the M2 condition "vs Crazy, Wilson
    lower bound ≥ 50%" is met by the raw policy with no search. The checks behind the number:
    - same evaluation code and seeds as the base's 59%;
    - seats balanced;
    - every game won by the game's own `TryGetWinner`.

## R6 — human fine-tune on the winners' moves only (pre-registered 2026-10-21, 22:10)

- **Hypothesis:** imitating only the side that won each human game (filtered behaviour cloning)
  gives a stronger raw policy than imitating both sides (R4).
- **Change from R4:** `--winners_only`. No policy label on the loser's decisions, nor in games
  decided by a timeout kick; the value and ownership targets are unchanged. Same init (R3 step
  100k), data snapshot (6,741 files on the PC), schedule (20,000 steps, lr 1e-4) and seed.
  - Each batch carries roughly half the policy labels of R4's.
  - Also logs the new value check (`--value_val_games 400`); it doesn't affect training.
- **Evaluation:** R4's protocol at the same steps (6k from `last.pt`, then 20k): vs Normal, Hard
  and Crazy, 200 games each on the fixed seeds, paired with R4 by `tools/compare_evals.py`, plus
  `human_bench.py`.
- **Expected:** vs Crazy higher than R4 at the same step; human top-1 a little lower (half of
  human moves are losers').
- **Falsifier:** vs Crazy not higher than R4 at both 6k and 20k (pooled paired sign test p > 0.2)
  → filtering by result doesn't help at this data size; keep both sides.
- **Incident (2026-10-21, 22:35):** stopped at step 2,800 and to be relaunched from scratch, same
  config, once R4 finishes.
  - **Cause:** four training processes (R3, R4, R6, R5's learner) plus an idle inference server
    oversubscribed the GPU's 16 GB (15.8 GB in use). The driver started paging, and every job
    crawled: R5's learner took 12 minutes for its first 50 steps, and R3/R4 logged nothing for
    3–4 minutes.
  - **Fix:** the inference server and R6 were stopped. The rule from now on: at most three
    training processes on the GPU at once.

### R5 results
- **Iteration 1** (23:14): 600 games played (Crazy 314/400 at temperature 0.5); learner 2,307 steps,
  final KL to the old policy 0.011, clip fraction 0.14–0.21, BC anchor loss steady at 2.19.

  | | vs Hard | vs Crazy |
  |---|---|---|
  | R4 step 6k (start) | 195/200 | 180/200 |
  | R5 i01 | 196/200 | 180/200 |

  - Paired on Crazy: only i01 won 10, only R4 won 10 (p = 1).
  - **No measurable change.** The step is small (lr 3e-5 with cosine decay, anchor weight 0.5), so
    this is expected after one iteration.
- **Iteration 2** (00:18): its games won 334/400 vs Crazy at temperature 0.5 (iteration 1's
  games: 314/400). Greedy evaluation: Hard 195/200, Crazy 182/200 (R4 at 6k: 195, 180).
- **R5 stops here** (its 2 iterations are done). The remaining iterations moved to R5b, which uses
  a larger step and starts from R4 at 20k, now the better start (94% vs Crazy).

## R5b — RL with a larger step, from R4 at 20k (pre-registered 2026-10-21, 23:55)

- **Why:** R5's first iteration moved the policy too little to measure (KL 0.011, Crazy 180/200
  before and after), and R4 at 20k (94% vs Crazy) is now the reference.
- **Hypothesis:** with a 3× larger learning rate and a weaker anchor, a few PPO iterations raise
  the raw policy's win rate against Crazy above R4 at 20k.
- **Change from R5:** start from R4 step 20k; lr 1e-4 (R5: 3e-5); BC anchor weight 0.2 (R5: 0.5).
  - Same as R5: opponents Crazy 2/3, self 1/3; temperature 0.5; 600 games per iteration; 2 epochs.
  - 5 iterations, starting when R5's iteration 2 has been evaluated.
- **Evaluation:** every iteration vs Crazy, 200 games on the fixed seeds, paired with R4 at 20k.
  Vs Hard and Normal at the end.
- **Expected:** a paired gain vs Crazy by iteration 3; KL per iteration 0.03–0.1.
- **Falsifier:** no paired gain after 3 iterations (point estimate not above 188/200), or a drop
  below 180/200 → RL at this scale doesn't help; record and stop.
- **Incident (2026-10-22, 00:55):** the GPU jammed again with R3, R6 and R5b's learner (15.5 GB
  in use). All three stalled for ~20 minutes: R6 at step 8,000, the learner at step 0.
  - The learner and its loop were stopped; R6 and R3 recovered at once (9.8 GB).
  - R5b restarts automatically when R6 writes its 20k checkpoint. Its 600 iteration-1 games are
    kept, and the loop skips games already played.
  - **Rule from now on:** with R3 running, only one more training process on the GPU.

### R6 results (2026-10-22, 02:30)

| | vs Normal | vs Hard | vs Crazy | Human top-1 / CE | Value (150 games) |
|---|---|---|---|---|---|
| R4 step 20k | 199/200 | 199/200 | 188/200 | 43.2% / 2.15 | 75.7%, 0.607 |
| R6 step 10k | — | — | 175/200 | — | — |
| R6 step 20k | 200/200 | 197/200 | 189/200 | 42.8% / 2.19 | 76.8%, 0.580 |

- **Paired, R6 vs R4 at 20k:** vs Crazy only R6 won 8 games, only R4 won 7. Pooled over the three
  bots, 586 = 586 with 10 vs 10 (p = 1).
- **Falsifier fired:** filtering the human data by result gives no gain at this data size.
  Imitate both sides.
- **Value:** R6's new 400-game value check ended at log loss 0.47, accuracy 79.9%.
- The 6k checkpoint was lost (its `last.pt` was overwritten), so 10k stands in for it.

### R4 step 20k on the 19 weakest cells, fresh seeds (02:30)
- 20 games per cell, seeds from 12,000,000 (`--cell_list runs/evals/r4_20k_gate_weak15.txt`):
  **328/380 = 86.3%** (82.5–89.4). The gate's seeds gave 70.5%; those cells were picked for scoring
  low.
- No cell is below 70% on the fresh seeds.
- **The three failing cells,** pooled over both seed sets (40 games each):
  - Oumaji vs Vengir, seat 1: 26/40 (65%)
  - Oumaji vs Xin-xi, seat 1: 27/40 (68%)
  - Xin-xi vs Vengir, seat 2: 28/40 (70%)

  Weaker than the average matchup, but not broken. This is the paired baseline for R5c.

### R5b results
- **Iteration 1** (02:13; relaunched after the 00:55 GPU jam): learner KL 0.017, clip fraction
  0.22, anchor loss 2.07. **Crazy 187/200** (R4 at 20k: 188/200).

- **Iteration 2** (02:34): its games (played with the i01 policy) won **365/400 vs Crazy** at
  temperature 0.5, against 336/400 for iteration 1's games played by R4 at 20k (+7 points, ~3σ).
  The sampled policy improved, while the greedy one (what's evaluated) hadn't moved yet (187 vs
  188).
  - The **learner crashed** (CUDA device-side assert): one decision of 284,423 had its played row
    at index ≥ 1,024, beyond `collate`'s 1,024-row cap (the longest legal list was 1,200 rows).
    Fixed: such decisions are skipped.
  - BC training has the same cap but clamps the label instead, so the very rare decisions with more
    than 1,024 legal rows train on a wrong label. Negligible here; worth raising the cap later.
  - **R5b ends here** with one evaluated iteration (no change). The GPU slot goes to R7, the
    better-evidenced lever.

- **Interpretation check (03:10):** R4 at 20k sampled at temperature 0.5 on the standard Crazy
  seeds wins 178/200 (89.0%, 83.9–92.6), against 188/200 greedy. RL trains the sampled policy
  (84% → 91% in R5b's own games), and its gains don't show in greedy play until the sampled policy
  passes the greedy one.

## R5c — RL aimed at the weak matchups (pre-registered 2026-10-22, 02:20)

- **Why:** the full M4 gate failed on 3 cells, and 19 cells won ≤ 15/20
  (`runs/evals/r4_20k_gate_weak15.txt`). Oumaji on our side, and Vengir, Xin-xi and Aimo on the
  bot's, recur.
- **Hypothesis:** RL on games drawn only from those 19 tribe-pair × seat cells raises the win rate
  vs Crazy in exactly those cells.
- **Change from R5b:** each game's tribes and our seat come from the 19 cells (`dagger.py
  --tribe_pairs`).
  - Same as R5b otherwise: start R4 step 20k; opponents Crazy 2/3, self 1/3 (on the same tribe
    pairs); temperature 0.5; lr 1e-4; anchor 0.2; 600 games per iteration.
  - Starts when R5b ends (after its 3rd iteration if it shows no gain); as many iterations as fit
    before 05:30.
- **Evaluation:** the 19 cells × 20 games on fresh seeds (`--cell_list`, seeds from 12,000,000),
  paired with R4 at 20k on the same seeds; plus the standard 200 games vs Crazy (no regression).
- **Expected:** a paired gain on the 19 cells (R4 at 20k scored 70.5% there on the gate's seeds,
  and somewhat more is expected on fresh seeds, since the cells were chosen for low scores).
- **Falsifier:** no paired gain on the 19 cells after 2 iterations (p > 0.2), or the standard Crazy
  evaluation drops below 180/200.

## R7 — more human training on the complete human set (pre-registered 2026-10-22, 02:40)

- **Why:** human imitation is the only lever that has moved the bots' results tonight. R4 gained
  from 6k to 20k steps (Crazy 180 → 188, p = 0.15) with held-out cross-entropy still falling, while
  R5, R5b and R6 were flat.
- **Hypothesis:** 20,000 more steps from R4 at 20k, on the complete human set, raise the win rate vs
  Crazy and in the weak cells.
  - The complete set is `human_full`: 7,732 files, against R4's 6,741-file snapshot.
- **Change:** init R4 step 20k; data `human_full`; 20,000 steps; lr 5e-5 (warmup 1,000, cosine);
  everything else as R4. Validation on human_full's own held-out split; the hash split is the same
  rule, so R4's held-out games stay held out.
- **Starts:** when R5b stops (after its 2nd iteration if it shows no gain; the GPU takes one job
  beside R3). **Launched 02:40.** Its validation split is human_full's first 40 held-out files (39
  accepted), not R4's 40, so the training-log numbers don't compare with R4's; `human_bench.py`
  (human1's split) does.
- **Evaluation:** vs Crazy, Hard and Normal (200 games each, fixed seeds) and the 19 weak cells
  (380 games, seeds from 12,000,000), all paired with R4 at 20k; plus `human_bench.py`.
- **Expected:** vs Crazy ≥ R4's 188/200; a paired gain on the weak cells.
- **Falsifier:** pooled over Crazy + the weak cells, no paired gain (p > 0.2 and the point estimate
  not higher) → more of the same data has stopped paying.

### R7 results
- **Interim, step 10k (03:50):** vs Crazy 186/200 (R4 at 20k: 188); 19 weak cells 317/380 = 83.4%
  (R4 at 20k: 328/380).
  - Paired, pooled over the 580 games: only R7 won 32, only R4 won 45 (p = 0.17). Not better so
    far.
  - The learning rate is mid-schedule; the 20k checkpoint decides.
- **Final, step 20k (04:30):**

  | | Normal | Hard | Crazy | 19 weak cells | Human top-1 / CE | Value |
  |---|---|---|---|---|---|---|
  | R4 step 20k | 199/200 | 199/200 | 188/200 | 328/380 | 43.2% / 2.146 | 75.7%, 0.607 |
  | R7 step 20k | 199/200 | 194/200 | 191/200 | 323/380 | 43.7% / 2.109 | 77.2%, 0.641 |

  - **Paired with R4 at 20k:**
    - Crazy: 10 vs 7 games won by only one side (p = 0.63)
    - weak cells: 31 vs 36 (p = 0.63)
    - Crazy + weak cells pooled: 514 vs 516 (p = 0.91)
    - Hard + Normal: 393 vs 398 (p = 0.13)
  - **The falsifier fired:** more training on the same human data, even the complete set, keeps
    improving imitation but no longer improves play against the bots. R4 at 20k stays the
    reference.

### R5c results
- **Iteration 1** (04:40): learner KL 0.017, clip fraction ~0.25.
  - **Games:** its 600 games on the 19 cells won 308/400 vs Crazy at temperature 0.5.
  - **Evaluation:** standard Crazy 188/200 (R4 at 20k: 188); 19 weak cells 324/380 (R4 at 20k:
    328/380).
  - No change yet. Iteration 2's games (with the i01 policy) are running at 297/379 vs Crazy.
- **Iteration 2** (05:16): its games won 297/379 vs Crazy at temperature 0.5 (iteration 1's
  games: 308/400).
  - **Evaluation:** standard Crazy 186/200 (R4 at 20k: 188; paired 7 vs 9, p = 0.80); 19 weak cells
    316/380 = 83.2% (R4 at 20k: 328/380).
  - **Falsifier fired:** no gain on the 19 cells after 2 iterations (point estimate lower).
  - Iteration 3 was stopped during its games at the end of the night.
- **Reading of tonight's RL (R5, R5b, R5c; 6 learner steps in all):** none moved greedy play.
  - Each step is small: KL 0.01–0.02 per iteration, with ~600 games and one outcome for ~500
    decisions.
  - The sampled policy did improve in R5b (84% → 91%).
  - Before more RL: much more throughput (the native Windows host, or GPU inference once R3 is
    off the GPU), a learned baseline (the value head is decent on human data now: log loss ~0.5),
    and acting at a lower temperature.

## Morning checks (2026-10-22, ~05:50, Ben's questions)
- **R3 stopped** on Ben's instruction at step ~259k. Its last checkpoint is `step250000.pt`; R3 is
  superseded by the human fine-tune.
- **The bots get their built-in advantage**, checked in a match built exactly as the evaluation
  builds it:

  | Bot difficulty | Bot handicap | Bot income at the start | Our income |
  |---|---|---|---|
  | Easy | 0 | 1★ | 2★ |
  | Normal | 1 | 2★ | 2★ |
  | Hard | 2 | 3★ | 2★ |
  | Crazy | 4 | **5★ (+3★ per turn)** | 2★ |

  The bot has AutoPlay on, so `GetStupidity` = max(0, 2 − handicap) and Crazy plays without
  deliberate mistakes. Our seat is a person's seat (handicap 1, AutoPlay off). This is
  `AddAIOpponent(tribe, HandicapFromDifficulty(d))`, as the real app builds bot opponents.
- **Recorded re-run of R4 at 20k vs Crazy** (`tools/eval.sh … r4_20k_watch 3`, same seeds): 188/200
  again, so the evaluation is deterministic.
- **The 3 failing M4 cells, 10 fresh games each** (seeds from 12,000,000, `runs/evals/m4_failing3.txt`):
  24/30 = 80%.

## Games on the PC without Steam (2026-10-22, morning)
- **How:** the Android build's own `libil2cpp.so` (the binary verified against the real app) runs on
  the PC under **QEMU user-mode emulation** (`qemu-aarch64` 10.0.13, static, from Debian), with the
  Android 14 runtime (linker, libc, libm, libdl, liblog, libc++) taken from the PolyRef emulator.
  `polyhost` is built for Android as before. `POLYHOST=qemu` in `py/polyenv.py` launches it;
  `tools/setup_pc_games.sh` builds the kit and copies it to `~/polyarm` on the PC. No Steam, no
  game install, no internet (WSL on the PC has none: its eero profile is paused).
- **Exact:** 12 seeded games (6 against Crazy), with a fixed pseudo-random policy
  (`py/parity_hosts.py`), run on the PC under QEMU and on the Mac's native Steam build:
  - every legal list, every observation and the final state bytes are identical;
  - the 4 `envbench` bot games also match (same winners and turns);
  - QEMU's floating point is software IEEE, so this is the same arithmetic as an ARM CPU.
- **Speed:** ~22× slower per thread than native. `envbench`, one thread: legal list 21 ms per
  decision (Mac 0.9), built-in AI 23 ms (Mac 1.1).
  - QEMU's default CPU (`max`) is 25% slower than `-cpu cortex-a72`, because it emulates pointer
    authentication in software. The game needs only ARMv8.0.
  - Garbage collection isn't the cost: with `GC_DONT_GC=1` it's 6% faster.
  - **Self-play on the PC** (`py/selfplay.py`, R4 against itself, 28 game processes × 6 slots, GPU
    inference): 651 decisions/s with all threads busy, while another project's runs took ~16
    threads. R4's self-play games are short: 360 decisions and 17.5 turns on average, so ~1.8
    games/s.
- **Faster later:** a native x86-64 build would be ~20× faster per thread: the Windows Steam build
  (needs Ben's Steam login on the PC), or an x86-64 Android build. Ours is APKMirror's arm64 +
  armv7 variant. Whether an x86-64 one exists is unchecked: APKMirror and APKPure refused scripted
  requests.

## S1 — self-play generations from R4 (pre-registered 2026-10-22, 11:00)

- **Why:** Ben asked for self-play on the PC with saved generations, each measured against the last,
  and regular checks against Crazy. Against Crazy, R4 already wins 94%, so games against it carry
  little signal. Self-play keeps the opponent at the policy's own level: about half the games won,
  the most informative outcome.
- **Hypothesis:** PPO generations on self-play games improve the greedy policy. Each generation
  beats the previous one, and the gain accumulates against generation 0 (R4 at 20k).
- **Setup (`py/sp_loop.py`, all on the PC):**
  - Generation 0 = R4 step 20k.
  - Generation g plays `--games` training games with gen g−1 at temperature 0.5.
    - Half are against itself (both seats recorded).
    - Half are against an earlier generation, drawn uniformly from gen 0 and the last 7 others
      (only the learner's seat recorded). Generation 1 plays only itself.
    - Seeds: 50,000,000 + 100,000·g.
  - One PPO step on those games gives gen g (`py/train_rl.py`): clip 0.2, lr 1e-4 with cosine
    decay, 2 epochs, entropy 0.003, value weight 0.25, BC anchor 0.2 on `human_full`, temperature
    0.5. As R5b, except:
    - **More games per step:** 1,500 against R5b's 600.
    - **Mostly self-play:** R5b had 2/3 of its games against Crazy.
    - **The value head as the baseline** (`--baseline value`, new): A = z − v_old(s), centred and
      scaled. `v_old` is the acting network's own estimate before each move, recorded by
      `py/selfplay.py`. Last night's RL summary asked for a learned baseline, and the value head's
      held-out log loss on human games is ~0.6. A baseline that depends only on the position adds
      no bias.
  - **Evaluations,** greedy, each seed played twice with the seats swapped, run on the CPU while the
    next generation learns:
    - vs gen g−1: 200 games, seeds from 13,000,000;
    - vs Crazy: 200 games on the standard seeds from 10,000,000, so directly comparable with R4's
      188/200 (`selfplay.py --match bot:3` reproduces `eval_vs_bot.py` game for game);
    - vs gen 0: 200 games, seeds from 14,000,000.
  - Results go to `runs/sp/s1/summary.tsv` on the PC (`tools/sp_sync.sh s1` copies them to the
    Mac). Evaluation games are saved for the web app's watch mode.
  - The number of game processes follows the PC's free threads (load average), between 8 and 28,
    because the PC is shared with other projects.
- **Launched** 2026-10-22 12:29 on the PC. Its first step re-measures gen 0 against Crazy with the
  PC's own pipeline.
- **Expected:**
  - Each generation scores above 50% against the previous one.
  - By generation 6, ≥ 60% against gen 0.
  - Crazy at or above 188/200.
- **Falsifier:** after 6 generations, gen 6 vs gen 0 has a Wilson lower bound ≤ 50%, or Crazy is
  below 180/200 for two generations in a row.
  - Then: stop, read the learner's logs (KL, clip fraction, value baseline error), and change one
    thing.

### S1 results
- **Gen 0 vs Crazy on the PC** (12:40): **188/200**, the same as R4 at 20k on the Mac with the same
  seeds.
  - 163 of the 200 games are move-for-move identical (same result and length).
  - The other 37 have the same result, ±1–6 turns. The rules are identical across hosts (parity
    above), so the difference is the network's arithmetic: CUDA on the PC, MPS on the Mac. Tiny
    differences flip near-tied greedy moves.
  - Evaluations are compared within a host: every S1 evaluation runs on the PC.

## Human data, status (2026-10-22, 17:10)
- **Championship-format human games: 7,732** (3.17M decisions) in `data/human1`, i.e.
  `human_full` on the PC. This is everything still available:
  - all 27,548 Might/Lakes/Small game ids from the Challengermode harvest were tried: 7,822
    downloaded, 19,726 refused by the game's server (1005 "Can't load game"; mostly pre-2026, so
    expired);
  - the YouTube share links (1,525) are already in.
- **Training to a plateau (Ben: "trained on as many of those for as long as possible until gains
  plateau"):** R7 (R4 + 20k steps on all 7,732) has reached it for this network. Held-out policy loss:

  | Step | Policy loss | Top-1 |
  |---|---|---|
  | 14k | 2.1407 | 43.6% |
  | 16k | 2.1352 | 43.6% |
  | 18k | 2.1324 | 43.7% |
  | 20k | 2.1325 | 43.8% |

  - Its value head kept improving: log loss 0.83 → 0.46, accuracy 78.5% (R4 at 20k: 0.56, 75.9%).
  - Play vs Crazy: the same as R4 (191 vs 188/200).
- **Beyond this, more human data means other formats.** The 14,221 Polytopia Official "Instant
  Tournaments" are Might **Continents** Small, ~40k games; not downloaded.

## S2 — self-play generations from R7 (pre-registered 2026-10-22, 17:10)
- **Why:** gen 0 should be the network trained on every human game until its gains stopped. That is
  R7, not R4, which saw 6,741 of the games. R7's value head is also much better (0.46 vs 0.56 log
  loss), and S1 uses that head as its baseline.
- **Change from S1:** gen 0 = R7 step 20k (`/home/greff/runs/r7_human_full/step020000.pt`). Nothing
  else changes: games, pool, learner, BC anchor on all 7,732 human games every step, evaluations
  and seeds.
- **S1 stops** after its gen 1 evaluation against Crazy. Its results stay on record.
- **Expected and falsifier:** as S1's, with gen 0 = R7. R7's gen-0 Crazy evaluation on the PC is the
  baseline (Mac: 191/200).
- **Amended before S2's first learning step (17:20):** **no BC anchor** (`bc_weight` 0). The
  learner trains on self-play games only.
  - Ben: "We should not be memorizing a training set. As long as whatever we have is trained on all
    7k human games we are good."
  - Gen 0 (R7) is trained on all 7,732 games. A 20% anchor would replay ~0.65 passes over the human
    set every generation, i.e. memorizing it.
  - Drift away from sensible play is watched by the per-generation evaluations vs gen 0 and Crazy.
  - Also new: settings can change between steps through `<run>/override.json`, without a restart.
  - Game processes: all 28 threads 6am–10pm, half at night, when Ben's other projects run.

## Faster legal lists: moves checked once per unit (2026-10-22, 17:50)
- **Where the time went:**
  - R7's self-play games are long (~1,000 decisions). Late in a game, building the legal list took
    ~190 ms per decision under QEMU (~8.6 ms natively).
  - Profile of an R4 self-play game: 75–80% of it was our `IsValid` call on every move target,
    ~250 per list late in the game.
- **Why that is redundant:** the decompiled `MoveCommand.IsValid` is:
  - target-independent checks: the turn, the unit on the From tile, `UnitState.CanMove`, its unit
    data;
  - then `PathFinder.GetPath(From, To)` non-empty: one path search per target.

  The targets come from the same unit's `GetMovementOptions`. Over ~120k unit move lists (8 bot
  games and 2 policy self-play games) a unit's targets were always all valid or all invalid
  (0 mixed). So the first target's full `IsValid` now stands for all of them.
- **Checks:**
  - `envtest 8 41 10 100`: 655 states swept, 0 mismatched; fast vs fully validated legal list,
    0 of 6,558 states differ; 655 clones exact;
  - two recorded self-play games replayed: all 1,638 legal lists identical row for row, same
    winners;
  - `parity_hosts.py`: the same hashes as before, on the Mac and on the PC.
  - If the game ever rejects a listed command, `step()` relists that state with every command
    checked and returns not-ok. The rules stay the game's: `ActionManager` validates every command.
- **Speed:**
  - late-game steps 8.65 → 1.85 ms natively (4.5×); a whole R7-length game ~3.7×;
  - bot games: legal list 0.92 → 0.73 ms.

## Bug: a person in seat 2 got the bot's tribe (found 2026-10-22, 18:30; fixed)
- **Found** while exporting replays for the public site: in every game with a **bot in seat 1 and our
  network in seat 2**, the game was created with the bot's tribe on both sides (40 of 40 checked;
  all seat-1 games correct).
- **Cause:** `env::Match::build` read the person's `PlayerData` with `settings.GetPlayer(i)`. With a
  bot seated, the settings list the person first, so seat 2 got the bot's entry and its tribe.
  - Fix: with a bot seated, take the one LocalUser entry.
  - The (bot, person) combinations now get the requested tribes. Person–person and (person, bot)
    games are byte-identical to before.
- **Affected:**
  - every evaluation against the built-in bots: in the seat-2 half of the games, the network played
    a mirror match with the bot's tribe:
    - eval_vs_bot.py: R1–R7 vs Easy/Normal/Hard/Crazy;
    - selfplay.py --match bot:3: S1/S2 vs Crazy;
    - the --teacher reference;
  - **the M4 gate's seat-2 cells** measured mirror matches of the bot's tribe, not the labelled
    matchup (e.g. "Xin-xi vs Vengir, seat 2" was Vengir vs Vengir). The pooled 92.4% is still real
    games vs Crazy, but half of them are mirrors;
  - DAgger/RL training games against bots with our seat 2 (R5, R5b, R5c).
- **Not affected:**
  - self-play: S1/S2 training games and network-vs-network evaluations;
  - the AI-teacher data (both seats are persons);
  - human replays;
  - the pass-and-play setup verified against the real app;
  - the rules themselves.
- **To redo:**
  - R7 vs Crazy on the standard seeds (running on the PC, 18:40);
  - the M4 gate, before it is quoted again.
  - Recorded seat-2 bot games in runs/games are relabelled (`mirror_bug: true`, both tribes = the
    bot's).

### S2 results (stopped 18:45)
- **Gen 1** (no anchor, lr 1e-4, value baseline): the learner ran away.
  - KL to the acting policy rose to ~1.6 in mid-epoch and ended at 0.50, with clip fraction
    45–62% (S1's gen 1 with the anchor: KL 0.037).
  - Gen 1 vs gen 0: 33/80 when stopped (41%). Worse, not better.
- **Reading:** without the anchor, lr 1e-4 makes steps far beyond PPO's trust region. The value
  baseline also works: its error, 0.174 against the outcome's 0.250, is a 30% variance cut.
  - The step size is the problem, not the signal.

## S3 — S2 with a bounded step per generation (pre-registered 2026-10-22, 18:45)
- **Change from S2:**
  - `--target_kl 0.05`: the learner stops once the mean KL to the acting policy over the last 50
    steps passes 0.05 (PPO's standard early stop);
  - lr 3e-5 (S2: 1e-4).
  - Everything else as S2: gen 0 = R7, no BC anchor, value baseline, 1,500 games, half against
    itself.
- **Reuse:** S2's gen-1 training games were played by gen 0 (R7) with the same seeds and pool. They
  are exactly S3's gen-1 games, so they are copied rather than replayed.
- **Bot evaluations use the fixed seat-2 setup** (above). S3's gen-0 Crazy evaluation is R7's
  corrected baseline.
- **Expected:** KL ≤ ~0.06 per generation; each generation ≥ 50% vs the previous; by gen 6
  ≥ 58% vs gen 0.
- **Falsifier:** gen 3 not above 50% vs gen 0 (point estimate) → the per-generation step is too
  small to matter at 1,500 games; next to try: more games per generation.

### S3 results
- **Gen 0 (R7) vs Crazy with the seat-2 fix:** 188/200 (94.0%, 89.8–96.5%); seat 1 97/100,
  seat 2 91/100. With the mirror matches it had been 190/200.
- **Gen 1's learning step:** stopped at the KL limit after 2,200 of 7,636 steps (KL 0.052, clip
  fraction 0.34), so ~29% of the generation's decisions were used. Its evaluations were running at
  19:00.
- **Gens 1–8 overnight** (200 games each, standard seeds; paired sign tests against gen 0 on the same
  Crazy games):

  | Gen | vs previous | vs gen 0 | vs Crazy | Crazy, paired with gen 0 |
  |---|---|---|---|---|
  | 0 | — | — | 188 | — |
  | 1 | 103 (51.5%) | 104 (52.0%) | 191 | 9 vs 6, p = 0.61 |
  | 2 | 111 (55.5%) | 98 (49.0%) | 187 | 8 vs 9, p = 1 |
  | 3 | 99 (49.5%) | 96 (48.0%) | 189 | 8 vs 7, p = 1 |
  | 4 | 96 (48.0%) | 99 (49.5%) | **197** | 9 vs 0, p = 0.004 |
  | 5 | 96 (48.0%) | 108 (54.0%) | 195 | 8 vs 1, p = 0.04 |
  | 6 | 86 (43.0%) | 98 (49.0%) | 189 | 9 vs 8, p = 1 |
  | 7 | 99 (49.5%) | **117 (58.5%, 51.6–65.1)** | **196** | 10 vs 2, p = 0.04 |
  | 8 | 99 (49.5%) | **117 (58.5%, 51.6–65.1)** | 193 | 8 vs 3, p = 0.23 |

  - From gen 2 on, the KL limit was never reached: KL 0.015–0.017 per generation with all
    ~6,700 steps run.
  - Night speed (8 game processes beside the evaluations): ~95 min of games per generation.
- **Against the pre-registration:**
  - "each generation ≥ 50% vs the previous": no, 43–55.5%; the single steps are within noise;
  - **the falsifier fired at gen 3** (48% vs gen 0, not above 50%);
  - the later expectation, ≥ 58% vs gen 0 by gen 6, was met only at gen 7 and gen 8 (58.5% both,
    lower bounds 51.6%).
- **Reading:**
  - Small steps that each measure as "equal to the previous" do add up: gens 7 and 8 both beat
    gen 0 with a lower bound above 50%.
  - Against Crazy, gens 4, 5 and 7 are paired-significantly better than gen 0 (≈ 94% → 97–98%).
  - Gen 6 was a dip on every measure.
  - The evidence for self-play gains is real but modest. More games per generation (larger
    steps at the same KL) is the next change, as the falsifier said.
- **Gens 9–17** (to 2026-10-23 16:15):
  - vs gen 0: 118, 111, 115, 119, 128, 127, 112, **131 (gen 16: 65.5%, 58.7–71.7)**, 118 of 200;
  - vs Crazy: 195, 197, 192, 195, 192, 195, 192, 195, **198**;
  - vs the previous generation: 42–54.5%.
  - The loop died when Windows was restarted at 17:08. Now a Windows task holds WSL open and the
    Mac's site loop restarts the loop when it isn't running.
- **Human benchmark** (150 held-out human games, `py/human_bench.py`):

  | | Top-1 | Policy CE | Value accuracy (early game) |
  |---|---|---|---|
  | Gen 0 | 43.7% | 2.109 | 77.2% (69.9%) |
  | Gen 7 | 43.2% | 2.153 | 68.5% (54.8%) |
  | Gen 17 | 42.5% | 2.219 | 69.5% (56.7%) |

  - The policy stays human-like.
  - The value head, trained only on self-play outcomes, lost most of its early-game accuracy on
    human games.
  - Options for using the human recordings without replaying them as labels:
    [training-explained.md §9](training-explained.md).
- **Gens 18–28** (to 2026-10-24 10:00; 200 games each):

  | Gen | vs previous | vs gen 0 | vs Crazy |
  |---|---|---|---|
  | 18 | 101 | 116 | 196 |
  | 19 | 87 | 133 | 192 |
  | 20 | 104 | 120 | 192 |
  | 21 | 94 | 121 | 192 |
  | 22 | 96 | 132 | 195 |
  | 23 | 98 | 131 | 195 |
  | 24 | 101 | **140 (70.0%, 63.3–75.9)** | 192 |
  | 25 | 93 | 128 | 191 |
  | 26 | (crashed, see below) | 127 | 193 |
  | 27 | 100 | 129 | 189 |
  | 28 | 87 | — | 190 |

  - Gen 26's evaluation against gen 25 crashed: the per-turn command cap forced End Turn while it
    wasn't legal (a city reward was pending). Fixed in commit 855310b.
  - **Wins are getting slower:**
    - Gen 1 beat gen 0 in 25.4 turns on average (median 25); gen 24 took 34.5 (median 34).
    - Against the previous generation, the average won game went from 27 turns (gen 1) to 35–39
      (gens 22–28).
    - Against Crazy, the average win has stayed at 21.1–22.0 turns at every generation.
    - Nothing in the reward prefers a fast win, so self-play has drifted to long games.

## S3b — S3 with fixed early opponents, the bot and a turn cost (pre-registered 2026-10-24, 10:20)
Ben asked for three changes to S3 from the next generation (gen 30). It continues in the same run
directory, so gens 0–29 are unchanged.
- **Opponents in the training games:**

  | Opponent | Share | Before (S3) |
  |---|---|---|
  | The learner itself | 40% | 50% |
  | Crazy, the game's AI (`--bot_share 0.1`) | 10% | — |
  | Gens 0, 1, 2 (always in the pool, `--fixed 0,1,2`) | 6.25% each | gen 0 only |
  | The last 5 generations before the learner (`--recent 5`) | 6.25% each | the last 7 |

  - In games against the bot, only the network's seat is recorded, as with checkpoints.
- **Turn cost (`--turn_cost 0.005`):**
  - A win at turn T counts as max(0.5, 1 − 0.005 T) in the advantage, instead of 1. For example,
    0.90 at turn 20, 0.85 at turn 30 and 0.80 at turn 40. A loss is still 0.
  - So winning 20 turns sooner is worth about 10% of a win: speed is a tiebreaker, not the goal.
  - The value head still learns win or loss, so its estimate stays a probability of winning; it
    is used as the baseline.
- **Everything else as S3:** 1,500 games, temperature 0.5, lr 3e-5, target KL 0.05, no BC anchor;
  evaluations unchanged (seeds, greedy play, 200 games each).
- **Expected, by gen 36:**
  - Won evaluation games are faster: against gen 0, ≤ 30 turns on average (gens 20–27: 32–35);
    against Crazy, ≤ 20.5 turns (gens 0–28: 21.1–22.0).
  - Strength holds: ≥ 60% against gen 0 (gens 19–27: 60–70%) and ≥ 94% against Crazy.
- **Falsifier:** two consecutive generations below 58% against gen 0, or a Crazy result paired-
  significantly worse than gen 29's on the same games. Either means the turn cost is costing
  strength, and I'd halve it.

## Ben vs gen 24 (2026-10-24, 10:48; `runs/games/play_2026-10-24/104827`)
Ben (Imperius) beat gen 24 (Xin-xi) at turn 29, 10 cities to 1, "pretty easily". Gen 24 wins 96% of
its games against Crazy, and Ben plays at about Crazy's level, so beating the bot doesn't mean
playing well against a person.
- **What gen 24 did** (counted from the recording):

  | | Gen 24 | Ben |
  |---|---|---|
  | Techs in 29 turns | 6: Mining, Smithery, Hunting, Forestry, Mathematics, Fishing (turn 20) | 14, incl. Sailing (9), Ramming, Chivalry, Navigation |
  | Units trained | 20 catapults, 12 swordsmen, 10 warriors | 21 warriors, 8 catapults, 4 knights |
  | Navy | no ports, no boats | 4 ports, 16 boat upgrades |
  | Attacks | 38 | 105 |
  | Cities (turns 6 / 12 / 18 / 24 / 29) | 3 / 5 / 5 / 4 / 1 | 3 / 4 / 6 / 7 / 10 |

- **Ben's reading:**
  - It left itself open to knight chains.
  - It had no navy on Lakes, where water control lets you pick off catapults.
  - It fielded a catapult-heavy army without enough front-line units.
  - It expanded suboptimally and didn't exploit new techs.
- **Mine:** it researched very few techs (6 vs 14) and banked into catapults and monuments. Next:
  - measure techs by turn 20, ports and army mix across generations and in the human games (the
    strategy book's indicators);
  - check whether self-play drifted here, or whether it never learned this from the human games.
- **Indicators across generations** (`py/indicators.py`; 400 games each: 400 human championship games
  and the training games played by gens 0, 9, 19, 24 and 29; turn-based figures count only
  player-games that reached that turn):

  | | Human | Gen 0 | Gen 9 | Gen 19 | Gen 24 | Gen 29 |
  |---|---|---|---|---|---|---|
  | Techs by turn 10 / 20 | **4.2 / 9.2** | 3.6 / 7.1 | 3.6 / 7.3 | 3.7 / 7.6 | 3.6 / 7.4 | 3.5 / 7.2 |
  | Cities at turns 5 / 10 / 20 | 2.2 / 3.9 / 5.8 | 2.3 / 3.9 / 5.7 | 2.3 / 3.9 / 5.7 | 2.3 / 3.9 / 5.6 | 2.3 / 3.9 / 5.6 | 2.3 / 4.0 / 5.7 |
  | Stars left at End Turn (turns 3–20) | **3.1** | 4.8 | 5.2 | 5.0 | 5.5 | 5.9 |
  | Port by turn 20 | 88% | 83% | 83% | 83% | 81% | 82% |
  | Front line / ranged share of units | 85% / 13% | 87% / 12% | 79% / 19% | 74% / 24% | 75% / 23% | 73% / 25% |
  | Catapults | 5% | 8% | 13% | 16% | 16% | 17% |
  | Riders | 20% | 16% | 9% | 6% | ~5% | 6% |

  - **Tech pace is inherited, not a self-play drift:** gen 0 was already two techs behind humans by
    turn 20 (R3 imitated the game's AI first), and self-play hasn't closed the gap.
  - **Banked stars grow each generation** (4.8 → 5.9; humans 3.1).
  - **The catapult-heavy army is a self-play drift:** catapults 8% → 17%, riders 16% → 6%.
  - **Expansion and ports match humans.** Ben's game was a bad case for ports, not the norm.

## Human midgames (2026-10-24, afternoon; Ben: "find the most interesting, close, complex midgames")
- **`py/human_midgames.py`** scores the start of every player-turn in the 7,732 human championship
  games, from each player's own observation:
  - **closeness:** scores within 20%, cities within 2, the smaller army at least half the larger;
  - **complexity:** units on the board, units in contact, legal moves, attacks available;
  - **openness:** score-lead changes later in the game.
  - Results: 6,072 games replay exactly to the server's final state. 2,909 of them reach a close,
    contested midgame (turn ≥ 8, both sides with 3+ cities and 4+ units, some contact), giving
    17,836 positions.
- **Training starts:** the best 2,000 by complexity + closeness, at most two per game and 4+ turns
  apart, from 1,515 games.
  - Median turn 15 (10–22), 23 units on the board, 5 in contact.
  - The player to move went on to win 47% of the time, so the starts don't favour either side.
  - Who won the human game isn't part of the ranking, so the set isn't biased toward upsets.
- **`py/human_starts.py`** saves each position as a game state:
  - It replays the game to just before the player's first decision of that turn.
  - Most human games are versions 121–122, so it loads the state marked as the current version
    (new polyhost op 13, `load_current`): play continues under the pinned v123 rules.
  - It checks that the observation and legal list equal the recorded ones, both after the
    conversion and after a plain reload.
  - Games before v121 are skipped.
- **Showcase** (`py/human_showcase.py` → the local watch list, `human_showcase`):
  - 12 games ranked for watching (back-and-forth plus a close, complex position), each opening at
    that position (`#watch=…&turn=…&player=…`).
  - They are other players' games, so they stay local: never on the public site.
- **Finding: 92% of the human championship games are mirror matches** (7,092 of 7,732: both
  players on the same tribe).
  - Our self-play and evaluations draw the two tribes independently, so only ~1 in 12 games is a
    mirror.
  - The web app now draws player 2 in the opposite hue when both tribes match.
- **Ben's decisions:**
  - Keep independent tribe draws ("what tribe you are doesn't make a huge difference anyways").
  - 30% human starts, after seeing a sample: 12 random starts rendered whole-map in a local
    gallery, `runs/human_starts_preview/index.html`.
- **Saved:** 1,715 of the 2,000 positions (285 were from versions before 121), 0 failed checks.
  Tested from these starts: 4 self-play games on the Mac and 6 on the PC under QEMU, all played to
  a finish.

## S3c — S3b plus human midgame starts (pre-registered 2026-10-24, 12:45)
- **Change from gen 37** (after S3b's pre-registered window, gens 30–36):
  - 30% of the training games not against the bot begin from one of the 1,715 human midgame
    states instead of a new map, then play on under the pinned rules.
  - Opponents are drawn as in S3b.
  - The learner sees only how its own games end, never the humans' moves, so the human set isn't
    memorized.
  - The turn cost counts turns from the start position.
- **Unchanged:** everything else as S3b. Evaluations still start from new maps with the same seeds,
  so they measure play from turn 0.
- **Hypothesis:** facing human-style midgames teaches what self-play alone doesn't (a front line,
  boats, faster research) and transfers to games from turn 0.
- **Expected, by gen 43** (in the network's own new-map training games, `py/indicators.py` skips
  human-start games):
  - ranged share of units ≤ 20% (gen 29: 25%), riders ≥ 8% (6%), techs by turn 20 ≥ 7.8 (7.2),
    stars banked ≤ 5.0 (5.9);
  - strength holds: ≥ 60% against gen 0 and ≥ 94% against Crazy.
- **Falsifier:** by gen 43 no indicator has moved halfway to its target (ranged share still
  ≥ 23%, techs by turn 20 ≤ 7.4), meaning human midgames don't transfer to new-map play. Or two
  consecutive generations below 58% against gen 0.
- **Also:** Ben plays the gen-43 network.

## Self-play speed (2026-10-24, 13:00–13:30)
- **Before:**
  - About 440 decisions/s with 20 game processes (QEMU) plus 8 for evaluations, so ~1,800 games an
    hour and ~65 min per generation (49 min of games, 16 of learning).
  - The limit was the one self-play Python process: one core (the GIL), 79% of it spent running
    the networks.
  - That time was mostly per-call overhead: several networks per step, a few positions each,
    hundreds of kernel launches per pass, and a GPU sync in the legal-row selection.
  - The 27 QEMU processes used about 13 of the 28 threads; the GPU was ~51% busy.
- **Changes:**
  - `selfplay --shards`: one child process per 10 game processes.
  - `py/fast_infer.py`: legal rows scored densely, the whole pass captured as CUDA graphs per
    (batch, rows) bucket. Same choices as `Net.forward`: 100% argmax agreement in a benchmark on
    gen-30 games, and checked on the first batch of each network at run time.
  - Per call, 2–8 positions: 15–17 ms → 2.6–3.0 ms. An A/B on 48 games with 8 processes:
    195 → 313 decisions/s.
- **After (gen 32):**
  - About 650 decisions/s with the same 20 + 8 processes (2 shards × ~320).
  - QEMU now uses all 28 threads (load 29), so emulation is the limit.
  - A larger QEMU translation cache (`QEMU_TB_SIZE=1024`) made no difference (57 vs 57
    decisions/s per process). The cost is the emulation itself.
- **Next levers:**
  - A native Windows build of polyhost; it needs the game's Windows files.
  - Less game-logic work per decision (profile polyhost natively on the Mac).
  - A faster learner: it is now ~35% of a generation and runs while the CPU is idle.

### S3b results (window gens 30–36) and S3c so far (2026-10-24, 20:30)
- **Evaluations, gens 30–38** (200 games each):
  - vs previous: 51.5, 50, 45, 48, 46.5, 49, 44.5, 46, 46.5%. Every generation from gen 32 on is
    below 50%, ~46.5% on average: each step loses slightly to its predecessor.
  - vs gen 0: 62.5, 62.5, 62.5, 64, 63.5, 61, 59, 61, 59.5% (gens 22–29 averaged ~65%).
  - vs Crazy: 95–98%.
- **S3b against its pre-registration:** failed.
  - Wins didn't get faster: won games vs Crazy took 21.8 → 23.3 turns (gen 36), 24.1 (gen 38);
    vs gen 0, 31–38 turns.
  - Strength slipped slightly.
  - The falsifier (two consecutive generations below 58% vs gen 0) did not fire.
- **S3c indicators so far** (new-map training games of gens 29 / 33 / 38; human 400–1,200 games):

  | | Human | Gen 29 | Gen 33 | Gen 38 |
  |---|---|---|---|---|
  | Techs by turn 20 | 9.2 | 7.2 | 7.2 | 6.6 |
  | Stars left at End Turn | 3.0 | 6.0 | 6.6 | 7.4 |
  | Port by turn 20 | 88% | 82% | 77% | 56% |
  | Ranged / catapults | 13% / 4% | 24% / 16% | 25% / 19% | 27% / 20% |

  - Every indicator is moving away from the human values.
  - The pre-registered read is at gen 43.

## S3 to gen 48: regression and instability (2026-10-25 morning)
- **Evaluations:**
  - vs previous, gens 39–47: 48, 47.5, 48.5, 47.5, 42, 47.5, 39.5, 48, 39%.
  - vs gen 0: 58.5, 58.5, 59.5, 61, 60, 59.5, 59.5, 61.5, 56.5%.
  - vs Crazy: 93.5–97%.
- **Learning became unstable from gen 40:**
  - The KL limit stopped generations after 250–1,350 of ~8,000 steps (earlier generations ran all
    their steps at KL ~0.015).
  - Policy entropy rose from 1.74 to 2.2–2.3; clip fraction ~0.3.
- **Games turned into grinds:** the mean length went 29 → 42 turns, games of 100+ turns 1 → 114 of
  1,500, units trained per game 57 → 117, and a generation's games took ~85 min instead of ~42.
- **S3c at its read (gen 43):**
  - Ranged share 24 → 20% (met).
  - Riders 6 → 7%. Techs by turn 20 7.2 → 6.3 and stars banked 6.0 → 8.2 (both worse).
  - vs gen 0 60% at gen 43, 56.5% at gen 47.
- **Attribution:** four changes went in within two generations (bot opponents, fixed early
  generations, the turn cost, human starts), so the cause can't be separated. Gen 24 is still the
  best checkpoint.
- **Ben's decision:** stop S3 (`STOP` file set) and branch from gen 29.

## S4 — two branches from S3 gen 29 (pre-registered 2026-10-25, 10:15)
- **Runs:** `~/runs/sp/s4a` and `~/runs/sp/s4b`. Each links S3's gens 0–29 and continues at gen 30
  on half the PC (13 game processes, 4 for evaluations; at night at least 5).
- **S4a (control):** S3's settings. Half the games against itself, half against gen 0 (R7) and the
  last 7 generations; no bot opponents, no turn cost, no human starts.
- **S4b:** S4a plus 30% of the training games starting from the 1,715 human midgame states.
- **Unchanged:** both use the faster self-play (shards, CUDA-graph inference), so S4a also tests
  whether the speed changes play any part in the instability.
- **Evaluations each generation** (200 games each): vs previous, vs gen 0, vs Crazy, and for S4b
  vs S4a's same generation (seeds from 15,000,000; only once the two differ).
- **Expected:**
  - S4a behaves like S3 did up to gen 29: few or no KL early stops, entropy ~1.7–1.8, vs previous
    ~50%, vs gen 0 60–68%.
  - If human starts help, S4b beats S4a's same generation more than 50% of the time from ~gen 34
    and ≥ 55% by gen 38, with indicators closer to the human ones.
- **Falsifiers:**
  - S4b vs S4a ≤ 50% over gens 35–38 (800 games) means human starts don't help; drop them.
  - If S4a also becomes unstable (KL early stops on most generations, rising entropy), the cause
    isn't the S3b/S3c settings. Then look at the shared changes (inference precision, the logp the
    actors record).

### S4 results (2026-10-26, 09:00)
- **S4b vs S4a, same generation** (200 games each):

  | Gen | 30 | 31 | 32 | 33 | 34 | 35 | 36 | 37 | 38 |
  |---|---|---|---|---|---|---|---|---|---|
  | S4b wins | 99 | 103 | 107 | 102 | 107 | 99 | 94 | 86 | 86 |

  - **Gens 35–38: 365/800 = 45.6% (95% CI ~42–49%).** The pre-registered falsifier fired: human
    starts don't help, and late in the window they hurt a little. S4b is stopped (`STOP`).
  - S4a gets S3's full allocation again (28 game processes, 8 for evaluations).
- **Stability:** both branches stayed stable through gen 40.
  - Every generation ran nearly all its steps, at KL 0.010–0.019 and with no early stops.
  - Games average 32–33 turns, with ~1 game of 100+ turns per 1,500.
  - So S3's collapse after gen 40 came from the S3b/S3c settings (Crazy, fixed early generations,
    turn cost, human starts together), not from the speed changes, which S4a shares.
  - Entropy is drifting up in both (S4a 1.55–1.75 at gens 30–36, 1.97–2.00 at gens 39–40); watch it.
- **Plateau:**
  - vs gen 0 (R7): S4a gens 30–40 averaged ~62.6% (59–66.5%); S3 gens 19–29 ~64%.
  - vs previous: ~48% on average.
  - Plain self-play has not improved on R7 by more than ~+90 Elo (64%) since about gen 19, and gen
    24's 70% is still the best single result.

## Architecture review (2026-10-26; Ben: "you obviously are doing this fundamentally wrong")
- **The network makes every decision** one command at a time from the game's legal list (~450 per
  side per game), from a single forward pass.
  - No search or lookahead: it never tries a move in the engine before playing it.
  - Scripted: only the input encoding, the training setup, the 400-commands-per-turn and
    150-turn caps.
- **Self-play reward:** only the final result (1 win, 0 loss). Each of a side's ~450 moves gets the
  same advantage, z − v_old, so per-move credit is very noisy.
- **Plan, in order:**
  1. Credit per move from changes in the win estimate (GAE, S5 below).
  2. Turn planning with the engine: sample several whole turns, judge their end positions with the
     value head, play and learn from the best (proved small first).
  3. A native engine for speed. Steam has a Linux build (depot 874393, ~300 MB) that could run in
     WSL without emulation; Ben has to download it, and its game version must match.

## S5 — credit per move (GAE), branch from S4a gen 41 (pre-registered 2026-10-26, 16:20)
- **Change:** `train_rl --gae_lambda 0.97`, over each player's own decisions.
  - Each move is judged by the change in the acting network's win estimate up to the player's next
    move (the outcome after the last), and A = Σ 0.97^k δ_{t+k}: about a turn of look-back credit.
  - λ = 1 is S4a's outcome-only advantage (unit-checked).
  - On 40 gen-40 games the advantage's sd fell from 0.329 to 0.118: most of the outcome noise is
    explained by the estimate's trajectory.
- **Everything else as S4a;** half the PC each.
- **Evaluations as S4a,** plus S5 vs S4a's same generation (200 games, seeds from 15,000,000).
- **Expected:** S5 beats S4a's same generation ≥ 55% by gen 46 and vs gen 0 rises above S4a's
  ~62%, with stability unchanged (no KL early stops, entropy not rising faster than S4a's).
- **Falsifier:** S5 vs S4a ≤ 50% over gens 44–47 (800 games). Then per-move credit from this value
  head doesn't help; the estimate is too noisy to bootstrap from (it explains only ~14% of outcome
  variance in self-play).

## S6 — does turn planning beat the raw network? (pre-registered 2026-10-26, 16:50)
- **`py/turn_search.py`:** at the start of each of its turns, the planner copies the game into 8
  scratch slots (the engine's save/load).
  - Each slot plays a complete turn with the policy: one plan takes the top move, seven sample at
    temperature 1.
  - Each plan is scored by the value head at the moment the policy chooses End Turn (the position
    after the plan, from the planner's view).
  - The best plan's position is loaded into the real game and its End Turn played.
  - No training: the same network (S4a gen 47) plans for side A and plays raw for side B.
- **Measurement:** 30 seeds × 2 seats = 60 games, planner vs raw gen 47.
- **Expected:** the planner wins ≥ 60%. The value head then ranks whole-turn plans well enough to
  act as a teacher, and the next step is training the policy toward the chosen plans (expert
  iteration).
- **Falsifier:** ≤ 55%. Then the value head is too weak to rank plans. Next would be better
  position evaluation (the opponent's best reply, short rollouts, or more value training) before
  any planning-based training.

### S6 result: turn planning loses (2026-10-27)
- **Planner (8 plans, value-head scoring) vs raw gen 47: 18/60 = 30% (95% CI 20–43%).**
  Falsifier fired.
  - The value head preferred a sampled plan over the top-move plan on 73% of turns (mean estimated
    gain +0.04), and those plans were worse.
  - The search finds positions the value head overrates (sampled at temperature 1, they are off the
    policy's own distribution).
  - The value head can't rank plans as it is. Before trying planning again: plans sampled closer to
    the policy (lower temperature), evaluation after the opponent's reply, and a better-trained
    value head.

### S5 result: credit per move helps (2026-10-27)
- **S5 vs S4a, same generation:** gens 42–47: 97, 104, 106, 117, 118, 124 of 200. Over gens
  44–47: 465/800 = 58.1%; the falsifier (≤ 50%) did not fire, and the expectation (≥ 55% by gen 46)
  was met.
- **vs gen 0:** S5 63, 64.5, 65, 63, 67.5, 66% (gens 42–47); S4a 58, 60.5, 63, 63, 56, 63%.

### S4a collapsed too; the entropy bonus suspected (2026-10-27)
- **S4a (plain S3 settings):**
  - vs gen 0: 56, 54.5, 54, 49.5, 51, 44% at gens 48–53.
  - KL early stops at gens 50, 51, 52, 54, 56.
  - Entropy 2.0 → 2.8.
  - Stopped (`STOP`).
- **S5's entropy also rose** (1.84 → 3.13 at gen 46, then 2.5–2.9). Its training games grew
  slower (gen 48's took 3.6 h on 7 processes). Greedy evaluations hide this.
- **Hypothesis:**
  - Once the self-play signal averages to noise (a policy about as strong as its opponents), the
    normalized policy-gradient term cancels on average.
  - The entropy bonus (0.003) doesn't cancel, so entropy climbs every generation.
  - This fits every branch going bad 10–15 generations after its progress stalls.

## Native engine on the PC (2026-10-27)
- **The Steam Linux build** (depot 874393, build 2.17.3.16375; Ben downloaded it on the Mac) runs
  polyhost natively in WSL: `tools/setup_pc_linux.sh`, `POLYHOST=linux`. Built with Zig's C++
  compiler from the PC's Python environment (WSL has no g++).
- **Parity:** `py/parity_hosts.py` (12 games, 25 turns, half against Crazy) gives output identical
  to QEMU's: every legal list and final state.
- **Speed:** the same 12 games took 5 s natively vs 38 s under QEMU, on a loaded PC.

## S5b — S5 on the native engine, entropy bonus 0 (pre-registered 2026-10-27, 17:30)
- **Change:** from gen 48's learning step, `train_rl --entropy 0` (was 0.003). Speed only: the
  native engine, the whole PC.
- **Expected:**
  - Entropy stops rising: ≤ 2.5 at every generation from gen 50, trending down.
  - Strength holds or improves: vs gen 0 ≥ 63% (gens 49–54).
  - KL early stops stay rare.
- **Falsifier:** entropy still rising (above 3.0 by gen 54) or vs gen 0 below 58% twice in a
  row. Then the bonus isn't the cause; next suspects are the noise-normalized advantages and
  actor/learner precision.

## S7 — four tracks in rotation from S5 gen 49 (pre-registered 2026-10-27, 18:45)
- **Ben:** "run a few tracks to experiment with what works best … you may switch off
  periodically."
- **Rotation:** `tools/tracks.sh` runs one generation at a time (games, learning, evaluations) of
  whichever track has the fewest, so the tracks share the GPU in turn and stay in step. All four on
  the native engine with the whole PC; ~1 h per track-generation, so each track gains ~6
  generations a day.
- **Tracks** (all with credit per move and no entropy bonus):

  | Track | Change |
  |---|---|
  | A `s5` | the base: GAE λ 0.97, entropy 0 (continues S5b) |
  | B `s5v` | value weight 1.0 (was 0.25): a better win estimate, for credit and later for planning |
  | C `s5l` | GAE λ 0.9: credit ~10 decisions back instead of ~30 |
  | D `s5k` | KL anchor to gen 0 (R7), weight 0.03, on the RL batch's own positions |

  - D's anchor replays no human games.
  - At weight 0.1 the anchor pulled too hard in a test (KL to gen 0 1.10 → 0.95 within 100 steps),
    hence 0.03.
- **Evaluations each generation:** vs previous and vs gen 0 (and Crazy every third). B, C and D
  also play A's same generation (200 games).
- **Read at gen 56** (7 generations after the branch):
  - A track is better than A if it wins > 55% of its games against A over gens 53–56 (800 games),
    worse if < 45%.
  - Also compared: entropy trends (S5b's falsifier: entropy above 3.0 by gen 54) and vs gen 0.

## S8 — track E: varied human starts, tens of thousands (pre-registered 2026-10-27, 19:40)
- **Ben:** human-position starts should stay in training, varied carefully: not a small set, at
  least tens of thousands, so none is memorized.
- **`py/human_positions.py`:**
  - Up to 16 positions per exactly-replaying human game, any version.
  - Positions spread over the game's early, middle and late thirds, at least a turn apart.
  - Left out: decided positions (a player without cities, or a score below 40% of the other).
  - Result: 69,645 positions from 5,994 games (median turn 9; early 21%, middle 41%, late 38%).
- **`py/human_starts.py` on the PC's native engine:** 14 shards, ~10 minutes.
  - 53,272 states saved from 4,926 games, each checked to show the same observation and legal list
    as the recording under the current rules.
  - ~16k positions dropped, mostly older versions whose legal lists differ now; 4 games crashed the
    engine (a null reference in GetMovement) and were skipped.
  - Location: `~/polydata/human_starts_big` (769 MB).
- **Track E `s5h`:** the base track plus 30% of non-bot training games from these states.
  - Same share as S4b, so the comparison isolates the set: 1,715 close midgames there, 53,272
    varied positions here.
  - Each position comes up about once in ~170 generations.
- **Read with S7 at gen 56:** better than A if > 55% vs A over gens 53–56, worse if < 45%.

## The human game with the most kills (2026-10-27, for Ben)
- **Candidates:** 6,072 exactly replaying games, kills per game median 15.
- **Evenly matched:** mean score ratio ≥ 0.8 from turn 5, and the fewer kills at least 60% of the
  more.
- **Most kills: game 5da0cb57** (Zebasi mirror, v122).
  - 403 units killed (239–164) in 49 turns.
  - Scores within 15% on average (never below a 0.71 ratio); 3 city captures.
  - Player 1 won when player 2 resigned.
  - Kills by turns 20/30/40: 45, 139, 277; the busiest five turns, from turn 32, had 88.
- Local watch list `human_most_kills`, opening at turn 26. Other players' game: not published.

## S7 and S8 results (2026-10-30)
- **The read at gen 56** (gens 53–56 vs track A's same generation, 800 games each):

  | Track | vs A, 53–56 | vs A, 57–64 | vs A, 65–71 | vs gen 0, 65–72 | vs Crazy, 60–72 | Entropy at the end |
  |---|---|---|---|---|---|---|
  | A `s5` (base) | — | — | — | 68.5% | 97.3% | ~2.0, flat |
  | B `s5v` value weight 1.0 | **35.5% (worse)** | 27.0% | **18.0%** | 53.5% | 93.6% | 1.83 |
  | C `s5l` λ 0.9 | 49.8% (no difference) | 48.8% | 44.6% | **72.4%** | 97.8% | 1.56 |
  | D `s5k` KL anchor 0.03 | 49.4% (no difference) | 46.8% | **55.5%** (59% at gen 71) | **72.9%** | 97.0% | 1.65 |
  | E `s5h` 53k human starts | 49.0% (no difference) | 47.5% (gen 57 only) | stopped | — | — | 1.92 |

  - At the pre-registered read, only B differed from A (worse). D pulled ahead later, the only
    track still gaining on A. C beats gen 0 by the most but loses to A: a non-transitive result.
  - B kept falling: a value weight of 1.0 on the shared trunk damages the policy.
  - KL early stops: B at gens 51, 54, 55, 57; C at 54; D at 50–51 (the anchor's first
    generations); E at 55; A none.
- **S5b (entropy bonus 0), on track A:** entropy stayed ≤ 2.5 from gen 50 (2.2 → 1.9 by gen 56,
  then 1.9–2.2), and vs gen 0 held at 64–73% (gens 49–72). Expectation met, falsifier not fired:
  the bonus caused the rise. Strength held but didn't improve.
- **E stopped at gen 58** (2026-10-29 03:26): a game from a human start ended before the learner
  made a recorded move, and saving an empty game crashed (`np.stack` of nothing). The retries
  replayed the same game, so three failures stopped the track. Fixed: such games are counted, not
  saved.
- **The policy's value head memorizes outcomes within a pass.** Its stream loads 16 games at a time
  and yields their decisions in ~55 consecutive batches, each with ~16 positions of every game.
  - On 24 test games its training loss fell 0.22 → 0.01 within 150 steps.
  - A separate value network on a decorrelated stream (25% of each game's positions, 64 games
    shuffled together) stayed at an honest 0.6–0.8 against the outcome on the same games.
  - The base's recorded `v_old` is still out-of-sample (recorded before the games were learned
    from), but what the head learns is fitted to memorized outcomes. That may explain why it
    explains so little outcome variance and misled the planner (S6).
- **B stopped** (`STOP`, 2026-10-30), and the S7 rotation ended after B's gen 72 for S9 (`TRACKS_STOP`).

## S9 — five tracks from D gen 71 (pre-registered 2026-10-30, 21:30)
- **Why:** the restrategy in [superhuman-plan.md](superhuman-plan.md).
  - PPO tweaks have stopped paying.
  - The value function is what blocks search.
  - Nothing measures exploitability or play from human-style positions.
  - Ben approved it: "Keep human starts in, they can't hurt."
- **Base:**
  - Track D (`s5k`) at gen 71: GAE λ 0.97, entropy 0, KL anchor to gen 0 (R7) at 0.03, value weight
    0.25, lr 3e-5, KL limit 0.05, temperature 0.5, 1,500 games per generation.
  - Pool: self 0.5, plus gen 0 and the last 7 generations.
  - Plus 30% of non-bot games from `human_starts_big`, without the human held-out games
    (`train_bc.is_val`, CRC32 % 10 == 0): 47,829 of 53,272 positions.
  - Settings: `tools/s9_setup.sh` and each run's `override.json`.
  - Rotation: `tools/tracks.sh`.
  - Each track stops at gen 83 (`LAST`).

  | Track | Run | Change from the base |
  |---|---|---|
  | 1 Control | `s9c` | none |
  | 2 Critic | `s9v` | `train_rl.py --critic`: a separate value network, started from the gen-71 weights. Before each step it scores every decision, and those estimates replace `v_old` in the advantages. It learns on its own optimizer (lr 1e-4): BCE against the λ-return (λ 0.95) plus 0.2 × ownership. Its stream is decorrelated: 25% of each game's decisions per pass, 2 passes, 64 games shuffled together. The data is this generation's and the previous generation's games. The policy's own value head trains as before |
  | 3 Magnet | `s9m` | `--anchor_every 6`: the anchor (weight 0.03) is the run's own generation at the last multiple of 6 below the one being learned (gen 66 for gen 72, gen 72 for gens 73–78, gen 78 for gens 79–83) instead of gen 0 |
  | 4 League main | `s9l` | Pool: self 0.5, exploiter `s9x`'s 2 latest generations 0.25, gen 0 + the last 3 generations 0.25 |
  | 5 Exploiter | `s9x` | Plays only `s9l`'s latest generation. No anchor, no human starts. Reset to `s9l`'s latest when it beats `s9l` ≥ 65% (same generation, 200 games) or 8 generations after the branch or the last reset (`resets.tsv`) |

- **Deviations from superhuman-plan.md §4:**
  - The branch is at gen 71, so the read is at gen 83.
  - The critic's window is 1 earlier generation, not 4, to keep learning time down.
  - The critic gets its own sampled stream, after the memorization finding above.
  - The held-out human positions use the human data's own held-out split (10%, not 5%).
  - Magnet: gen 71 isn't a multiple of 6, so gen 72's anchor is gen 66; from gen 73 on it is the track's own.
- **Evaluations each generation:**
  - 200 games each, seats swapped, greedy: vs previous and vs gen 0, Crazy every third
    generation.
  - Tracks 2–4: vs `s9c`'s same generation (seeds from 15,000,000).
  - Track 5: vs `s9l`'s same generation.
  - Every third generation, tracks 1–4: vs gen 0 from 100 held-out human positions (one per game,
    `selfplay.py --starts … --starts_split val`, each from both seats, seeds from 17,000,000).
    Tracks 2–4 also play `s9c` from them.
  - Strategy indicators of every generation's training games: `indicators.tsv`.
- **Read at gen 83:**
  1. **Head-to-head** vs `s9c` over gens 80–83 (800 games): better > 55%, worse < 45%.
  2. **Exploitability:** a fresh exploiter for 4 generations against each of tracks 1–4's gen 83
     (initialized from it, playing only it, no anchor), then 200 games. A difference of ≥ 10
     points between tracks counts.
  3. **Human positions:** the h evaluations pooled over gens 72–83.
  4. **Indicators** vs the 400 human games.
  5. **Critic quality:** value log loss and accuracy by game phase, on held-out games of `s9c`
     and on 150 held-out human games, the critic vs `s9c`'s value head.
  6. **Planner re-test** (S6 protocol, 60 games, plans at temperature 0.5): `s9v` gen 83 with its
     critic scoring plans, and the control's head on `s9c` gen 83, each vs its own raw network.
- **Expected and falsifiers:**
  - **Critic:**
    - Expected: log loss ≥ 0.02 lower than the control's head on held-out games; ≥ 55% vs
      `s9c`; planner ≥ 60%.
    - Falsifier: log loss not lower, or planner ≤ 55%.
  - **Magnet:**
    - Expected (my guess): ≤ the control.
    - Readings: ≥ 55% means a moving magnet beats the fixed human prior; ≤ 45% means keep the
      fixed anchor; in between means keep the fixed anchor (simpler).
  - **League:**
    - Expected: ≥ 50% vs `s9c`, exploitability ≥ 10 points lower than `s9c`'s, and better from
      human positions.
    - Falsifier: exploitability not lower, or < 45% vs `s9c`.
  - **Exploiter:**
    - Expected: > 60% vs `s9l` within a few generations of each reset, falling as `s9l` adapts.
    - If it never passes 55%, the exploiter finds nothing at this budget.

## Determinized-state builder v1 (2026-10-30; Ben approved it with S9)
- **What it is:** `polyhost/determinize.hpp`, wire op 14, `polyenv.Env.determinize(src, dst, viewer, seed)`.
  - It writes a fog-safe sample of a match into another slot, so a search never uses what the
    player can't see.
  - Kept: what the viewer's client shows.
  - Replaced or removed:
    - **Units:** invisible enemy units and every unit on an unexplored tile are removed.
    - **Unexplored tiles:** terrain, climate, resources and unowned villages and ruins come from a
      map generated with the sample's seed.
    - **History and randomness:** the command history is emptied, and a new random seed changes
      ruins and every later draw.
    - **Opponent techs:** inferred from visible evidence: the tribe's starting techs, the techs
      behind units it trained and improvements it built, and their prerequisites.
    - **Opponent stars:** a banked-stars prior of 3.
    - **Enemy units:** xp and creation turn set to 0, and every unit renumbered.
    - **AI memory:** reset.
  - **Known leak:** enemy cities on unexplored tiles are kept where they are (0.7–1.3 per state in
    bot games). Not yet sampled: units in the fog.
- **Test** (`polyhost detertest`, bot-vs-bot games, the current player as viewer): 1,899 states
  (8 games, seed 11) and 715 (6 games, seed 3).
  - The viewer's observation is unchanged: 0 failures.
  - The output is a fixed point of the serializer: 0 failures.
  - Hidden changes (tiles, units, stars, techs, seed, unit ids; the kept cities aside) don't reach
    the output: 0 of 1,899.
  - The built-in AI plays both seats two turns on the sample: 0 failures of 1,899.
  - No inferred tech the opponent doesn't hold: 0.
  - The rules don't read the stripped history: 0 of 472 (80 random commands from the state and from
    its stripped copy).
  - Python: same observation and legal list, same seed → same bytes.
- **Found while building it:**
  - A city's owner is the tile's: `ImprovementState.owner` is always 0, and `founder` says who
    built an improvement.
  - The action stack is not a history. It holds the last command's actions, and a pending city
    reward reads it for its score, so it is kept when the actions are the viewer's.
  - Units obtained without their tech: swordsmen and ships from ruins, giants from city level-ups,
    the starting unit, and converted units (which keep their birth tribe).
  - The opponent's banked stars: one turn of income double-counted, since income arrives at its turn
    start anyway, so the builder uses a prior.
- **Next:**
  - Build it into the PC's Linux polyhost, as a new binary, before the S9 read's planner re-test.
  - Then expert iteration on samples (superhuman-plan.md pillar 3).

### S9 read, part 1 (2026-11-02): head-to-head, human positions, indicators
- **All five tracks reached gen 83** (2026-11-01 08:07; ~3.3 h per round of five). There were no KL
  early stops, and entropy stayed at 1.6–1.7 on every track (the exploiter 1.5–1.6).
- **Head-to-head vs `s9c`** (200 games per generation):

  | Track | Gens 80–83 (pre-registered) | Gens 72–83 | vs gen 0, 72–83 | Human positions vs gen 0 | Human positions vs `s9c` | vs Crazy |
  |---|---|---|---|---|---|---|
  | `s9c` control | — | — | 75.1% | 64.5% | — | 96.9% |
  | `s9v` critic | 54.8% ± 3.4 | 52.2% ± 2.0 | **78.4%** | 64.8% | 51.6% | 97.6% |
  | `s9m` magnet | 53.4% ± 3.5 | 52.3% ± 2.0 | 76.5% | 66.6% | 52.2% | 98.4% |
  | `s9l` league | 50.9% ± 3.5 | 50.1% ± 2.0 | 76.8% | 64.8% | 48.5% | 97.8% |

  - By the pre-registered bar (> 55% better, < 45% worse), no track differs from the control.
  - The critic is closest: 111, 108, 112, 110, 108 of 200 at gens 79–83 (~55%), from ~50% at gens
    72–78.
  - The magnet beat my guess (≤ the control) but didn't clear the bar. Its reading is "in between":
    keep the fixed anchor.
- **Exploiter `s9x`:** vs `s9l`'s same generation 87–109 of 200, never above 55% (best 54.5% at
  gen 75). It was reset by the 8-generation rule at gen 80, never by the 65% rule. Pre-registered
  reading: the exploiter finds nothing at this budget. The read-time exploiters (part 2) say whether
  the tracks are that robust or the exploiter too weak.
- **The critic's own training loss against the outcome** (on its sampled stream) fell 0.47 → 0.31
  over the 12 generations. Its held-out quality is part 2.
- **Indicators** of gen 83's training games (humans: techs by turn 20 9.2, stars banked 3.1,
  catapults 5%):
  - techs by turn 20: 6.6–7.2;
  - stars banked: 5.6–6.5;
  - catapults: 16–20%;
  - port by turn 20: 71–83%.
  - No track moved the strategic gaps.
- **Every track vs its previous generation** averaged ~47–49% while vs gen 0 stayed flat: the
  generations cycle rather than climb.
- **Part 2 is running:** fresh exploiters `e9c`, `e9v`, `e9m`, `e9l`, 4 generations against each
  track's gen 83 (`tools/s9_exploit.sh`). Then the critic's held-out quality and the planner re-test.

### S9 read, part 2 (2026-11-02): exploitability; and the two no-training tests
- **Fresh exploiters** (`tools/s9_exploit.sh`): each starts from a track's gen 83, plays only it,
  4 generations of 1,500 games, no anchor. Exploiter vs its target, 200 games per generation:

  | Target | Gen 1 | Gen 2 | Gen 3 | Gen 4 |
  |---|---|---|---|---|
  | `s9c` control | 46% | 45.5% | 47.5% | **48.5%** |
  | `s9v` critic | 50% | 46.5% | 56% | **53%** |
  | `s9m` magnet | 47.5% | 44.5% | 43% | **47%** |
  | `s9l` league | 49% | 50.5% | 56.5% | **53.5%** |

  - Spread at gen 4: 6.5 points, under the pre-registered 10, so the tracks don't differ in
    exploitability. None of the exploiters gained anything in 4 generations either.
  - With `s9x`, that is five PPO exploiters that never passed ~56%, while Ben beat gen 24 easily.
  - Reading: PPO with this budget is too weak an optimizer to find the holes. These numbers are
    not evidence of robustness. A search-based exploiter is the next probe (radical-ideas.md A3).
- **Engine-only throughput** (`py/engine_bench.py`, 28 native processes, random legal play):
  - 6 slots per process: **16,991 decisions/s**; 24 slots: 15,919/s. Observations take 4% of the
    time.
  - Self-play runs at ~2,200/s, so the network costs ~87% of the time: self-play is ~8× below
    what the engine can serve.
  - The B3 kill criterion (< 6,000/s) did not fire. Throughput is a real lever, through inference
    batching (~24 positions per batch now) and less compute per decision.
  - (Random play reaches simpler states than real games, so the true ceiling is lower; to measure.)
- **Weight soups** (`py/soup.py`):
  - **Control gens 76–83, uniform mean:** vs `s9c` gen 83 197/400 = **49.3%** (CI 44–54), vs gen 0
    75%. The B4 falsifier fired: averaging one track's cycling generations gains nothing.
  - **The four S9 finals, mean (one parent, gen 71):** vs `s9c` gen 83 221/400 = **55.3%** (CI
    50.4–60.0), vs gen 0 75%.
    - About as strong as the best track (`s9v`, 54.8%), with no training at all.
    - A small, unconfirmed gain. Re-test it against each track's gen 83 before using it.

### S9 read, part 3 (2026-11-02): the planner re-test (radical-ideas.md A1)
- **Protocol** (`turn_search.py --reply`):
  - 16 plans per turn: the top-move plan and 15 sampled at temperature 0.5;
  - each made on a fog-safe sample and replayed on a second one;
  - after each plan's End Turn, the opponent's whole reply played greedily by the planner's own
    network from the opponent's view;
  - scored by the planner's chosen value at its next turn, averaged over the 2 samples;
  - the best plan played in the real game.
- **Result:** `s9v` gen 83 planning with its critic (`critic_g083.pt`) vs `s9v` gen 83 raw:
  **164/200 = 82.0% (95% CI 76.1–86.7%)**, seeds from 16,000,000 (100 × 2 seats). The
  pre-registered bar (≥ 60%) is passed by far: search is unblocked.
- **Caveat, the reason for the follow-ups:** here the opponent is the very network the planner
  uses to simulate replies, so every greedy reply is predicted exactly (apart from the hidden part).
  Against an opponent it doesn't model, the gain may be much smaller.
  - Queued: the same planner vs `s9c` gen 83, vs gen 0 (R7, human imitation) and vs Crazy.
  - Also queued: `s9c` gen 83 planning with its own value head (the scorer comparison).
- **Second caveat:** the determinizer keeps enemy cities on unexplored tiles in place (the known
  leak), so part of the gain may come from that. To measure later.
- **Batching** (`selfplay.py`, `s9c` gen 83 self-play, 600 games, 3 shards):
  - 6 slots per process: 3,104 decisions/s, 20 positions per batch;
  - 16 slots: 3,530/s, 33 per batch;
  - 32 slots: 3,501/s, 29 per batch.
  - More slots barely help: each shard is a single Python process. The next lever is more shards
    (more inference processes) or encoding once per turn (radical-ideas.md C1).

### S9 read, part 4 (2026-11-02): the planner against opponents it doesn't model
- **The same planner** (`s9v` gen 83 + `critic_g083.pt`, 16 plans, 2 fog-safe samples, temperature
  0.5; replies modelled by `s9v` itself), 200 games each:

  | Opponent | Planner | Unplanned `s9v` gen 83 vs the same opponent |
  |---|---|---|
  | `s9v` gen 83 itself | 82.0% (76–87) | 50% |
  | `s9c` gen 83 | **85.5%** (80–90) | ~55% |
  | gen 0 (R7) | **94.0%** (90–97) | ~78% |
  | Crazy | 154 of 155 finished (the run ended early) | ~97% |

  - The gain holds against opponents the reply model doesn't match: about +30 points (~+250 Elo)
    against similar networks.
  - **The scorer matters:** `s9c` planning with its own value head beat itself 72.5% (66–78), vs
    82% with the S9 critic.
- **More inference shards** (`selfplay.py --shards`, 600 games, 28 processes, 6 slots):
  - 3 shards 3,104 decisions/s; 7 shards **3,559/s**; 14 shards 2,929/s.
  - About 3,500/s is the ceiling with one network call per decision.

## S10 — four tracks from the critic track's gen 83 (pre-registered 2026-11-02, 21:10)
- **Ben's decisions (2026-11-02):**
  - **Tracks:** the privileged critic, turn-clock credit, search in the loop and a control.
    "Make sure you are evaluating against the new hard coded model."
  - **Base:** `s9v` gen 83.
  - **The planner everywhere:** in his games and in evaluations.
  - **The fresh human pass** (~20M parameters, strategy conditioning, the Might Continents games
    as pretraining) starts now, beside S10.
  - **Weight edits** may go into the agent when measured.
  - **His calibration games** come later. The ladder starts at Crazy with its own handicap.
  - **The evaluation bot is rebuilt on the built-in Crazy AI** (full view allowed), as an
    evaluation opponent only.
- **Base:** `s9v` gen 83 and its fog critic `critic_g083.pt`.
  - Settings: GAE λ 0.97, entropy 0, KL anchor to gen 0 (0.03), lr 3e-5, KL limit 0.05,
    temperature 0.5, 1,500 games per generation.
  - Starts: 30% of non-bot games from human positions (minus the held-out games).
  - The fog critic trains in every track (search needs it).
  - Throughput: 7 inference shards.
  - Setup: `tools/s10_setup.sh`.
  - Each track stops at gen 95 (`LAST`).

  | Track | Run | Change from the control |
  |---|---|---|
  | Control | `s10c` | none: the fog critic drives the advantages (`s9v`'s recipe) |
  | Privileged critic | `s10p` | `--pcritic`: a training-only critic that also sees the opponent's own view (recorded per decision) drives the advantages. It starts from the fog critic, with the opponent embedding at zero |
  | Turn-clock credit | `s10t` | `--turn_lambda 0.9`: credit up to the start of the player's next turn (after the reply), decaying per turn instead of per decision (λ = 1 within a turn) |
  | Search in the loop | `s10x` | expert iteration with the planner. To be built, then pre-registered as an addendum before it starts |

- **Evaluations:**
  - each generation, as S9: vs previous, vs gen 0, the rival (`s10c`), from human positions;
  - every third generation: vs Crazy, and vs the scripted bot (`py/script_eval.py`, 100 games). That
    is RuleBot until the improved Crazy-based bot is ready, then that one.
  - at the read, the planner versions of each track's gen 95 (its own fog critic as scorer) vs each
    other and vs gen 0.
- **Read at gen 95:** tracks vs `s10c` over gens 92–95 (800 games): better > 55%, worse < 45%.
- **Expected and falsifiers:**
  - **Privileged critic (`s10p`):**
    - Expected: > 55%, with its critic's training loss against the outcome clearly below the fog
      critic's.
    - Falsifier: ≤ 50% and no lower loss.
  - **Turn-clock credit (`s10t`):**
    - Expected: ≥ 52%.
    - Falsifier: < 45%.

### S10 addendum: `s10x`, search in the loop (pre-registered 2026-11-02, 21:45, before it started)
- **Hypothesis:** the planner's gain (about +30 points over the raw network, S9 read parts 3–4) can be
  moved into the raw network by expert iteration, if the learning target is a damped policy
  improvement and not a copy of the best lane.
- **kevinz917's failure, and what is different here** (radical-ideas.md section 0: his students'
  raw play equalled the parent's, and search on top of them gained +54 to +67 Elo against the
  parent's +104):
  - **The max of noisy lanes.** He trained toward the best of several lanes. The best of 15 noisy
    scores is biased toward lucky noise (the optimizer's curse). Here the lanes are not far apart:
    in the scratch test the sampled lanes' scores had a standard deviation of 0.019 per turn (median
    0.011), and a lane's score differed between its 2 samples by a standard deviation of 0.009
    (median 0.004; measured, 127 turns). That is only the determinization part of the noise; the
    critic's own error comes on top.
  - **Narrowing.** Copying the argmax makes the policy sharper, so later search has worse lanes to
    choose from (his search gain on the students halved).
  - **A soft target instead.** The real game plays a lane drawn from the 15 sampled lanes with
    probability softmax(score / τ). Those lanes are draws of whole turns from the policy π, so the
    drawn turn is approximately a draw from π'(turn) ∝ π(turn) · exp(Q(turn) / τ). That is the
    KL-regularized improvement of π under the planner's scores: the form of Gumbel MuZero's
    completed-Q target (softmax(logits + σ(Q))) and of MPO's E-step. Lanes within the noise of each
    other get near-equal weight, so on flat turns the target is π itself (no move in expectation). A
    lane clearly ahead, or one that wins the game, dominates. τ → 0 is his argmax.
  - **A damped step.** The planner's decisions get a constant advantage +1 in PPO's clipped
    objective. At π = π_old its expected gradient is −∇KL(π' ‖ π). The clip stops it once a row's
    probability is 1.2 × the acting policy's: the same trust region as every other decision, and
    on flat turns (π' ≈ π) the expected gradient is zero, so it doesn't sharpen the policy there.
  - **Online, not offline.** He refit students once on search labels. Here the searched turns are
    played in the training games every generation, so the critic and the outcomes see that play too.
- **τ = 0.02, set from the scratch test before the start** (24 + 16 games, measured):

  | τ | turns | drawn lane − mean sampled lane | best lane − mean sampled lane | KL(weights ‖ uniform), mean / median |
  |---|---|---|---|---|
  | 0.05 | 127 | 0.015 (49% of the best's) | 0.030 | 0.115 / 0.025 nats |
  | 0.02 | 189 | 0.017 (67%) | 0.025 | 0.32 / 0.044 nats |

  - At 0.02 the drawn lane keeps two thirds of the planner's estimated gain over the policy's
    average turn, while the median turn's weights stay near uniform (0.044 nats).
  - The mean KL, 0.32 nats per turn of ~16 commands, is about 0.02 per decision (derived). That is
    below PPO's 0.05 limit per generation, so the clip, not τ, bounds the step.
- **Config:** identical to `s10c`: the same base (`s9v` gen 83 and its fog critic), the same
  override.json apart from the keys below, the same evaluations (rival `s10c`). Run dir
  `~/runs/sp/s10x`, port 9850, `LAST` 95, setup `tools/s10x_setup.sh`.
  - **One infrastructure change:** 4 inference shards instead of 7. With the planner, a shard
    (4 game processes) holds 1.92 GB of GPU memory instead of 1.10 GB, and one with 7 processes holds
    2.37 GB (torch's peak reserved, measured). Seven of them, with their CUDA contexts, would need
    ~16 GB beside the human pass's training (derived). Shards only split the games across inference
    processes; the games and the learning don't change.

  The changes:
  - **Training games** (`selfplay.py --search_share 0.25`, `py/search_loop.py`):
    - A fixed draw per (game seed, turn, player) picks 25% of the learner's turns (both seats in
      self-play games, the learner's seat otherwise).
    - At the start of such a turn, the planner of S9 read part 3 runs on scratch slots of the same
      game process:
      - 16 lanes: the top-move lane and 15 sampled at T 0.5 from the actors' own (bf16) policy;
      - 2 fog-safe samples;
      - End Turn, then the opponent's whole reply, played greedily by the learner's network from the
        opponent's view of the sample;
      - scored by the track's fog critic of the previous generation (`critic_g(g−1).pt`, in fp32).
    - The drawn lane is played in the real game through the normal loop, command by command. Its rows
      are forced, and the log-probability recorded is the acting policy's for that row at the real
      observation. A command not legal in the real game (its hidden part differed) ends the plan, and
      the rest of the turn is sampled from the policy as usual.
    - Recorded per decision: `searched`. Per searched turn: the scores (top-move lane, drawn lane,
      best, mean), KL(weights ‖ uniform), lane spread and sample-to-sample noise, commands, broken,
      seconds.
    - The training games run on `polyhost-det` (the same build plus the determinizer, used in the S9
      planner tests). The evaluations keep `polyhost`.
  - **Learning** (`train_rl.py --search_adv 1.0`, in `learn_extra`):
    - The planner's decisions get the constant advantage +1 in the clipped objective (above).
    - **The PPO ratio stays honest:** these decisions get no GAE advantage. They are left out of the
      advantage normalization, and the KL early stop (0.05) is measured on the sampled decisions only
      (`kl`; theirs is logged as `kl_search`).
    - Every other decision is unchanged: GAE λ 0.97 from the fog critic, clip 0.2, anchor 0.03 to
      gen 0.
    - The fog critic trains on every decision, searched or not, as in `s10c`.
  - **Fog:** lanes, replies and scores all run on samples built from the learner's own view. The
    scorer is the fog critic, and nothing reads the real hidden state.
- **Read at gen 95:**
  1. **As S10:** `s10x` vs `s10c` over gens 92–95 (800 games): better > 55%, worse < 45%.
  2. **A4 kill test, raw play:** below 52% in (1) means search in the loop didn't reach the raw
     network.
  3. **A4 kill test, search on top:** the S9 part 3 planner (16 lanes, 2 samples, T 0.5, the track's
     own `critic_g095.pt`; seeds from 16,000,000, 200 games), on `s10x` gen 95 vs raw `s10x` gen 95,
     and the same on `s10c` gen 95. If `s10x`'s planner gain is less than half of `s10c`'s, the
     policy narrowed the way kevinz917's did. (The planner-vs-planner games of the S10 read compare
     the two directly.)
  - **Indicators each generation** (the games log): searched turns, best − top-move lane, drawn −
    top-move lane, KL(weights ‖ uniform), broken plans, and the policy's entropy (learn log). If the
    policy absorbs the search, best − top-move should shrink slowly over the generations.
- **Expected (guessed):** 55–60% vs `s10c` over gens 92–95, and the planner's gain on `s10x` at least
  70% of its gain on `s10c`.
- **Falsifier:** below 52% vs `s10c`, or the planner's gain on `s10x` below half of its gain on `s10c`.
- **Cost:**
  - Scratch tests (measured): 3.3–4.0 s per searched turn per game-process thread.
    - One shard of 4 processes, 40 games: 83 decisions/s with the planner, 289/s without.
    - One shard of 7 processes, 56 games: 118 decisions/s with the planner.
    - Both small runs include a long tail of last games, so they understate throughput.
  - `s10c`'s gen 84 games held ~62k learner turns, so 25% is ~15k searched turns per generation
    (derived).
  - Guessed: a generation's games take ~30–40 min in the daytime (28 processes) and ~60–70 min at
    night (14), against ~10 and ~20 min for `s10c`.
  - **First generation** (gen 84, started 2026-11-02 23:12 at night: 14 processes, 4 shards, beside
    the human pass's training on the GPU):
    - First 50 games per shard (measured): 91–95 decisions/s per shard, 4.3–4.7 s per searched turn,
      best − top-move lane 0.025, drawn − top-move lane 0.016–0.017, KL(weights ‖ uniform) 0.39–0.41,
      broken plans 5%.
    - The planner chose 25.5% of the recorded decisions (first 60 games).
    - Derived: ~370 decisions/s over 4 shards, so its ~1.3M decisions take ~1 h at night. That is
      ~3× `s10c`'s night rate (~1,150/s, half its daytime 2,288/s, guessed).
    - With learning (~20–50 min) and evaluations (~35 min), an `s10x` generation is ~2 h at night
      (guessed).
  - Every round of the S10 rotation grows by one `s10x` generation.
- **Default behaviour of the shared files is unchanged** (checked on the PC before deploying):
  - the same match with the old and new `selfplay.py` gave identical results (6 games);
  - default training games have the same fields as `s10c`'s;
  - `train_rl.py` old and new on the CPU gave bit-identical policy and critic weights (6 games).

## HP1 — the fresh human pass: a 22M network with strategy conditioning z (pre-registered 2026-11-02, 21:30, before the 21:33 launch)
- **Ben (2026-11-02):** "Now, beside S10"; ~20M parameters; strategy conditioning
  (radical-ideas.md B5); the Might Continents games as pretraining, then the championship games;
  train once on the human set (no anchor that replays it).
- **Hypotheses:**
  1. **Controllability (B5):** a network trained with z (the player's own build facts of that game)
     follows a z it is given at play time: tech tempo, catapults and navy move toward the z.
  2. **Size (B6):** a 22M network trained on the same data imitates people better than R7 (5.9M)
     and plays at least as well.
- **z (`py/zfacts.py`), per player per game, from the game's record only (no judgement):**
  - the first 6 techs researched, in order; the first 10 units trained, in order;
  - techs by turn 10 and by turn 20; cities at turn 10; the turn of the first port and of the first
    naval unit (with "had one" flags); units trained by turn 20, per class (warrior, rider, archer,
    defender, catapult, swordsman, knight, giant, mind bender, cloak/dagger, other) and ship
    upgrades by turn 20. Turns are the game's counter (0 at the start); a game that ended earlier
    counts to its end.
  - Input: embeddings of the two sequences plus a linear map of the scalars, added to the global
    token. **z is dropped (set to "absent") on 10% of training decisions**; absent z adds exactly
    nothing, so z = 0 is the plain unconditioned network.
  - z holds the future of the game, so held-out top-1 with the game's own z is not comparable with
    R7. **All comparisons with R7 use z = 0.**
  - Known leak, accepted: the value head can read z (e.g. many cities at turn 10). Value quality
    is reported at z = 0 only.
- **Model:** `model.Net(d=512, layers=6, z=True)`: R7's architecture twice as wide, 21.9M
  parameters before z (measured). bf16, gradient accumulation and a per-process GPU memory cap so it
  fits beside S10.
- **Data:**
  - **Continents:** the plan was ~14k Might Continents Small "Instant Tournament" games. Found
    tonight:
    - the instant tournaments the site lists (10,000 of 14,221) date from 2023-12 to 2025-08, so most
      will return 1005 (expired);
    - the 1,019 regular Might/Continents/Small tournaments already known (2026 dailies among them)
      are the realistic source;
    - **the game client's saved token was refused (HTTP 401)**, so nothing can be downloaded until
      Ben logs in to the game again.
  - **Tonight, instead:** stage 1 pretrains on the AI-teacher games (`m1v2`, 19,956 games, 24.6M
    decisions), as R3 did for R4/R7. Stage 2 is the 7,732 championship games (`human_full`, same
    held-out split as R4/R7).
- **Arms (separable; one GPU job at a time):**

  | Arm | Run | Stage 1 | Stage 2 | When |
  |---|---|---|---|---|
  | A | `hp1a` | m1v2 AI games with z, 25,000 steps × 256 (6.4M samples, ~1/4 pass) | human_full with z, 24,000 steps × 256 (~2.2 passes), held-out every 1,000 steps, best checkpoint kept | tonight |
  | B | `hp1b` | as A, then the Continents games with z (one pass) | as A | after Ben's login and the download |
  | C | `hp1c` | A's stage 1 | as A, advantage-weighted (AWR, weights from a value learned on human outcomes) | only if A passes T1 or T3; not built yet |

  - B against A isolates the Continents pretraining. C against A isolates the weighting.
  - **No auxiliary heads in A or B** (they would confound the size and z results); a later arm if
    wanted.
  - **Train-once rule:** each arm is one fresh network trained once on the championship set
    (stage 2, early-stopped on held-out loss). B is a second fresh network on the same set, which
    makes it an ablation: Ben's call, asked in the handoff.
- **Tests (z = 0 unless stated):**
  - **T1, imitation:** `human_bench.py` on the 150 held-out human games. R7: top-1 43.7%.
    - Expected: ≥ 45.0%.
    - Falsifier: < 43.7%.
    - Also logged: top-1 with the game's own z (an upper bound, not compared).
  - **T2, controllability** (vs Crazy, 100 games per z set, each game's z drawn from held-out human
    games of the agent's own tribe; facts measured from the agent's own commands):
    - **Tech tempo (B5's test):** z sets "fast" (techs by turn 20 ≥ 9) and "slow" (≤ 5).
      - Expected: fast gives ≥ 8.5 techs by turn 20 (over games reaching turn 20); slow is at least
        3 lower.
    - **Catapults:** "none" (0 catapults by turn 20, game ≥ 20 turns) gives ≤ 5% catapults among
      units trained by turn 20; "many" (≥ 3) gives at least twice z = 0's share.
    - **Navy:** "no navy" (no port and no naval unit in a game of ≥ 20 turns) against "early port"
      (port by turn 8): the share of games with a naval unit by turn 20 differs by ≥ 30 points.
    - **Kill (B5):** if the agent moves less than half of the gap between the two z sets' own
      facts on tech tempo and catapults, z is dropped from the plan (no z track in RL).
  - **T3, head-to-head vs R7** (`selfplay.py --match`, 200 games, seats swapped):
    - Expected: > 55%.
    - Kill (B6): ≤ 52% → the size change doesn't pay, and arm C is not run.
  - **T4, vs Crazy** (200 games, the standard seeds): expected ≥ R7's 191/200.
  - Also at the end: vs `s10c` gen 95 for orientation, not as a test.
- **Cost:** A ~6–7 GPU-hours beside S10 (stage-1 rate guessed at ~500 samples/s from R3's
  2,500/s alone at 5.9M); T2–T4 ~2 h of games. B adds the Continents pass. Engineering ~1 evening.
- **Files:** `py/zfacts.py` (new); `py/model.py` (`z`, optional and off by default: old checkpoints
  load and give identical outputs, checked); `py/train_bc.py` (`--z`, `--z_drop`, `--accum`,
  `--mem_cap_gb`, `--heads`, `--grad_ckpt`; defaults unchanged); `py/eval_vs_bot.py` (`--z_from`,
  `--z_filter`, facts per game); `py/cm_replays.py` (`--format`, `--tag`), `tools/cm_list_tournaments.py`
  (`--out`), `py/convert_replays.py` (`--preset`, for Continents). PC copy in `~/poly/hp` (S10's
  `~/poly/py_sp` untouched). Status: `runs/agents/humanpass_status.md`.
- **Amendment before any test ran (2026-11-02, 21:50):** T2 draws each game's z from all human games
  of the agent's tribe that pass the set's filter (training and held-out), not only the held-out
  ones: the held-out pool had only 14 "slow" players, in 6 tribes (measured). On new maps the z is
  only an input vector, so the split it comes from doesn't matter for this test.
- **Change to arm A's stage 1 (2026-11-02, 22:04), before any test:** once S10's games shared the
  GPU, stage 1 fell from ~445 to ~330 samples/s (measured), which put arm A's end near 09:00. Stage 1
  is cut from 25,000 to 12,000 steps (3.1M samples, about 1/8 of a pass over m1v2), resumed from its
  step-2,000 checkpoint, so the cosine schedule follows the 12k length from there. Stage 2 is unchanged.
  - First check of z (stage 1, step 2,000, 10 held-out AI games): policy loss 1.7585 with the game's z
    against 1.7622 without (measured): z already carries a little information.
- **Running (2026-11-02, 22:10):** arm A on the PC, `~/runs/hp1a` (`tools/hp1_chain.sh`, code in
  `~/poly/hp`): stage 1 at ~375 samples/s beside S10 (measured), ETA ~00:15; stage 2 ETA ~05:30
  (derived from that rate). `tools/hp_after.sh` runs T1 at stage-2 steps 8,000 and 16,000, then T1, T3
  and T2 + T4 on `best.pt` when the chain ends (`~/runs/hp1a/tests/`). Arm B waits for Ben's game
  login (`tools/hp_continents.sh`, then `tools/hp1b_chain.sh`).
  - Stage 1, step 4,000 (measured): held-out AI-game policy loss 1.4348 with z, 1.4367 without (top-1
    52.6% either way); value log loss 1.37 with z against 0.92 without. The value head is already
    reading outcomes through z (the leak noted above), so search and evaluations should use the value
    at z = 0.
  - **Lead's note (22:45):** stage 1's training value accuracy is 0.98–0.99 at steps 4,800–5,000
    (measured): z holds later facts of the same game (cities at turn 10, units by turn 20), so the
    value head reads the result from it. The policy tests (T1–T4) don't use the value. Any later
    z run should train the value head only on z-dropped samples, or keep z out of the value path.
