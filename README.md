# Polytopia Superhuman Agent

**Goal:** an agent that decisively beats strong human players at 1v1 Might in The Battle of Polytopia.

**Status:** Phase 0 complete and playable; Phase 1 baselines and the Elo ladder exist.

> **Read `HANDOFF.md` first.** This document is the ORIGINAL PLAN, and parts of it have been
> overtaken by what the build found — most notably the 2020 agent-ordering gate, which has been
> dropped deliberately. `HANDOFF.md` has current state, next steps and the reasoning.

**Why this document exists:** it is the entry point for a fresh session. Read this first, then
`WORKFLOW.md` for how sessions run and what the current state is, then
`docs/research-findings.md` for the verified evidence base and citations. Everything asserted here
with a number behind it was verified against a primary source on 2026-08-02; see the findings doc.

| File | Contents |
|---|---|
| `HANDOFF.md` | **Start here.** Current state, next steps, known gaps, lessons |
| `README.md` | The original plan: thesis, locked decisions, architecture, phases, risks |
| `WORKFLOW.md` | How autonomous sessions run, current state, open questions |
| `docs/research-findings.md` | Verified prior art with sources |
| `docs/outreach.md` | License-request messages and contacts |

## A note on the neural network

Search-first is a *sequencing* choice, not an architectural one. The endpoint is neural-network
dominated. The Phase 2 handcrafted evaluation exists for exactly two reasons: as a measuring stick,
so it is possible to tell whether the network is helping, and as a *teacher* for Phase 3's supervised
distillation, which is what solves the cold-start problem. Both are scaffolding that gets discarded.

Engineered inputs and aggressive action pruning feeding a large network is the mainstream winning
architecture, not a compromise: Stockfish NNUE feeds hand-crafted king-relative feature planes into
its network; AlphaStar feeds structured entity lists into a transformer rather than pixels; and
tribes-rl's legality mask *is* action pruning — hardcoded domain knowledge the network never spends
capacity learning. This project follows that pattern deliberately.

The only claim the evidence argues against is a network with *no search at all* as the final answer,
and even that is genuinely open in this game. That is why track B exists rather than being dismissed.

---

## 1. The thesis: why this will work

No one has beaten a human at Polytopia. The best published result is from 2020: the strongest agent
in the only peer-reviewed paper on the game lost **25–0** to a merely *moderate* human player.

That sounds discouraging. It isn't, and the reason is the core bet of this project.

### 1.1 The branching factor is a measurement artifact

The 2020 paper reports a per-turn branching factor of ~10^15 (up to 10^32 late game) and a game tree
of ~10^1500, versus Chess at 10^123 and Go at 10^360. Those numbers scared the field off.

But they measure at the wrong granularity. A Polytopia *turn* is a variable-length sequence of ~10
interdependent sub-actions. Measured per **decision** rather than per **turn** — which is how Chess
and Go are measured — the picture inverts. tribes-rl demonstrated this in practice: decompose the
turn into micro-decisions with a `SELECT → VERB → TARGET` phase machine and legality masking, and the
per-decision action space is a masked `Discrete(197)`, usually far smaller in practice.

**~197 is smaller than Go's ~250.**

The honest characterization of this game is therefore not "astronomically harder than Go." It is:

> A ~200-branching game with a ~3× longer horizon (≈500 decisions/player/game), imperfect
> information, and stochasticity.

Each of those properties is individually solved in the published literature. Nobody has assembled
the pieces for this game.

### 1.2 The reason nobody has done it is neglect, not difficulty

Total serious effort ever applied to this problem: **one academic paper and three hobbyist repos**,
two of which are under two months old. For comparison, Chess absorbed ~50 years of work by thousands
of people; Go absorbed a DeepMind-scale program.

The 2020 authors (QMUL) published the *framework* and moved on to Stratega, then TAG — generality
over depth. Their repo has been dead since August 2022. There is no competition, no prize, no
benchmark culture, and no corporate incentive. The game's own shipped "AI difficulty" is not AI at
all: Easy/Normal/Hard/Crazy grant the bot **1/2/3/5 stars per turn**. It is an income handicap plus
an aggression knob. Even the developers never invested.

### 1.3 Two structural advantages over Go

- **The reward signal is far richer.** Go yields one bit at the end of the game. Polytopia exposes
  stars/turn, city count and levels, tech position, and score *every turn* — directly measurable,
  strongly predictive proxies for winning. Dense reward shaping is legitimate here in a way it never
  was for Go. This substantially offsets the longer horizon.
- **The human ceiling is much lower.** Top chess players stand on five centuries of theory and a
  player base in the millions. Competitive Polytopia has a few years of meta development and a player
  base in the thousands. Superhuman play may be *easier* to reach here than in Chess despite the game
  being more complex, because the target is far less optimized. This is untested and is one of the
  more interesting things this project will find out.

### 1.4 The chess lesson everyone in this space forgot

Chess engines were crushingly superhuman **before neural networks existed.** Deep Blue beat Kasparov
in 1997. Stockfish with a handcrafted evaluation and alpha-beta search was several hundred Elo above
the human world champion by ~2016. NNUE arrived in Stockfish 12 (2020) and added roughly another
100 Elo — a real gain on top of an already-dominant engine, not the source of its dominance.

Polyfish inverted this ordering: it built the network first, never built a strength measurement, and
spent a year losing to its own handwritten heuristic without knowing why.

**Search plus a good evaluation function is the backbone. The network upgrades the evaluation; it
does not replace the search.** This project follows the historical ordering deliberately.

---

## 2. Locked decisions

| Decision | Choice | Rationale |
|---|---|---|
| **Engine** | Fresh build; tribes-rl, PolyEnv, Polyfish as reference | No license risk. Designs are not copyrightable; code is. We copy architecture, not source. |
| **Rules source** | Extract the ruleset from PolyEnv (MIT) where possible | Only permissively licensed rules implementation available. 12 regular tribes. |
| **Algorithm** | Search-first as the primary track; model-free RL raced in parallel against the same ladder | Search-first mirrors how Chess actually got superhuman and is measurable at every stage. The parallel race produces a head-to-head comparison nobody has run. |
| **Fidelity** | Real-game parity from the start | The one risk that can invalidate the entire project is being superhuman at our clone rather than at Polytopia. Pay this cost up front. |
| **Human eval** | Owner plays it first; public WASM deployment once it is not embarrassing | Immediate signal now; public deployment later doubles as corpus generation for the missing-human-data problem. |
| **Scope** | 1v1 Might, 11×11, regular tribes only, fog on | Matches the competitive format. Avoids multiplayer general-sum instability entirely, which is the one genuinely unsolved barrier. |

### On scope discipline

Full fidelity to all 16 tribes and all game modes is enormous surface area and directly costs
throughput and correctness. tribes-rl and PolyEnv **independently converged** on excluding the
special tribes (Aquarion, Elyrion, Polaris, Cymanti). Two teams reaching the same scope is signal.
Target full fidelity *within the competitive subset*.

---

## 3. Prior art: what to steal, what to avoid

### Steal

1. **One header-only C/C++ core → two build targets (native + WASM).** tribes-rl's best idea. It
   satisfies "fast" and "playable UI" without maintaining two rule implementations. Their guarantee —
   the browser game is bit-identical to the training environment — is what makes self-playtesting
   trustworthy.
2. **`SELECT → VERB → TARGET` phase machine with a masked action head.** The single most important
   design decision in the problem. The engine guarantees legality; the model only ranks legal
   options. Build this before anything else.
3. **Golden-trace verification harness.** tribes-rl traces against Java Tribes. We trace against the
   **real game** (memory extraction or PolyMod), which nobody has done.
4. **Belief-particle MCTS for fog** (PolyEnv) — correct shape for imperfect information.
5. **Rule parameters as data, rule structure in code.** Java Tribes exposes ~150 tunable constants
   plus JSON terrain configs. That is the right 80/20: cheap balance modding and new map types
   without paying for a fully data-driven interpreter.
6. **A metrics database from day one.** Polyfish has a training-metrics SQL schema — but found its
   fatal bugs via code review, not via metrics. Instrument, and then actually look.

### Avoid

1. **Building the network before the strength measurement.** Polyfish's own expert review: *"you have
   no strength measurement."* This is the defining failure of the field.
2. **Assuming alternating players in search backprop.** Polyfish flipped value sign per depth parity;
   Polytopia allows **consecutive same-player moves**, so nodes "frequently prefer children whose
   evaluations were bad for it." Our turn model must make this structurally impossible to express.
3. **Returning the same value for terminal states and horizon cutoffs.** Polyfish returned 0.0 for
   both, making "capturing the enemy capital look identical to a draw inside the search."
4. **Compressing value targets into noise.** Polyfish's tanh scaling squeezed targets to ~±0.05, so
   Q-values were negligible and MCTS degenerated into pure policy-prior following.
5. **Shipping with no evaluation** (tribes-rl: excellent engine, completely unknown agent strength).
6. **Skipping license hygiene.** MIT or Apache-2.0 header in the first commit.

---

## 4. Architecture

```
core/            C/C++ header-only engine. Single source of truth for rules.
                 Compiles to: native static lib (training) + WASM (browser UI).
                 Rule parameters loaded from data/rules/*.json.
                 Action space: SELECT -> VERB -> TARGET phase machine, masked.
                 Terminal and horizon states are DISTINCT types, not both 0.0.
                 Turn model makes consecutive same-player moves explicit.

trace/           Golden-trace harness. Replays known positions against the real
                 game (memory extraction / PolyMod) and against PolyEnv.
                 CI gate: any rules divergence fails the build.

ladder/          Elo ladder. Frozen anchors, match scheduler, rating computation.
                 THIS IS BUILT FIRST. Nothing else is trusted without it.

search/          Turn-level search. Handcrafted eval -> learned eval (NNUE-style).
                 Adaptive simulation budget: spend sims where policy is uncertain.

train/           Python. Two tracks against one ladder:
                   track-a/  search-first: eval distillation, then policy head
                   track-b/  model-free: recurrent policy, vectorized rollouts

web/             WASM bridge, renderer, in-browser inference. Bit-identical to core.

data/rules/      Rule parameters as JSON. Modding and map-type surface.
docs/            research-findings.md and design notes.
```

### Non-negotiable invariants

- The rules exist in exactly one place. If the browser and the trainer can disagree, the project is
  already broken.
- Legality is the engine's job, never the model's.
- Nothing claims a strength improvement without a ladder number attached.

---

## 5. Phase plan

Each phase has an explicit gate. Do not proceed past a failed gate — rescope instead.

### Phase 0 — A playable, verified, tweakable game (weeks 1–6)

The deliverable is a game you can sit down and play, on both machines, whose rules are verified
against the real thing and whose parameters you can change.

- C++20 core; ruleset extracted from PolyEnv (MIT). Phase-machine action space from day one.
- **Rule parameters as data** (`data/rules/*.json`) — the ~150-constant pattern from Java Tribes,
  extended. Change a value, reload, play the change.
- **Map versatility:** sizes, land/water ratios, biome mix, tribe assignment, seeds, and fixed
  starting positions for reproducible testing.
- **Game modes:** Might (domination) and Score/Perfection.
- **Two build targets from one core:** native (macOS ARM + Windows x64) and WASM.
- **Web UI served from the WASM build** — one artifact plays on the Mac and the PC, no second
  renderer to keep in sync.
- Golden-trace harness against the real client.

**Gate:** play a full game start to finish on both machines; change a rule parameter in JSON and see
it take effect; trace harness passes on a held-out position set.

> **Why the UI comes first.** A playable build is itself a verification instrument. You catch rules
> bugs by *playing* that no trace diff will surface, because you know what the game is supposed to
> feel like. This ordering is not a concession to motivation — it buys correctness.

### Phase 1 — Benchmark agents and the Elo ladder (weeks 6–10)

- Reproduce the 2020 agents in-engine: Random, Rule-Based, OSLA, Monte Carlo, MCTS, RHEA.
- **Elo ladder with frozen anchors**, in this chain:
  `Random → RuleBased → MCTS → RHEA → in-game AI (each difficulty) → human`
- Metrics DB and experiment tracking.

**Gate:** rank any two agents to ±20 Elo in under an hour of compute, **and the reproduced agents
recover the published ordering** (RHEA > RuleBased > MCTS > MC > OSLA > Random, per the 2020 table).

> That second condition is the strongest whole-engine correctness check available. If six independent
> agents rank differently in our engine than in the paper's, something in the rules is wrong — and we
> find out before any of it matters.

### Phase 2 — Search + handcrafted eval (months 3–5). **Run with fog OFF.**

Full observability is a derisking lever nobody used, and the Tribes framework supports it natively.
Solve the perfect-information game first, then add fog back in Phase 5.

- **Search whole turns, not individual actions.** Consecutive same-player moves break standard
  negamax intuitions; this is the design issue underneath Polyfish's sign bug.
- Apply **Online Evolutionary Planning** (Justesen et al.) and **Evolutionary MCTS** (Baier &
  Cowling). Both were explicitly named as future work in the 2020 paper and have *still never been
  applied to this game*. Free contribution.
- Handcrafted eval to beat: the current state of the art is **seven linear features with hand-set
  weights `{5, 4, 0.1, 4, 2, 3, 2}`**. Target projected stars/turn, city count and levels, tech
  position, army value, territory, capital safety, tempo.

**Gate:** +200 Elo over all Tribes baselines, and at least one win against a competent human.

### Phase 3 — Learned evaluation (months 5–7). The NNUE moment.

Keep the search. Replace the eval with a network trained on Phase 2 positions **labeled by deep
search results** — supervised distillation of a teacher we already trust. This sidesteps the
cold-start problem entirely and is far more sample-efficient than bootstrapping value from win/loss.

**Gate:** learned eval at shallow search beats handcrafted eval at deep search.

### Phase 4 — Policy network and self-play (months 7–11)

Close the AlphaZero loop. Use **league play with frozen anchors**, not naive self-play, to guard
against non-transitive cycling.

Track B (model-free) runs here in parallel against the same ladder. The head-to-head is a result in
its own right.

**Gate:** continuous Elo gain without plateau; beats a good club-level human.

### Phase 5 — Fog and stochasticity (months 11–14)

Determinization plus belief particles (PolyEnv implements this shape), or a ReBeL/DeepNash-style
approach. Zero-sum 1v1 is what makes those guarantees valid — another reason the scope is narrow.

**Gate:** Elo loss from re-enabling fog is recovered within one training cycle.

### Phase 6 — Human calibration (ongoing from Phase 4)

Public WASM deployment. Every game logged. This generates the human corpus the game has never had —
the single asset that most distinguishes Polytopia from every solved game. Maintain continuous
golden-tracing against the live game as it updates.

---

## 6. Compute reality

At tribes-rl's demonstrated 1.8M agent-steps/sec, **the environment is not the bottleneck.** A game
is ~1000 steps, so ~10B steps is on the order of 10M games in a couple of hours on high-end
consumer hardware.

The bottleneck moves to **NN inference inside MCTS**. Rough planning arithmetic: 800 simulations ×
~500 decisions × 2 players ≈ 800k network evaluations per game. A small net batched on a high-end
consumer GPU puts this in the range of seconds per game, so millions of games is weeks, not hours.

Highest-leverage optimization: **adaptive search budget.** Most Polytopia decisions are trivial
(obvious captures, obvious builds). Spend simulations only where the policy is uncertain. Plausibly
a 10× multiplier, cheap to implement.

*These are order-of-magnitude planning figures to validate in Phase 0, not measurements.*

---

## 7. Risks

| Risk | Severity | Mitigation |
|---|---|---|
| **Rules divergence from the live game** | Existential | Continuous golden-tracing in CI. Chosen as a Phase 0 requirement precisely for this reason. |
| Evaluation bottleneck (humans are slow and finite) | High | Bootstrap Elo through the anchor chain; spend human games only at milestone gates. |
| Turn-boundary search bugs | High | Distinct terminal/horizon types; turn-level search; regression tests on known tactical positions. |
| Map overfitting | Medium | Aggressive procedural randomization; hold out map seeds for evaluation. |
| Self-play non-transitivity | Medium | League play with frozen anchors, never naive self-play. |
| Live game updates break parity | Medium | Trace harness runs against current client; version-pin and re-verify on each game update. |

## 8. Kill criteria

Rescope or abandon if:

- Phase 2 search with a decent eval cannot clear the 2020 baselines by 200 Elo → the engine or the
  turn-search design is wrong.
- Phase 3's learned eval fails to beat the handcrafted one → the position representation is
  inadequate.
- Golden-tracing reveals rules drift that cannot be closed → we are building a different game, and
  no claim about real players is available.

## 9. Publishable contributions along the way

Not the primary goal, but each phase drops one, and they are cheap given the work is being done:

1. **Phases 0–1** — a fast, verified, permissively licensed benchmark with reproducible
   baselines and a human-evaluation protocol. Targets NeurIPS Datasets & Benchmarks. The existing
   field has zero published evaluations, so the bar is utility and rigor rather than SOTA.
2. **Phase 2** — OEP and EvoMCTS applied to the benchmark that requested them six years ago.
   AIIDE or IEEE CoG.
3. **Phase 4** — search-first vs model-free head-to-head on identical infrastructure. Nobody has run
   this comparison in any large multi-action strategy game.
4. **Phase 6** — the first human gameplay corpus for Polytopia.

Check current CFPs directly; deadlines move.

---

## 10. Immediate next actions

1. `git init`, MIT or Apache-2.0 license file, first commit.
2. Read `docs/research-findings.md` in full — it holds the verified numbers and source links.
3. Audit PolyEnv's rules implementation for correctness and extractability. Its MIT license is its
   strongest feature; its code quality is unverified.
4. Stand up `trace/` against the real game before writing any agent code.
5. Build `ladder/` with the Random and Tribes RuleBased anchors. Get a number on the board.
6. Only then write `search/`.

### Relicensing: still worth doing, even though we build fresh

Building our own engine removes relicensing as a *blocker*, but not as a *material accelerant*. The
architectural ideas (phase machine, dual-target build, trace-harness concept) are not copyrightable
and are already ours. What a permissive upstream license would buy, ranked:

1. **Polyfish's memory extractor — highest value by a wide margin.** We locked in real-game parity
   from Phase 0, which is the most expensive decision in this plan. A working Unity/IL2CPP memory
   injector's value lives in specific struct offsets and multi-version handling — the one category of
   artifact that *cannot* be reimplemented from the idea alone. Plausibly a month of schedule.
2. **tribes-rl's `docs/` porting specs, `tools/` trace harness, and trace fixtures.** Ruleset
   documentation and test data — the tedious correctness work with no research value.
3. **`web/` renderer and `image-gen/` sprite pipeline.** Requirement-5 time, zero research value.
4. **The C11 engine core** — lowest value; we are rewriting it and want to own it.

Two second-order reasons that matter more than the code:

- **Our own license hygiene.** If our engine is arguably derivative of an unlicensed repo, we cannot
  cleanly MIT-license our release, which guts the benchmark-paper path (that contribution is
  worthless if nobody can legally use it).
- **Velocity.** Without a license, clean-room discipline means read-then-spec-then-implement,
  ideally by different people. Absurd for a solo project. A permissive license means just reading the
  code and working.

Legal position: no license = all rights reserved (no copying, modifying, or distributing). GitHub's
ToS *does* grant the right to view and fork public repos on GitHub, but grants nothing about use
outside GitHub, modification, or distribution. Ideas and facts are not copyrightable; expression is.

**Action:** email HenBOMB (Polyfish) first, then pham-tuan-binh (tribes-rl). Ask specifically — "would
you consider adding MIT or Apache-2.0?" — rather than vaguely; most `NOASSERTION` repos simply never
picked a license. Offer credit, upstream contributions, or co-authorship. These are two of roughly
four people who have ever worked seriously on this problem, so this is also the collaboration email.
