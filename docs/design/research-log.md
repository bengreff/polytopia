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
