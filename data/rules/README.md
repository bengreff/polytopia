# `data/rules/`

Every tunable number in the engine lives here, not in code (`docs/phase0-plan.md` §3 — structure in
code, parameters in data). `core/include/poly/content.hpp` says *what exists*; this directory says
*what the numbers are*.

`live.json` targets **The Battle of Polytopia v2.16.3**, scope = the 12 regular tribes.

## Structure

Top level is 13 keys:

| Key | Contents |
|---|---|
| `version` | Ruleset identifier, `"live-2.16.3"`. Bump when the numbers change. |
| `combat` | Damage formula constants, defence bonuses, healing, veterancy, vision radii, Infiltrate. |
| `economy` | Tech cost formula, Literacy discount, adjacency rules, one-time star sources, ability costs. |
| `movement` | Move costs, rough terrain, Zone of Control, turn-ending actions, forced-spawn push order. |
| `units` | One entry per `UnitType` (19), plus `_naval`. |
| `improvements` | One entry per `Improvement` (12), plus `_temple_growth`, `_road`, `_bridge`, `_embassy`, `_monument`. |
| `resources` | One entry per `Resource` (9). |
| `city` | Population thresholds, income, capacity, borders, capture, level-up rewards. |
| `score` | Per-item score values, used by Perfection and Glory. |
| `tribes` | One entry per `Tribe` (12), plus `_base_rates` for map generation. |
| `modes` | One entry per `GameMode` (4), plus `_map_sizes` and `_ruin_counts`. |
| `difficulty` | One entry per `Difficulty` (4). |
| `_unverified` | See below. |

### Key naming

Keys inside `units`, `improvements`, `resources`, `tribes`, `modes` and `difficulty` are the
`content.hpp` enumerator names, lowercased with underscores — `UnitType::MindBender` → `mind_bender`,
`Tech::FreeSpirit` → `free_spirit`, `Tribe::XinXi` → `xin_xi`. String *values* that name a tech, a
skill, a unit or a terrain use the same convention (`Terrain` lives in `types.hpp`:
`field` / `forest` / `mountain` / `water` / `ocean`).

**A key beginning with `_` is never an enumerator.** It is shared or auxiliary data that has no enum
member — `improvements._temple_growth`, `units._naval`, `_unverified`. A loader that iterates a
section must skip `_`-prefixed keys; a loader that looks entries up by enum name will never see them.

### `null` conventions

- `null` in `_unverified` means **genuinely unknown** — no source states it. Do not substitute a
  guess; the loader treats a missing/null value as a recoverable default (`json.hpp` accessors are
  total by design).
- `modes.*.turn_limit: null` means *no turn limit*, not unknown. Same for `modes.creative.victory`.
- `units.*.tech: null` means the unit needs no technology (Warrior, Giant, Explorer).
- `tribes.luxidoor.start_tech: null` is correct — Luxidoor is the one tribe with no starting tech.

## What `_unverified` means

`_unverified` is a flat array of dotted paths. A path appears there when the value is either:

1. **Unknown** — no source gives it, so the value is `null` (`combat.stomp_damage`,
   `improvements._embassy.cost`); or
2. **Sourced only to `OWNER` or `ASSUMED` authority** in `docs/rules-research/resolutions.md` —
   i.e. Ben's play experience, or a default chosen between conflicting/absent sources. The value is
   real and implementable, but it has not been confirmed against the client or a primary source.

That makes the set of things we might be wrong about **queryable rather than folklore**. It is the
verification backlog; `resolutions.md` §"Verification backlog" ranks what to measure first.

Nothing outside those two categories is flagged. `PRIMARY`, `CORROBORATED` and `DERIVED` values —
including the tech cost formula, the Literacy discount and Luxidoor's level-3 capital — are settled
and unflagged.

Every path in `_unverified` is checked to resolve against the document. Keep it that way: if you
delete or rename a key, fix the path too.

## Notable corrections already applied

These contradict at least one wiki page. They are correct; do not "fix" them back.

- **Port is unlocked by Fishing (T1)**, not Sailing. Two wiki pages are stale.
- **Swordsman has no Fortify** (removed in 2.8).
- **Bomber attack is 3** (nerfed in 2.8.5).
- **Explorer makes 12 moves**, not 15 (v2.16.3).
- **Forge builds on Field *or* Forest** and has no adjacency prerequisite for placement — adjacency
  only scales its output.
- **Market is 1 per city**, and its income is the **sum of the levels** of adjacent Sawmills,
  Windmills and Forges — not a count of unique building types.
- **Temples**: 5 levels, growth every 2 turns, max at 8 turns, 100 score per level, 500 max. Every
  wiki source saying 12 turns is wrong.
- **Whale and the Boat/Ship/Battleship line do not exist.** Naval is Raft/Scout/Rammer/Bomber plus
  Juggernaut/Dinghy/Pirate, and a hull carries **no HP of its own** (`hp_from_carried_unit`) — the
  embarked land unit's HP is used, and damage carries both ways.
- **Tech cost** is `tier * num_cities + 4`; with Literacy, `ceil(cost * 2 / 3)`. Both verified in
  the live client.

Two entries deserve a note because the sources genuinely disagree:

- `city.starting_vision_radius` is **2** (25 tiles). `resolutions.md` R18 says a 1-tile radius, but
  the turn-0 score identity — which R12 makes an engine self-test, and which reproduces all 16
  published turn-0 scores — requires 25 explored tiles. Flagged; measure it.
- `tribes.oumaji` and `tribes.kickoo` carry `water: 1.0`. Their documented water multipliers (0.5 and
  2.0) are dead values the real game never applies, so the engine implements them as no-ops.

## Changing a value and reloading

1. Edit `data/rules/live.json`.
2. If you changed something's authority — resolved an unknown, or verified an `OWNER`/`ASSUMED`
   value in-game — remove its path from `_unverified` and record the finding in
   `docs/rules-research/resolutions.md`. The JSON is the parameter store; `resolutions.md` is the
   audit trail. They must not drift.
3. Validate: `python3 -m json.tool data/rules/live.json > /dev/null`. The C++ reader
   (`core/include/poly/json.hpp`) is a hand-written minimal parser — **no comments, no trailing
   commas, no `\u` escapes**.
4. Rebuild/rerun. `Rules` is loaded once per process and passed as `const Rules&`; there is no
   hot reload. Nothing is baked at compile time, so a rebuild is not required for the data itself —
   but any test that hardcodes a number will need updating.
5. Re-run the engine self-tests. The turn-0 score identity (`score.turn0_base` + 5×unit cost +
   100×tech tier + 50×(capital level − 1) = the 12 published turn-0 scores) catches most mistakes in
   `tribes`, `units` and `score` immediately.

## Sources

- `docs/rules-research/resolutions.md` — what the engine does, and the authority for each rule.
  **Overrides the research doc wherever they differ.**
- `docs/rules-research/live-rules.md` — the sourced wiki/patch-note spec, with per-number citations.

No value in `live.json` originates anywhere else.
