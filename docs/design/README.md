# Design docs: championship-scope Polytopia AI

Working design for a superhuman 1v1 Might player. These are brainstorming documents and change often;
the decision log below records what is settled.

| Doc | What it covers |
|---|---|
| [scope-and-rules.md](scope-and-rules.md) | The exact target format, and the rules that shape the design |
| [architecture.md](architecture.md) | Turn planner + network evaluator, and where ML sits inside the planner |
| [training-and-data.md](training-and-data.md) | Warm start, data sources, self-play, gates, compute |
| [open-questions.md](open-questions.md) | Decisions that need Ben, and unknowns we must measure |
| [reviews/2026-10-20.md](reviews/2026-10-20.md) | Adversarial review findings, and what was done with each |

## Goals (Ben, 2026-10-20)

- **End goal:** beat top humans, plus a **research write-up**. The second means experiments must be
  reproducible and logged from day one.
- **Timeline:** a first working model **within ~2 days that beats the default bots**, then ongoing
  background training with no deadline.
- **Proof:** no public demonstration against top humans is needed.
- **Involvement:** **checkpoints at each stage.** Ben plays the bot, reviews, and approves the next
  stage.

## Decision log

| Date | Decision | Why |
|---|---|---|
| 2026-10-20 | Scope is the World Championship format only: 1v1 Might, Lakes, Small 14×14, 12 regular tribes on both sides | Minimal scope with full championship capability |
| 2026-10-20 | Exact rules come from the real game code via polyhost, never a re-implementation | Avoids the bug class that stalled Polyfish |
| 2026-10-20 | Architecture: search over turns, with a learned network as evaluator and move proposer. **v5:** per-decision Gumbel search (not plan-level beam), heavy search at play time only | Search gives superhuman tactics; learning gives strategic judgement. v5: review findings 4, 10 |
| 2026-10-20 | **No hand-written evaluator.** Hand-written code states facts only: legality, infrastructure, canonical position hash, derived facts such as reachability. **v5: canonical move ordering dropped** | Hand-written evaluation caps strength (Ben's call). v5: ordering wasn't provably safe (review #2) |
| 2026-10-20 | Warm start from built-in bot games, plus human replays if we can get them | Day-one competence without hand-written evaluation |
| ~~2026-10-20~~ | ~~Long-run training is expert iteration~~ → **v5: decided by the M3 A/B** (PPO vs expert iteration at equal simulator time; expert iteration needs ≥ 50 Elo) | Search multiplies simulator steps ~15×; decide by measurement (review #12) |
| 2026-10-20 | **Local hardware only** (M2 Pro Mac + RTX 4070 Super), no cloud | Ben's call; the plan optimizes learning per game |
| 2026-10-20 | **Local play only**; no official-server play | Ben's call; no real-client interface needed |
| 2026-10-20 | Human data = public championship replay links (`share.polytopia.io/g/<GUID>`), loaded through the client's replay path | Ben's pointer; exactly our format; collect IDs now, since replays may expire |
| 2026-10-20 | "Beats Ben" gate (v5) = pooled vs Crazy with a Wilson lower bound ≥ 90%, no matchup cell below 70%, then a best-of-5 against Ben | The v4 per-cell gate was statistically unpassable (review #8) |
| 2026-10-20 | **v5 milestones:** M0 infrastructure + measurement → M1 BC policy (≥ 60% vs Normal) → M2 turn sampler (beats the default bots) → M3 PPO-vs-expert-iteration A/B → M4 beats Ben → M5+ layers added on evidence | Smallest learning loop first; one change per measured run (review #5, #6) |
| 2026-10-20 | Policy = pointer over the game's own legal-command list | Covers every command type (diplomacy, rewards, …) with no masks to keep in sync (review #3) |
| 2026-10-20 | Topology: **target = everything on PC `backhouse`** (native Steam `GameAssembly.dll` actors on 20 cores + 4070 Ti SUPER + learner); fallback = Mac emulator actors with PC inference and learning | The PC has 2× the cores and 4× the RAM of the Mac with no emulator; the native-build spike is now top priority |
| 2026-10-21 | **The Android emulator is temporary.** It stays as (1) the working environment until a native host exists, and (2) a reference oracle: same seed + commands must give byte-identical states on both builds. It is retired once the Windows `GameAssembly.dll` host passes that test. A native macOS `GameAssembly.dylib` host can replace it on the Mac too | polyhost binds to the game only through the exported `il2cpp_*` API (dlopen/dlsym), which `GameAssembly.dll`/`.dylib` also export, so the port is mostly loader and path changes |
| 2026-10-20 | M2 target = **beat Crazy** (pooled lower bound ≥ 50%), via an Easy→Crazy curriculum | Ben: "default bots" means Crazy |
| 2026-10-20 | **Layer 0 interface built** (`polyhost/env.hpp`, `py/polyenv.py`): matches are built as the real app builds a **pass-and-play** game, verified byte-identical | The championship is two humans; pass & play is the client path for that, and it offers Might |
| 2026-10-20 | Legal commands = the game's own generators (`CommandUtils`, `GetMovementOptions`, `GetAttackOptions`), each checked with `IsValid`; **not** an `IsValid` sweep | `IsValid` alone accepts commands the client never offers (captures without the start-of-turn rule, recovers at full health, other tribes' abilities) |
| 2026-10-20 | The AI's `StayCommand` (marks a unit done) is not in the agent's list; data generation plays it via `STEP_BOT_OWN`. `EndMatch` is sent automatically once the game reaches FinalTurn | The client never offers Stay to a person; EndMatch is valid only at FinalTurn, and the AI sends it at once |
| 2026-10-21 | **The built-in AI keeps its full view** when it plays demonstration games; only our agent is fog-limited (observation allowlist, proven by the M0 fog-leak test). Supersedes "drop imitation of moves that depend on unexplored information" | Ben: its omniscience stands in for guessing what is in the fog; our bots must never get that information |
| 2026-10-21 | M1 teacher = the game's AI on both **person** seats of the exact championship build (handicap 1, AutoPlay off → `GetStupidity` 0). This is the "Crazy-brain, Normal-income" teacher | `PlayerState.GetStupidity` = max(0, 2 − handicap) for AutoPlay players, 0 otherwise |
| 2026-10-21 | First-move edge, AI vs AI on the championship build: first seat won 924/1732 = 53.3% (95% CI 51.0–55.7%) | Small but real; the early 6/6 was noise |
| 2026-10-20 | Observation v1 = allowlist of what the viewer's client shows. Enemy unit XP, age and home city are withheld; opponent stars and techs are withheld | Fog principle (scope-and-rules.md); the client shows veteran status, not XP |

## Iteration log

- **v1** (2026-10-20): first draft of all four docs.
- **v2** (2026-10-20): self-review pass. Added the planner cost model and the replan-on-reveal
  rule, split pruning into "safe" and "learned", and added a fog-leak test and an
  out-of-distribution guard for the warm-started value. See the "v2 changes" notes in each doc.
- **v3** (2026-10-20): second review. Every candidate plan is now replayed on every fog sample (avoids
  strategy fusion). Added a per-unit intent head (more ML in the planner at low cost), a seat input,
  a 60-turn training cap scored as a draw, and opponent-visibility checks to measure.
- **v4** (2026-10-20): Ben's answers. Local-only compute (removed the cloud stage, added an honest
  local budget and hardware topology). Championship replay sources researched (share links in
  YouTube descriptions, Challengermode brackets; 13,852 qualifier players in 2026).
  Defined the "beats Ben" gate.
- **Layer 0** (2026-10-20): interface built and checked (see architecture.md "Layer 0 — status").
- **v5** (2026-10-20): four adversarial reviews (ML, rules, systems, strategy) → near-total rewrite.
  - Fog model corrected: explored tiles are visible live; only invisible units, unexplored tiles
    and stars are hidden.
  - Pointer policy over the legal-command list; canonical ordering dropped; per-decision Gumbel
    search; heavy search at play time only.
  - Cost model rebuilt (~3 ms per node).
  - Lean milestones M0–M5 with definitions of done, Ben checkpoints and kill criteria.
  - Statistical gates; CI suite against silent bugs; PPO-vs-expert-iteration A/B.
  - Full dispositions in reviews/2026-10-20.md.
