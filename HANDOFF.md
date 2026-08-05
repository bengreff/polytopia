# Handoff

State of the project, what to do next, and why. Read this before `README.md` — that file is the
original plan and parts of it have been overtaken by what the build actually found.

**Run it:** `./run.sh`, then open <http://localhost:8080>.

```
./build/cli/poly-cli --selfplay 20            whole-engine smoke test
./build/cli/poly-cli --match rulebased random --games 20
./build/cli/poly-cli --ladder --games 8        round robin, ~10 min
./build/core/tests/poly_tests                  89 tests
```

---

## 1. Where things stand

**Playable.** Full rules for the 12 regular tribes: movement with terrain and zone of control,
combat with the verified damage formula, capture, harvest, build, train, research, city growth
and level rewards, naval embark/upgrade, roads and city connections, temples, veterancy,
explorers, super units, healing, conversion, disband and destroy. Fog with a server-side
knowledge model. Default environment is the base game's 16×16 with four tribes.

**Measured.** Seven agents and an Elo ladder that runs the published experimental setup in
about ten minutes. The playable baseline (`rulebased`) beats every other agent at roughly 70%.

**Verified.** 89 tests, clean under `-Werror -Wconversion` and ASAN/UBSAN.

---

## 2. Do this first: a bot worth playing against

The stated goal is games against a decent opponent. `rulebased` is that opponent, and it is
**not good enough yet** — 75% against *random*, which is weak for a scripted policy.

It wins by domination at ~69 turns rather than on score, which localises the problem: the
losses are **over-extension, not passivity**. It pushes units at villages and enemy cities and
leaves its capital thin. In priority order:

1. **A defensive term.** Keep a body within reach of the capital when an enemy unit is closer
   to it than a defender is. This is almost certainly the single largest win available and is
   maybe thirty lines in `best_move`.
2. **Do not walk wounded units into contact.** `badly_hurt` exists and gates Recover; the move
   scorer ignores it entirely, so a 2-HP Warrior still charges a city.
3. **Spend down before ending the turn.** The bot hoards stars. Score counts units at 5 points
   per star of cost and techs at 100 per tier, so unspent stars are worth literally nothing.
4. **Then re-measure against `random` at 20+ games.** Anything below ~90% means it is still
   losing games it should not.

**A warning from experience.** Two of my strong design instincts about this bot were simply
wrong, and only measurement caught them: units-first selection cost **55 points** of win rate
(80% → 25%), and gating attacks on a favourable damage *ratio* made it sit on its army and
never close games out. Change one thing, measure, keep or revert. Do not batch changes.

---

## 3. The defect to fix before any search work

**The action space contains free no-ops.** `Select` → `Deselect` costs two decisions and
changes nothing, so an agent can burn unbounded decisions inside a turn without ending it.

This has now degraded **four separate agents**:

| Agent | Symptom |
|---|---|
| MCTS | stalled at ~2400 decisions/turn; 3654s per pairing, games recorded as fake draws |
| paper2020 | re-selected the same city forever; **lost to random at 16.7%** |
| rulebased (v1) | scored Select/Deselect as if they were moves; rated 1044, barely above random |
| RHEA | burned plan budget on no-ops |

I patched around it four times instead of fixing it. It punishes deterministic policies
specifically, inflates effective branching on every search node, and Phase 2 is search-first.
**Close it before starting Phase 2.** The likely fix is to make `Deselect` illegal when the
current actor has taken no action, so the cycle cannot be entered — but check that this does
not break the human UI, which legitimately needs "changed my mind".

---

## 4. Known gaps

- **No map-type presets.** Generation is smoothed-noise blobs against a land quota. Continents,
  Pangea, Archipelago, Lakes, Waterworld and their water bands do not exist. `land_ratio` is the
  only lever.
- **Village and ruin densities are UNVERIFIED.** The documented formula
  `(floor(w/3)² − capitals) × 0.3` is integer-quantised and ignores height; on an 11×11 it gave
  2 villages against 4 ruins. Replaced with area-based density in `Rules`, so real counts can be
  dialled in as data without a rebuild. Measure them in game.
- **`web/server.js --mock` speaks wire protocol v1** and cannot serve the current UI. Port or
  delete.
- **Cloak/Dagger infiltrate, Stomp damage, and the special tribes** are unimplemented.
- **`rulebased` is duplicated content-wise with `paper2020`** in places; both are scripted and
  share targeting patterns. Worth extracting a common scripted base if a third one appears.

---

## 5. Rules confidence

`docs/rules-research/resolutions.md` is the file to read before touching any rule. It records
every open question with an authority level and, critically, **59 values flagged `_unverified`
in `data/rules/live.json`** — the set of things we might be wrong about is queryable rather
than folklore.

Highest-value things still to check in the live client, cheapest first:

1. Whether a besieged city really produces nothing (R4, the owner marked this uncertain).
2. Splash rounding — settles whether HP must be fractional (UNRESOLVED #1; currently sidestepped
   by storing HP in half-point units).
3. Building costs: Lumber Hut / Port / Market.
4. Real village counts per map size.
5. Whether an enemy city's info panel shows its star production (U-H; implemented as visible
   per the owner, which contradicts the UI research).

**A lesson worth carrying.** `OWNER` authority is strong but not infallible: the initial vision
radius was answered as "1 tile" and was actually 5×5, and a derived arithmetic identity that
reproduced 16 published turn-0 scores beat the recollection. When a self-test disagrees with a
remembered rule, give the self-test a real hearing.

---

## 6. What the build actually taught us

Recorded because these change how the remaining phases should be approached.

- **Almost every real bug was found by running the game, not by testing it.** The action-slot
  collision, the `GameState` padding trap, the turn-limit regression, the fog-viewer leak, and
  four agent failures — none caught by unit tests. The README's argument for building the UI
  before the agent held up better than expected.
- **A legality mask can only say *no*.** Wiring the human UI to it produced an interface where
  most tiles were unclickable and actions silently vanished. The fix — one enumerator, two
  projections — is the architecture to preserve: `legal_actions()` is a *projection* of
  `enumerate_options()`, so the agent's view and the human's menu cannot drift.
- **Legality generation, not the forward model, was the search bottleneck.** `apply()` was
  recomputing `legal_actions` to validate, so every search step paid twice. Fixing that plus
  hoisting a territory test gave 3.75×. Profile before optimising; the obvious culprit was not
  the real one.
- **Converging sources can be confidently wrong.** All four wiki sources said temples reach max
  at 12 turns; in-game measurement says 8. PolyEnv contains two Market implementations and the
  *dead* one was correct. The wiki's flat-5 starting stars is stale for every tribe.
- **The published 2020 ordering is no longer a gate.** It was dropped deliberately: the
  baselines are weak, the scripted agents beat them, and reproducing a six-year-old ranking is
  not what this project is for. The ladder remains valuable as a *relative* measure.
