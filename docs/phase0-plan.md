# Phase 0 implementation plan — a playable, versatile engine

**Deliverable:** a game Ben can sit down and play in a browser against a bot, on an arbitrary map,
with any of the 12 regular tribes, in any supported mode, with every rule constant editable in JSON.

**Status of this document:** plan, not yet implemented. Supersedes nothing in `README.md`; it is the
concrete expansion of README §5 Phase 0 and WORKFLOW.md "Order of work".

---

## 0. Decisions taken for this phase

Four questions were put to the owner on 2026-08-03. Answers, and what each one binds:

| Question | Answer | Binds |
|---|---|---|
| Build path to a playable screen | **Native core + browser UI over a local process bridge.** WASM added to the same build later. | §3, §6 |
| Rules canon when sources disagree | **The live game.** Wiki-sourced now, golden-traced against the real client later. A `tribes-2020` JSON preset reproduces the paper's ruleset for the Phase 1 replication gate. | §2, §4 |
| First playable scope | **All 12 regular tribes up front**, both Might and Perfection. Engine generic beyond that. | §4, §7 |
| Art | **Original vector/canvas renderer.** Heavy text labels on icons are acceptable. Must be a foundation prettier graphics can be dropped into. | §6 |

Two decisions taken by me under WORKFLOW.md's "default and continue" rule, both code-structure and
both flagged here because they read as deviations from `README.md`:

- **Static library with a narrow C ABI, not literally header-only.** README §3 lists tribes-rl's
  "header-only C core" as a thing to steal. The property actually worth stealing is *one rule
  implementation feeding every consumer*, which a static library plus a stable C facade delivers
  identically. Header-only C++20 across 12 tribes' worth of content would mean recompiling the world
  on every constant change, which directly costs the iteration speed this phase exists to buy.
- **The bridge to the browser is a Node process, not a C++ web server.** Detail in §6. It adds zero
  C++ dependencies and zero npm dependencies, and it is deleted when WASM lands.

---

## 1. Non-negotiables this phase must establish

These are the four failure modes `README.md` §3 identifies in prior work. Each gets a structural
defence now, while the code is small enough for it to be free.

**1. Terminal states and horizon cutoffs are different types.** Polyfish returned `0.0` for both, so
capturing a capital looked identical to a draw inside its search. Our status enum is
`Ongoing | Decided(winner) | TurnLimit | HorizonCutoff`, and the value API returns
`std::optional<Outcome>` rather than a float — there is no representation in which the two can be
confused.

**2. No API can express "value from the mover's perspective at depth d".** Polytopia allows
consecutive same-player moves, so Polyfish's flip-sign-per-edge backprop made nodes prefer children
that were bad for them. Every evaluation and outcome in our API is either a per-player array or takes
an explicit `PlayerId`. Depth parity is never a legal way to derive perspective, because the
signature does not offer it.

**3. Legality is the engine's job.** Nothing outside `core/` ever constructs an action and hopes. The
engine emits the legal set; consumers rank it. This holds for the UI, the bot, and later the network.

**4. Determinism is total.** Seeded PCG32 lives inside the state. Same seed plus same action sequence
yields a bit-identical state, asserted by a test that hashes the final state. This is what makes
replays, the trace harness, and reproducible bug reports possible, and it is very expensive to
retrofit.

---

## 2. Rules sourcing

The canonical ruleset is the current live game. Two research jobs feed this, both running now:

- `docs/rules-research/live-rules.md` — wiki-derived implementation spec: exact combat formula, full
  unit/tech/building/terrain tables, city level rewards, economy, vision, ruins, all 12 tribes, mode
  victory conditions and score values. Every number carries a source URL. Numbers that could not be
  confirmed go in an explicit `UNRESOLVED` section.
- `docs/rules-research/polyenv-audit.md` — read-only audit of PolyEnv (MIT). Not a source of truth
  any more, given the canon decision; its value is as a **cross-check** on the wiki numbers and as a
  read on how outdated it is (notably whether it implements the old Boat/Ship/Battleship line or the
  newer naval rework).

**Nothing gets implemented from memory.** A rule with no source line in `live-rules.md` is not
implemented; it goes on the UNRESOLVED list and I ask. WORKFLOW.md puts rules semantics on the
permanent stop-and-ask list, and this is how that is honoured in practice.

Where the two research docs disagree, the wiki wins and the disagreement is logged — those specific
rules become the first fixture set for the Phase 0 trace harness, because a disagreement is a
prediction about where our clone is most likely wrong.

---

## 3. Core architecture

```
core/
  include/poly/
    types.hpp      PlayerId, TileIdx, UnitId, enums. No logic.
    rules.hpp      Rules: every tunable constant. Loaded once per process, shared, const.
    state.hpp      GameState: trivially copyable POD. No heap, no pointers.
    action.hpp     Action encoding, phase machine, legality masks.
    game.hpp       The whole public surface: legal(), apply(), status(), observe().
  src/
    mapgen.cpp  combat.cpp  city.cpp  economy.cpp  tech.cpp  unit.cpp
    action.cpp  serialize.cpp  rules_load.cpp
  tests/
  capi/poly_c.h  Narrow C ABI. The single seam every consumer binds to.
```

### GameState is a POD

No `std::vector`, no pointers, no virtuals, nothing heap-allocated. Fixed-capacity arrays with
compile-time caps (`MAX_TILES = 1024` for up to 32×32, `MAX_UNITS`, `MAX_CITIES`, `MAX_PLAYERS = 16`).
The struct is `std::is_trivially_copyable_v`, asserted at compile time.

This matters because a forward model that can be cloned with `memcpy` is the difference between a
search that works and one that does not. Target `sizeof(GameState)` is roughly 32 KB; clone copies
only the live prefix, so an 11×11 game copies far less.

`Rules` is deliberately **not** inside `GameState` — it is passed as `const Rules&` to every
function. One shared instance across thousands of parallel environments, and state clones stay small.

Every mutating call also returns an `UndoRecord`. Phase 0 does not use it — copy-based cloning is
correct and simple, and this phase is about correctness. But apply/undo is the standard next
optimisation for search, and retrofitting it into an API that never allocated a place to put it is a
rewrite. The seam costs nothing now.

### The action space: SELECT → VERB → TARGET

README §3 calls this the single most important design decision in the problem, and it is built first.
A turn is a variable-length sequence of micro-decisions through a three-phase machine:

- **SELECT** — pick the actor. Units and cities both sit on tiles, so the action index *is* the tile
  index. There is also a global pseudo-actor for research and end-turn.
- **VERB** — pick what that actor does: Move, Attack, Capture, Build, Train, Upgrade, Research,
  Promote, EndTurn, Deselect. Small enum.
- **TARGET** — pick the target: a tile, a unit type, a tech, a building, or a level-up reward.
  Interpretation depends on the verb.

Because slot meaning is reinterpreted per phase, the head size is
`max(MAX_TILES, NUM_VERBS, NUM_TARGET_TYPES)`. For the 11×11 competitive format that is 121 tile
slots plus roughly 80 non-tile target slots — **about 201**.

That lands within a couple of slots of tribes-rl's `Discrete(197)`, arrived at independently. Two
designs converging on the same number is the strongest available evidence that this decomposition is
the natural one, and it is the concrete form of README §1.1's claim that the branching factor is a
measurement artifact: ~201 is smaller than Go's ~250.

The engine emits a legality mask per decision. Illegal actions are not merely discouraged, they are
not representable.

### Rule parameters as data

`data/rules/live.json` is authored; `rules_load.cpp` parses it once into the `Rules` POD. JSON parsing
never touches the hot loop. `data/rules/tribes-2020.json` is a second preset reproducing the paper's
constants for the Phase 1 replication gate, which is what lets us keep that gate without letting
six-year-old rules contaminate the default.

Structure lives in code, parameters live in data — README §3 steal-item 5. Concretely: *that a
Rammer deals splash damage* is code; *how much* is JSON.

---

## 4. Content scope

**Tribes:** all 12 regular — Xin-xi, Imperius, Bardur, Oumaji, Kickoo, Hoodrick, Luxidoor, Vengir,
Zebasi, Ai-Mo, Quetzali, Yădakk. Starting tech, terrain bias, and any unique unit each.
Special tribes (Aquarion, Elyrion, Polaris, Cymanti) are out of scope per README §2 — but the engine
does not hardcode their absence, since two prior teams independently excluded them for surface-area
reasons rather than structural ones.

**Modes:** Might/Domination and Perfection at minimum, expressed as a data-driven `VictoryCondition`
(`{domination, turn_limit, score_at_turn, capital_capture_rules}`) rather than an `if` ladder. Glory
and Creative fall out of that config nearly free; they get switched on if the wiki research pins their
exact conditions.

**Starting conditions, fully parameterised** — this is the "any starting conditions" requirement:
map size and land/water ratio, biome mix, seed, player count 2–16, per-player tribe and difficulty,
fog on/off, turn limit, fixed capital positions, and **fully-specified fixed maps** loaded from
`data/maps/*.json` for reproducible testing. Full-observability mode is required, not optional:
README §5 Phase 2 runs with fog off as a derisking lever.

---

## 5. Map generation

Port the QuasiStellar Polytopia-Map-Generator algorithm — both Tribes and PolyEnv derive from it, and
it is what the community treats as matching real-game distributions. Parameterised on size,
land/water ratio, relief, per-tribe terrain bias, resource density, capital spacing, and seed.

Plus a fixed-map loader, so that the moment a rules bug is found by playing, it becomes a permanent
regression fixture rather than a memory.

---

## 6. Playing it

### Bridge

`poly-cli --rpc` speaks newline-delimited JSON-RPC over stdin/stdout. `web/server.js` is a zero-npm-
dependency Node script using only the built-in `http` module: it serves the static UI and forwards
`POST /rpc` to the child process.

The game is turn-based, so there is no need for server push and therefore no need for WebSocket
framing, which is the only reason this can be dependency-free on both sides. No C++ networking
library, no npm install, nothing to keep patched. The entire bridge is deleted when the WASM target
lands and the same renderer talks directly to the module — which is why it is worth almost nothing
and should cost almost nothing.

RPC surface: `new_game(config)`, `legal()`, `apply(action)`, `undo()`, `state(player)`,
`advance_bots()`, `save()`, `load()`, `reload_rules()`.

`reload_rules()` is the Phase 0 gate requirement — edit `data/rules/live.json`, hit reload, see the
change without restarting.

### Renderer

Plain ES modules, canvas 2D, no build step. `web/js/`: `app.js`, `render.js`, `projection.js`,
`input.js`, `hud.js`.

v1 draws a top-down square grid with text labels on tiles and units — legible, fast to build, and
unambiguous when verifying rules, which is the actual job here. **All tile-to-screen maths is
isolated in `projection.js`**, so the isometric view the real game uses is a single-file swap, and
sprites later replace glyph draws inside `render.js` without touching anything else. That is the
"foundation for prettier graphics" requirement, discharged by keeping exactly one function aware of
the coordinate system.

The UI drives the phase machine directly: click a tile (SELECT), a verb menu appears filled from the
legality mask (VERB), legal targets highlight (TARGET). The human plays through the identical
interface the agent will, so a UI that feels wrong is evidence the action space is wrong.

### Opponent

`bots/scripted.cpp` — a clone of the in-game bots, per the brief. Research findings §6 establish what
those actually are: a utility scorer with no lookahead, plus an income handicap of 1/2/3/5 bonus stars
per turn for Easy/Normal/Hard/Crazy and an aggression knob. So: score every legal action with a
weighted heuristic, take the best, repeat until EndTurn wins. Difficulty is the star grant plus
aggression weighting.

This is roughly the 2020 paper's rule-based agent, which finished **2nd of 6** against actual search
agents — a genuinely adequate sparring partner for finding rules bugs, which is all it needs to be
this phase. It is throwaway; Phase 1 replaces it with the six properly reproduced baselines.

---

## 7. Milestones and gates

Estimates are working-time, sequential. Content depends on the research docs landing.

| # | Milestone | Gate | Est. |
|---|---|---|---|
| M0 | Repo scaffold: `git init`, MIT license, CMake + Ninja, core skeleton, test runner, bench harness | `cmake --build` green, one test passes, bench prints a steps/sec number | 0.5d |
| M1 | `types/rules/state/action` + phase machine + legality + apply. Movement and capture only, no content | Random-legal-action self-play runs 10k games without an assert; determinism test passes | 2d |
| M2 | Full content: combat, units, techs, buildings, terrain, resources, city growth/levels, economy, all 12 tribes | Every formula in `live-rules.md` has a unit test with the wiki number; UNRESOLVED list is empty or escalated | 4d |
| M3 | Map generation + fixed-map loader | Generated 11×11 maps are visually and statistically plausible; fixed maps round-trip | 1.5d |
| M4 | Modes, victory conditions, score | Might and Perfection both terminate correctly; score matches wiki values | 1d |
| M5 | JSON-RPC bridge + web UI | A full game is playable start to finish; `reload_rules()` visibly changes play | 2.5d |
| M6 | Scripted bot with difficulty tiers | Bot plays a legal complete game unsupervised; Crazy beats Easy well above chance | 1.5d |
| M7 | **Ben plays it.** Fix what feels wrong | README §5 Phase 0 gate: full game on both machines, JSON rule change takes effect | — |

Deferred out of this plan, deliberately: the golden-trace harness (README §5 lists it under Phase 0,
but it needs Polytopia installed plus Windows tooling — WORKFLOW.md already records it as blocked),
the WASM target (additive, and nothing depends on it until public deployment), and pybind11 bindings
(nothing consumes them until Phase 1).

---

## 8. Testing, from M0 rather than later

- **Formula tests** — every combat/tech-cost/growth number from `live-rules.md`, asserted against its
  cited source. These are the tests that matter; a wrong rule is silent and propagates into every
  downstream experiment.
- **Invariant fuzzing** — random legal playthroughs asserting stars never go negative, unit and city
  counts stay consistent, serialisation round-trips, no illegal action is ever emitted.
- **Determinism** — seed + action list produces an identical state hash. Run on every build.
- **Performance** — steps/sec printed on every bench run. tribes-rl's 1.8M/sec on comparable hardware
  is the reference point. Tracking it from M0 means a regression is attributable to one commit
  instead of being discovered at Phase 4 when it is unfixable.

---

## 9. What could go wrong

| Risk | Response |
|---|---|
| Wiki is incomplete or wrong on specific rules | UNRESOLVED list is explicit and escalated, never guessed. Those rules become the first trace-harness fixtures. |
| Live-game rules have drifted from every reference implementation | Expected, and precisely why canon is the live game. PolyEnv's audit measures the size of that drift. |
| 12 tribes up front slows time-to-playable | Content is table-driven, so tribes are mostly data. The genuine cost is unique units, which is small. |
| `MAX_TILES = 1024` inflates the action head for large maps | Head size is a compile-time config. Competitive 11×11 is ~201. Large maps are a Phase 0 versatility feature, not a training target. |
| POD state constraint gets violated as content grows | `static_assert(std::is_trivially_copyable_v<GameState>)`. Breaks the build, not the search. |
