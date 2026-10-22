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
