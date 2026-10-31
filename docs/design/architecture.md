# Architecture (v5)

v5 rebuilds the design after four adversarial reviews (see [reviews/2026-10-20.md](reviews/2026-10-20.md)).

## Principles

1. **Build in layers. Each layer is added only when a metric asks for it**, with one change per
   measured run. Polyfish died stacking unattributable changes on a broken instrument.
2. **Hand-written code states facts, never judgements.** Allowed:
   - legality (the game's own legal-command list);
   - infrastructure;
   - a canonical position hash;
   - **derived facts computed with the game's own code**, such as "which tiles can this enemy unit
     reach in 2 turns".

   Not allowed: any notion of a good move or position. **Canonical move ordering is dropped**: the
   rules review found too many hidden dependencies for it to be provably safe. Duplicate
   positions are merged by the canonical hash instead.
3. **Search is cheap in training and heavy at play time** (the Ataraxos pattern).

## Layer 0 — the polyhost contract (other agent)

- **Batched API:** reset(seed, tribes, seats) / legal / step / observe across many games per
  process.
- **Legal commands = the game's own list**, every `CommandType` (see scope-and-rules.md). Each
  command is encoded as (type, source tile, target tile, argument: unit type / improvement / tech /
  reward / player).
- **Clone:**
  - **v1:** Serialize/Deserialize with the command history stripped. First prove, with a replay
    diff, that the rules never read the history.
  - **v2:** a native object-graph copier that shares immutable game data. Target ≤ 0.2 ms, gated on
    `serialize(copy(x)) == serialize(x)`.
- **Canonical hash:** ignores history and normalizes unit IDs. Includes everything else that
  matters: unit direction, previous turn-end coordinates, created turn, XP, effects, random-number
  state.
- **Observation builder (allowlist only):**
  - own full state;
  - explored mask plus the **live contents of explored tiles, minus invisible enemy units**;
  - public facts (to measure: enemy score, techs).
- **Determinized-state builder:** observation + belief sample → a valid GameState with
  **reseeded random-number state** and **empty history**. A taint check enforces both. Use
  `tryCall`: a sample that throws is discarded, never allowed to crash the process.
- **Seeding and setup:** a seed per game; both seats as human type with handicap 1; bots with a
  chosen difficulty and handicap.

### Layer 0 — status (2026-10-20)

Built in `polyhost/env.hpp` (C++), `polyhost/wire.hpp` (protocol) and `py/polyenv.py` (client).

| Contract item | Status |
|---|---|
| reset / legal / step / observe, batched over slots | Done. One process hosts many matches; requests take lists of slots |
| Legal list = the game's own generators | Done. Strict parts equal a brute-force `IsValid` sweep; every AI command is covered except its `StayCommand` (bot-only) |
| Seeding and setup | Done. Pass-and-play construction, byte-identical to the real app's Might/Lakes/Small game. Seats are outside-driven (handicap 1) or the game's AI at a difficulty (`AddAIOpponent`) |
| Clone v1 (serialize) | Done, with the history kept. Save 0.14 ms, load ~0.4 ms. Idempotent after one round trip, and stepping a restored copy is byte-exact |
| Observation builder (allowlist) | v1 done: 32 per-tile features, 20 globals, own techs. **The perturbation fog-leak test is not written yet** |
| Determinized-state builder, history stripping | **v1 done (2026-10-30)**: `polyhost/determinize.hpp`, wire op 14 (`polyenv.Env.determinize`), tested by `polyhost detertest` (research log). Known leak: enemy cities on unexplored tiles are kept in place |
| Canonical hash | Not started (needed from M3) |
| Microbenchmarks | Done for the parts; dummy-network end to end is still to do |

**Measured costs per decision, one emulator thread** (replacing the estimates below):

| Part | Cost |
|---|---|
| legal list | 1.2–1.6 ms (136–152 commands) |
| observation | 0.01 ms |
| execute | 0.09 ms |
| built-in AI | 1.2–1.4 ms |
| save / load | 0.14 / ~0.4 ms |

With these numbers:
- the policy-only simulator cost is ~1.7 ms per decision;
- Crazy vs Crazy runs at 2.4 games/s on 6 emulator processes (about 1,000 commands per game).

## Layer 1 — the network

**Inputs.**
- **196 tile tokens:**
  - terrain, resource, building and level, road or bridge, territory owner;
  - city: level, population, capital, wall, besieged, connected;
  - village, ruin, starfish, lighthouse;
  - unit: type, owner, HP, XP or promotable, moved or attacked, hull, own-invisible;
  - explored flag;
  - **derived reachability:** can an enemy unit reach this tile in 1 / 2 turns.
- **Global tokens:**
  - own stars, income, techs;
  - both tribes, turn, scores, seat;
  - **time left from the live-clock formula**;
  - opponent handicap;
  - in-turn context (commands so far, stars spent).

**Positions and symmetry.** Use positional encodings that respect the board's 8 rotations and
reflections, and augment training data with them **only after polyhost verifies the rules are
symmetric**. Possible asymmetries: the push direction for units at the exact map centre, and seat
order.

**Trunk.** Transformer encoder. Start around 5M parameters. Inference runs on the 4070 Super, so
grow to 10–20M once the simulator, not the GPU, is the limit. Games are scarce; FLOPs aren't.

**Heads.**
- **Policy: a pointer over the legal-command list.**
  - Each legal command is embedded from its type, source and target tile tokens, and argument, then
    scored against the context; softmax over the list.
  - No fixed action indices or masks to keep in sync. A coverage test proves every legal command
    gets exactly one score.
- **Action value (Q):** the same pointer, predicting V after the command. One pass pre-scores all
  candidates, so only the best get simulated.
- **Value:** win probability for the side to act.
  - Targets: **TD(λ) / search-bootstrapped**, not raw ±1 on hundreds of correlated positions
    (AlphaGo's overfitting lesson).
  - Positions are down-weighted per game, and stale positions are re-scored with the EMA network on
    the GPU.
- **Aux:** final owner of each tile and city; turns until each capital falls; final income.
  Cheap, dense, and they carry much of the learning signal.
- **Belief (Layer 3):** a joint sampler. Count, then place: invisible enemy units; contents of
  unexplored tiles; enemy stars (and techs if hidden).

**Deferred until a metric demands them:**
- the value ensemble (if needed: randomized prior functions, not heads on a shared trunk);
- the intent head (if needed: end tile + ordered targets + final verb, tracked through
  embark/upgrade, recomputed after spawns).

## Layer 2 — search

**M2: turn sampler (cheapest planner).**
- At turn start, clone once. Sample N whole turns from the policy, with Gumbel noise and
  temperature. Score each end-of-turn position with V; play the best (in training, sample by
  softmax).
- Needs only one clone per candidate turn. First measurement of **search gain**.

**M3+: per-decision Gumbel search** (replaces plan-level beam search; ML review #4).
- At each decision:
  1. take Gumbel top-n candidates (n = 8–16), pre-screened by the Q head;
  2. complete each to end of turn with a policy rollout, and score with end-of-turn V (averaged
     over K ≤ 2 determinizations in training);
  3. sequential halving; select by the Gumbel MuZero improved policy.
- The improved policy is a **valid per-step training target**. Unlike the plan-level beam, there is
  no length bias, no undefined prior after merging, and no comparing mid-turn values across depths.
- Positions are merged by the canonical hash.

**Play-time extras (never in training):**
- **K = 8–16 samples**, judging capital safety **pessimistically** (worst case or CVaR of the
  capital-fall head across samples);
- an **opponent-reply check**, with the opponent playing from *its own* observation of each
  sample, applied to all finalists so they're scored on the same scale;
- larger n;
- **replan on surprise** (a reveal, a ruin, a blocked move);
- a budget from the live-clock formula;
- **sample from the improved policy**, not argmax: a mixed strategy is harder to exploit (Ataraxos).

**Regularization.** KL toward a slow magnet (an EMA policy plus a small uniform mixture),
annealed, as in Ataraxos / magnetic mirror descent. This is damping, not just a trust region.

**Criticality** only decides play-time budget. **Openings (turns 0–6) are always critical.**

## Cost model (v5)

The systems review rebuilt this from first principles. The earlier "0.3 ms per node, 30–80k
games/day" was off by an order of magnitude.

| Cost per search node | Now (estimate) | Target |
|---|---|---|
| Clone (deserialize, history included) | 0.5–1.5 ms | ≤ 0.2 ms (native copier) |
| Legal-command generation | 10–50 ms (serve.hpp style) → 1–3 ms engineered | ≤ 1 ms |
| Encode + GC overhead | 0.1–0.3 ms + 30–60% | — |
| **Total per expansion** | **~3 ms** | **≤ 1 ms** |

**Regimes**, about 600 decisions per game. The Mac's effective actor capacity is ~4.5 cores (8
processes scaled 5.2×).

| Regime | Simulator steps per game vs policy-only | Rough Mac games/day at 3 ms → 1 ms |
|---|---|---|
| Policy only (BC data, PPO) | 1× | ~100k → ~300k |
| M2 turn sampler, N = 8, on 25% of turns | ~3× | ~35k → ~100k |
| M3 per-decision Gumbel, n = 8, rollouts, 25% of turns | ~15× | ~7k → ~20k |
| v3 design (beam + K + union replay + reply check) | 100×+ | ~1–5k (the review's estimate) |

**These are estimates. Stage 0 replaces them with measurements:** end-to-end decisions per second
with a dummy network, on 14×14 Lakes. The table is also why the PPO-vs-expert-iteration A/B in
training-and-data.md uses **equal simulator time**: search costs 15× the steps per game.

## Must-pass tests (CI, before any training run counts)

- **Command vocabulary:** every legal command in 10k random mid-game states maps to exactly one
  policy input.
- **Fog leak:**
  - Determinized states are built from the allowlist only, with a taint check (seed replaced,
    history empty).
  - A perturbation test: vary the seed and `RandomHash`, `CurrentUnitId`, enemy history, enemy
    stars and techs, invisible-unit positions, and hidden units next to our paths.
  - The network input and the planner's choice (fixed seed) must not change.
- **Determinism:**
  - step∘clone == clone∘step;
  - serialize round-trips are idempotent;
  - a fresh process matches a long-lived one (catches static caches such as the AI's
    `AnalyzeSituationIfNeeded`).
- **Built-state validity:** serialize round-trip plus 1,000 random playouts with zero exceptions.
- **Canonical hash:** transpositions merge, and different positions never collide (randomized
  pairs).
- **Training-vs-inference parity:** the same input gives the same outputs in the learner and the
  inference server (Polyfish's batched-inference corruption).
- **Loud evaluator:** a failed or empty evaluation is a hard error, never a silent zero (Polyfish's
  strength gauge recorded nothing for weeks).
- **Injected-bug canary:** shuffled value labels must show up as a strength drop within one
  evaluation.
- **1-hour smoke test:** BC on 2k bot games must beat Easy ≥ 80%.
