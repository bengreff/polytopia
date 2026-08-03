# Research findings: prior art on Polytopia AI

Compiled 2026-08-02. Every claim below was verified against a primary source (paper PDF, GitHub API,
or repository file) unless explicitly marked as unverified. This document exists so a fresh session
does not have to redo the search.

---

## 1. The only peer-reviewed work

**Tribes: A New Turn-Based Strategy Game for AI Research**
Diego Perez-Liébana, Yu-Jhen Hsu, Stavros Emmanouilidis, Bobby Dewan Akram Khaleque, Raluca D. Gaina.
Queen Mary University of London. AIIDE vol. 16 (2020), pp. 252–258.

- Paper PDF: https://rdgain.github.io/assets/pdf/papers/perez2020tribes.pdf
- Repo: https://github.com/GAIGResearch/Tribes

This is the *only* peer-reviewed paper on AI for this game. It contributes the framework and
baselines; it contains **no neural network** (a web search summary claiming it proposes a CNN trained
on Puppet-Search labels is wrong — it conflated a different RTS paper; the PDF was read directly to
confirm). Its future-work section explicitly names as *not done*: model-free/deep RL agents, Online
Evolutionary Planning, and Evolutionary MCTS.

### Repo state (GitHub API, 2026-08-02)

| Field | Value |
|---|---|
| Last push | **2022-08-25** (dead) |
| Stars / forks | 98 / 22 |
| Open issues | 14 |
| License | **none specified** |
| Language | Java (93.5%), Java 8+ |

### Experimental setup in the paper

11×11 procedurally generated maps, 1:1 water/land ratio, 25 levels, 20 repetitions each = 500 games
per pairing. Tribes Xin-Xi and Imperius. Capitals mode, **full observability**, 50-turn cap. Budget:
**2000 forward-model `next()` calls per decision** (function-call budget, not wall-clock, for
hardware-independent comparison).

### Agent results (Table 1)

| Agent | Overall win rate |
|---|---|
| RHEA (rolling horizon evolution) | 74.84% |
| **Rule-based heuristic (no search)** | **65.80%** |
| MCTS | 60.44% |
| Monte Carlo | 50.32% |
| One-step lookahead | 47.76% |
| Random | 0.84% |

**RHEA vs. a "moderately good" human: 25 games, human won 100%.** This is the high-water mark of
published Polytopia AI.

### Complexity measurements (400 games, RHEA vs RB, 50 turns)

`h0` = turns 1–25, `hF` = turns 26–50.

| Metric | Overall | h0 | hF |
|---|---|---|---|
| Actions available per move | 54.47 | 21.86 | 85.83 |
| Moves per turn | 9.86 | 6.47 | 13.11 |
| Branching factor per turn | ~10^15 | 10^7 | 10^23 |

Winning bots reach ~10^32 branching in the final 5 turns. Game tree size ≈ **10^1500**.

Comparison (from the paper): Chess branching 30 / tree 10^123; Go 300 / 10^360; Hero Academy 10^8 /
10^711; Bot Bowl 10^51 / 10^3264.

> **Critical reframe:** these are *per-turn* figures. A turn is ~10 sequential sub-actions. Measured
> per-decision — the granularity at which Chess and Go are quoted — the action space is ~54 average,
> and tribes-rl's masked encoding caps it at 197. That is smaller than Go.

### The state-of-the-art evaluation function

Seven features Φ: deltas in production, technologies researched, game score, cities owned, units
owned, units defeated, and sum of owned city levels. Combined linearly with hand-set weights
**W = {5, 4, 0.1, 4, 2, 3, 2}**. Per-tribe progress values are averaged across opponents and
subtracted from the root player's.

That is the entire SOTA position evaluator for this game. It is a very low bar.

### Useful implementation notes from the paper

- **Root prioritization** (fixing action type at the start of each iteration) improved play — a
  simplified Progressive Widening.
- **RHEA population size 1 beat sizes 5 and 20.** Crossover generated invalid actions. RHEA
  effectively degenerates to a Random Mutation Hill Climber.
- **Skipping the MCTS rollout** and evaluating at the expansion node worked better than rollouts.
- Disband and Destroy actions were removed from the SFP action pool.
- Forcing an EndTurn every X steps was tried and discarded.
- Fog of war: vision 1 tile on plains, 2 on mountains. Unseen tiles are `Fog` terrain and are treated
  as plain empty tiles by the forward model. Visible enemy cities/units hide kill counts, original
  city, population, production, and units created.
- **~150 parameters** determine units/buildings/economy (`core/TribesConfig.java`); terrain
  probabilities in JSON. Framework runs with partial *or* full observability.
- Level generator is a Java port of https://github.com/QuasiStellar/Polytopia-Map-Generator

### Where the lab went

QMUL's group moved on to **Stratega** (general strategy games framework) and then **TAG** (tabletop
games framework) — generality over depth. Nobody owns this problem.

---

## 2. tribes-rl — the best engineering in the space

https://github.com/pham-tuan-binh/tribes-rl · demo: https://tribes.binhph.am/

| Field | Value |
|---|---|
| Created | **2026-07-07** |
| Last push | 2026-07-08 |
| Stars / forks | 11 / 2 |
| Language | C |
| License | **NOASSERTION** — legally unusable as-is |
| Size | ~147 MB |

**Architecture (this is what to copy):**

- Single **C11 header-only** engine compiling to two targets: a headless **PufferLib** environment for
  training, and **WebAssembly** for browser play. README: *"The game you play on the website is
  bit-identical to the environment the agent trained on."*
- **Action space:** each turn becomes a sequence of micro-decisions via a `SELECT`, `VERB`, `TARGET`
  phase machine with **one masked `Discrete(197)` head**. README: *"the action mask contains exactly
  the legal choices for the current phase, so the policy never has to learn what is legal."*
- **Throughput:** ~**1.8M agent-steps/sec** with 4096 parallel envs and an **8.2M-param MinGRU policy
  (768 hidden, 4 layers)**; 10-billion-step runs in **under two hours**.
- **Verification:** golden-trace harness against the Java Tribes implementation (`tools/`), with
  porting specs in `docs/`.
- **Reward:** win by domination `+(1 + speed_bonus * (1 - turns/200))`, plus configurable dense
  shaping for cities, capitals, kills, exploration.
- **Scope:** Might mode only, 11×11 generated maps, 2–4 players, fog on, 200-turn cap, regular tribes.
- Repo layout: `core/` (C11 + tests), `puffer/`, `web/` (WASM bridge, renderer, in-browser
  inference), `tools/` (golden-trace), `docs/`, `image-gen/` (sprite pipeline).

**Critical gap: zero evaluation.** No win rate, no Elo, no baseline comparison, no human games. The
README notes spectated games use temperature 0.5 "so spectated games look clean" — tuned for
appearance, not measured strength.

---

## 3. PolyEnv — the only clean license

https://github.com/FryOne2137/PolyEnv · docs: https://polyenv.readthedocs.io/

| Field | Value |
|---|---|
| Created | **2026-01-11** |
| Last push | 2026-07-20 |
| Stars / forks | 1 / 0 |
| Language | C++20 |
| License | **MIT** |
| Size | ~119 MB |

- C++20 simulation core with Python bindings. Requires Python 3.10+, C++20, CMake 3.20+.
- **12 regular tribes.** Explicitly excludes Aquarion, Elyrion, Polaris, Cymanti.
- `VectorGameEnv` — 256 environments batched, configurable threading.
- `MctsPool` — batched neural MCTS, trees held in C++, virtual loss, node budgets; only dense NumPy
  arrays cross the Python boundary.
- `SelfPlayPool` — **belief-particle MCTS** for fog of war.
- Replay save/load via `.polygame`. GUI support mentioned.
- Sync interface: `model_request_numpy()` for legal actions, then `step_fast()`.
- Map generation also derives from QuasiStellar's Polytopia-Map-Generator.
- No special tribes, **no trained bot, no reward shaping, no published throughput, no baselines.**

**Caution:** 1 star, single author, ~7 months old, commit messages like "error hangling update".
Its MIT license is its strongest feature; code quality is unverified. **Audit before depending on it.**

---

## 4. Polyfish — the only project that measured itself, and the results are instructive

https://github.com/HenBOMB/Polyfish

| Field | Value |
|---|---|
| Created | 2023-07-27 |
| Last push | **2026-08-02** (actively developed daily) |
| Stars / forks | 8 / 3 |
| Language | Rust |
| License | **none** |

Stack: Rust engine + AI, TypeScript rules replica (ported to Rust), C++ memory injection, Python
training, web UI. AlphaZero-style MCTS + Candle neural network, modular economy/military/expansion
evaluators, opening book. Has a second contributor and a real training-experiment pipeline
(`training_metrics_schema.sql`, `auto_train.sh`, Elo evaluation servers, named experiments).

**The "Game RIPPER"** performs memory injection to pull real game states directly from the Steam
version. This is the only demonstrated path to real-game ground truth.

### Measured results (from `hypothesis_driven_improvements.md`)

- The net **loses to its own greedy heuristic teacher roughly 2:1** — **−110 to −190 Elo** vs frozen
  Greedy baseline.
- Win rate **25–40%** vs Greedy; best observed 40.6%.
- **Opening is excellent:** first neutral village at **5.24 turns** vs the teacher's 6.2, capture
  rate 97–100%.
- **Mid-game collapses:** reaches a 3rd city in **39% of games vs Greedy's 81%**. By turn 16, Greedy's
  army value is **30 vs the model's 13**. Diagnosis: *"grabs one village and stops."*
- Open hypotheses concern teacher-anchor frequency (holding anchor games at 25% rather than letting
  them decay) and trunk capacity saturation.
- The doc references a large budgeted cloud training run; **this figure could not be verified and
  should not be planned around.**

### The bugs (from `expert_review.md`) — read this before writing any search code

1. **Value backprop flips sign at every tree edge by depth parity, but Polytopia allows consecutive
   same-player moves.** Result: nodes "frequently prefer children whose evaluations were bad for it."
2. **Terminal states and horizon cutoffs both return 0.0**, so "capturing the enemy capital look[s]
   identical to a draw inside the search."
3. **Value targets compressed to ~±0.05** by aggressive tanh scaling (raw scores are ~1850–2350 with
   differentials of 100–400). Q-values become negligible and "selection reduces to prior-following."

Net effect: MCTS ran "almost entirely on policy priors, with the value network contributing near-zero
(and sometimes inverted) signal."

The review's headline finding: *"the single biggest omission: you have no strength measurement."*
And: fixing correctness before throughput matters, because "speeding up game generation 20× before
fixing these would just generate corrupted training data 20× faster."

---

## 5. Other projects

- **PolyMod** — https://github.com/PolyModdingTeam/PolyMod · 85★, C#/BepInEx, Windows x86, updated
  2026-07-22. Mod loader for the *real* game; site polymod.dev. Capability details not confirmed —
  check their wiki/Discord. Relevant as a real-game validation and rules-verification channel.
- **polytopia_rl** — https://github.com/ClaireBookworm/polytopia_rl · HackMIT project, Py4J bridge
  from Java Tribes to a Gym-style Python API. Infrastructure only; no algorithms, no results.
- **Polydystopia** (DystopiaPolyScript, DystopiaBepInEx) — C#, private-server/scripting direction.
- Toy clones: `pihedron/polytopia` (3D JS), `Slimwolff/topiaClone`.
- Adjacent environments if this substrate fails: MicroRTS, Freeciv-gym, Unciv, Bot Bowl, Stratega.

---

## 6. The in-game AI is not AI

Difficulty levels grant bots **1 / 2 / 3 / 5 stars per turn** (Easy / Normal / Hard / Crazy) plus
increased aggression. It is an income handicap, not better decision-making. Experienced players
report beating 14 Crazy bots simultaneously and finding them "just too easy."

Consequence: **"beats the in-game AI" is a near-worthless milestone.** It is also evidence that the
developers never invested in game AI, which is part of why the problem is open.

---

## 7. The competitive scene (evaluation targets)

- **PolyElo** — in-game Elo matchmaking, added in update 2.0.58.
- **PolyChampions** — competitive league with its own Elo: https://polychampions.com/elo/
- **Challengermode** — ladders and tournaments:
  https://www.challengermode.com/polytopia/ranking
- **Esports World Championship** — ~$4,000 first prize, **Might mode, best-of-three,
  single-elimination**. Confirms 1v1 Might as the competitive format, which is why project scope
  matches it.

This is unusually good for a game-AI project: a rateable population with named top players and an
existing rating infrastructure to plug into.

---

## 8. On LLMs playing the game directly

Not viable as the player. Order-of-magnitude arithmetic: ~500 decisions per player per game means an
LLM-as-policy runs ~15–20 min/game with no search; inside MCTS at 800 sims/decision it is ~400k
evaluations per game, i.e. days. Self-play training is ~5–6 orders of magnitude out of reach.

Evidence on the capability itself: **GRASP** (https://arxiv.org/abs/2407.01892) ran frontier models
on 16,000 grid environments against random-walk and greedy-search baselines and found they "struggle
to consistently achieve satisfactory solutions." Spatial benchmarks consistently report **42% to
over 80% performance loss** as compositional/geometric complexity increases.

Decisive precedent: **Cicero** (Meta, Diplomacy, human-level play) "employed a separate strategic
planner, using an LLM only for translating predictions to dialogue." Meta had every resource to let
the language model do the strategy and chose not to.

**Where an LLM is genuinely useful here:** offline, outside the inner loop. Polytopia's missing human
corpus does not exist as game records but *does* exist as prose — wikis, tier lists, opening theory,
forum meta discussion. Concrete uses:

1. **Evaluation-function programmer** (FunSearch/Eureka pattern) — propose and iterate candidate eval
   functions in code, scored by real win rate against ladder anchors. Directly attacks the documented
   bottleneck (7 hand-weighted linear features), and is cheap.
2. Opening-book generation, verified by search.
3. A human-like prior for piKL-style anchoring during self-play.

An LLM must never sit in front of the legality mask — only behind it.

---

## 9. Summary of the gap

| Claim | Status |
|---|---|
| Anyone has published a paper on a *learned* Polytopia agent | **No** — only the 2020 search-based framework paper |
| Anyone has published an evaluation vs the in-game AI | **No** |
| Anyone has published an evaluation vs humans since 2020 | **No** |
| Anyone has beaten a strong human | **No** |
| Anyone has beaten a *moderate* human | **No** (RHEA: 0–25) |
| The one project that measured itself beats its own heuristic | **No** (−110 to −190 Elo) |

Absence of published evaluation is not proof of absence — someone may hold a strong private agent.
But three active projects with zero published win rates is real evidence that nobody has crossed the
line and told anyone.
