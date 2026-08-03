# Workflow

How sessions run on this project. Read alongside `README.md` (the plan) and
`docs/research-findings.md` (the verified evidence base).

---

## Settings

| Setting | Choice |
|---|---|
| **Autonomy** | Direct commits to `main`. No PR gate. |
| **Cadence** | Long runs that the owner kicks off. No scheduled agent. |
| **Blockers** | **Active session (owner present): ALWAYS stop and ask.** Autonomous run: judge by reversibility. |
| **First milestone** | Phase 0 — a playable, verified, tweakable game on both machines. Then Phase 1 — benchmark agents and the Elo ladder. |

The blocker rule is asymmetric on purpose. If the owner is in the conversation, asking costs one
message and removes all guessing. If a long autonomous run is underway, stopping wastes the run, so
default on anything cheap to undo and queue anything expensive.

**Autonomous-run reversibility split**

*Default and continue, logged in the commit message:* code structure, naming, file layout, test
organization, build configuration, library choice, refactors, documentation wording.

*Stop and queue:* game rules semantics where sources disagree; action-space or observation schema;
metrics database schema; reward shaping definitions; anything that invalidates recorded experiment
results; training runs over an hour; anything that costs money; anything sent outside these machines.

Rules semantics is on the stop list permanently. A wrong rule is silent, propagates into every
downstream experiment, and is the documented cause of the largest failure in this space.

---

## Environment

| | Machine | Role |
|---|---|---|
| **Dev** | This Mac (Apple Silicon) | All iteration. **The sim must build and run natively here.** Small-scale training via PyTorch MPS. |
| **Training** | Windows desktop | Big training runs only. **No remote access configured yet** — until it is, I write code and hand over commands to run. |

Consequences worth remembering:

- **No CUDA on the dev machine.** The environment is a CPU-parallel C++ sim, so this is fine for
  iteration; only large runs need the desktop.
- **The Windows box is also the game-tooling box.** PolyMod, BepInEx runtime inspection, and
  Polyfish's extractor are all Windows-first, so real-game parity tracing and the base-game AI
  report live there, not here.
- **Setting up OpenSSH on the Windows machine is high value and cheap.** It converts "hand over
  commands" into "launch and monitor jobs directly." Worth an hour before term starts.

## Stack

| Layer | Choice | Note |
|---|---|---|
| Engine core | **C++20** | PolyEnv is C++20 and its ruleset is the locked source — same-language lift, not a port. |
| Native build | clang (macOS ARM64), MSVC or clang-cl (Windows x64) | |
| Browser build | **WASM via Emscripten** | One artifact plays on both machines. No second renderer to keep in sync. |
| Python bindings | pybind11 | |
| Training | **PyTorch** | MPS on the Mac for iteration, CUDA on the desktop for real runs. |
| Rule params | JSON in `data/rules/` | Change a value, reload, play the change. |

## Git

- Direct commits to `main`. Keep it green; if a commit leaves the build broken, say so in the message.
- Commit messages state what changed, what was decided, and what was assumed.
- Tag before anything risky so it is easy to walk back.
- MIT or Apache-2.0 license file in the first commit.

---

## Current state

**Phase:** pre-implementation. No code written.

**Done**
- Plan and architecture (`README.md`)
- Verified prior-art survey (`docs/research-findings.md`)
- License requests sent 2026-08-03 to Binh Pham (tribes-rl, email) and HenBOMB (Polyfish, GitHub
  Discussion) — awaiting replies (`docs/outreach.md`)

**Next: Phase 0 — a playable, verified, tweakable game.** Nothing blocks starting this.

Order of work:
1. Repo scaffolding, license, build system for three targets (macOS ARM, Windows x64, WASM)
2. Core state representation and the `SELECT → VERB → TARGET` phase machine
3. Ruleset extracted from PolyEnv into `core/`, parameters lifted into `data/rules/*.json`
4. Map generation: sizes, land/water ratio, biomes, tribe assignment, seeds, fixed start positions
5. Game modes: Might (domination) and Score/Perfection
6. WASM build and web UI
7. Golden-trace harness

**Deferred: base-game AI report.** Requires Polytopia installed plus Windows tooling. Not on this
Mac (Steam library checked — Polytopia absent). Static decompilation is sufficient and does not
require running the game: ILSpy or dnSpy against `Assembly-CSharp.dll` for a Mono build, or
Il2CppDumper against `GameAssembly` plus `global-metadata.dat` for IL2CPP. Deliverable when
unblocked: `docs/base-game-ai.md` — decision procedure, whether any lookahead exists, how the
difficulty handicap is implemented, and honest strength relative to the 2020 rule-based agent.

Prior expectation to test: a hand-tuned utility or behaviour-tree scorer with no lookahead,
functionally close to the 2020 paper's rule-based agent — which nonetheless finished 2nd of 6.

---

## Open questions

1. **Is Polytopia purchased, and on which platforms?** Steam is preferred; that is what Polyfish's
   extractor targets. Needed for real-game parity tracing and the base-game AI report.
2. **Windows desktop specs** — GPU, VRAM, OS version. Determines batch sizes and whether Phase 4
   self-play is feasible locally or needs cloud.
3. **Should I set up OpenSSH on the Windows box**, or do you prefer running commands manually?
4. **Repo visibility** — public from the start, or private until Phase 1 has numbers? Public earlier
   is better for the collaboration angle with the tribes-rl and Polyfish authors.
5. **How much time per week during term** for reviewing what landed? Under an hour means commits
   should be smaller and more heavily narrated.
