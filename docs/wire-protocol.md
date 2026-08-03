# Wire protocol v2

**v2 supersedes v1.** The `legal` array is GONE. v1 handed the UI the agent's legality mask,
which can only say *no* — see `docs/ui-rebuild-plan.md` §1. v2 exposes annotated options
instead, and adds the queries a human interface needs: layers, reachability, and combat
preview.


The contract between the C++ core and the browser UI. Frozen so the UI and the engine can be
built in parallel.

**Transport.** `poly-cli --rpc` reads newline-delimited JSON on stdin and writes one
newline-delimited JSON response per request on stdout. `web/server.js` (Node, zero npm
dependencies, built-in `http` only) serves static files and forwards `POST /rpc` to that child
process. The game is turn-based, so there is no server push and therefore no WebSocket framing —
which is the only reason this is dependency-free on both sides. The whole bridge is deleted when
the WASM target lands and the renderer talks to the module directly.

**Request:** `{"id": <int>, "method": "<name>", "params": {...}}`
**Response:** `{"id": <int>, "result": {...}}` or `{"id": <int>, "error": "message"}`

---

## Methods

### `map_info` — how many players this map can seat
Player count is bounded by settlement sites, and `new_game` REFUSES a game with more players
than the map can seat. The UI calls this when the map size changes and clamps its opponent
picker, rather than offering counts generation will then reject.
```jsonc
params: {"width": 11, "height": 11, "land_ratio": 0.6}
result: {"max_players": 5, "expected_villages": 5}
```
Needs no game in progress. An 11x11 seats 5; a 20x20 seats 15.

### `new_game`
```jsonc
params: {
  "width": 11, "height": 11, "seed": 12345,
  "mode": "domination",              // domination | perfection | glory | creative
  "turn_limit": 0,                   // 0 = none, and 0 is the default. The UI sends no
                                     // turn limit at all; games end by domination and
                                     // Perfection sets its own.
  "fog": true,
  "players": [
    {"tribe": "imperius", "human": true,  "difficulty": "normal"},
    {"tribe": "random",   "human": false, "difficulty": "normal"}
  ]
}
result: <StateView>
```
`"tribe": "random"` (or an empty/unknown name) picks an unused tribe from a generator seeded
off `seed`, so a seed reproduces the whole matchup. The UI sends `random` for EVERY opponent:
an unmet tribe's identity is hidden by the knowledge filter, and letting the player pick it
would spoil that. Training pins tribes by name through the same entry point.

### `state`
```jsonc
params: {"player": 0}    // omit or -1 for full observability (god view)
result: <StateView>
```

### `act` — the human-scale action
One complete action, driven through the phase machine server-side. The UI thinks in
"build a Farm here"; the agent thinks in three micro-decisions. Both route through the same
`apply()`, so there is one implementation of what an action *does*. Rolls back on failure.
```jsonc
params: {"tile": 47, "verb": "build", "target": 1091}   // tile: -1 for research / end turn
result: <StateView> + {"log": ["Ruin: Resources -- +10 stars"]}
```
The result is a StateView with one extra key, `log`: the lines this action produced. It is not
decoration. A ruin's reward is rolled SERVER-SIDE, so the log line is the only place the answer
exists, and the UI raises it as a popup rather than filing it under the bot chatter. Lines are
`"Ruin: <name> -- <what you got>"`, `"City reached level N"`, `"Attack for N, retaliation M"`,
and generic summaries (`"Trained a unit"`) that the UI already says in more detail.

### `options` — what can happen here, and why not
Returns the FULL option set, annotated. This is the core of v2.
```jsonc
params: {"tile": 47, "layer": "unit" | "city" | "tile", "include_unavailable": true}
result: {"options": [
  {"verb":"train", "label":"Warrior",  "status":"available",     "cost":2, "target":1024},
  {"verb":"train", "label":"Rider",    "status":"missing_tech",  "cost":3, "needs_tech":"Riding"},
  {"verb":"train", "label":"Defender", "status":"cannot_afford", "cost":3, "target":1027},
  {"verb":"train", "label":"Archer",   "status":"blocked",       "cost":3, "reason":"too many"}
]}
```
`status` is one of `available` | `cannot_afford` | `missing_tech` | `blocked`.
**`cost` is always populated**, including when unaffordable — the price must stay legible.

One `target` caveat the UI has to absorb:
- `move` and `attack` carry `target: 0` and it is **meaningless** — those are map markers, and
  their targets come from `reachable` and `preview_combat` respectively.

The self-targeting verbs (`capture`, `examine`, `harvest`) now carry their own tile as
`target`, so the client sends back what the engine gave it and knows nothing about which verbs
target themselves. (Fixed 2026-08-03; the UI's `SELF_TARGET_VERBS` workaround is deleted.)

### `research_options`
Same shape, for the tech tree. `params: {"include_unavailable": true}`

### `layers` — what a tile has, in tap-cycle order
```jsonc
params: {"tile": 47}
result: {"layers": ["unit", "city"]}
```
**A city tile never yields a `tile` layer.** You cannot build under your own city, and the
real game offers no such layer — a phantom third layer would cost an extra tap on every city.

### `reachable` — movement markers
```jsonc
params: {"tile": 47}                  // tile holding the selected unit
result: {"tiles": [12, 13, 24, ...]}
```
Computed by the ENGINE from true state, never by the client. Zone of control from a
fog-hidden enemy still truncates movement, so a client computing this from visible tiles
would both draw markers the engine rejects and leak the hidden unit's position.

### `preview_combat` — the hover readout
```jsonc
params: {"from": 47, "to": 58}
result: {"valid":true, "attacker_damage":5, "defender_damage":3,
         "defender_dies":false, "attacker_dies":false}
```
Server-side because a client cannot see defence bonuses it has no tech for.

`valid` means **this attack is legal right now** — attacker is the current player's, is not
spent or freshly trained, has not already attacked, has not moved without Dash, and the target
is within weapon range. It is the attack equivalent of `reachable`, and the UI paints its red
rings straight from it. (Clarified 2026-08-03: the first implementation returned `valid` for
any two hostile units, which made the UI either draw markers `act` then refused or re-derive
weapon range client-side. Same reasoning as `reachable`.)

### `apply` — raw phase-machine step (agent path; UI should use `act`)
```jsonc
params: {"index": 47}
result: <StateView>
```

### `advance_bots`
Runs every non-human player until it is a human's decision again, or the game ends.
```jsonc
params: {}
result: {"state": <StateView>, "log": ["Bardur trains Warrior", ...]}
```

### `reload_rules`
Re-reads `data/rules/live.json` without restarting. This is the Phase 0 gate requirement:
change a value, reload, play the change.
```jsonc
params: {}
result: {"ok": true, "version": "..."}
```

---

## StateView

Everything the UI needs for one render. Fog is already applied server-side: with `fog: true`
and a real `player`, unexplored tiles are omitted from `tiles` entirely. **The client is never
sent information the player cannot see** — a UI bug must not be able to leak the map.

```jsonc
{
  "dims": {"width": 11, "height": 11},
  "turn": 3,
  "current_player": 0,
  "phase": "select",                 // select | verb | target
  "selected": 47,                    // tile index of the selected actor, or -1
  "pending_verb": "move",            // meaningful in the target phase
  "outcome": null,                   // null while ongoing; else {"how","winner","turn"}

  // A city with an UNRESOLVED level-up, or -1. The engine refuses every other verb while one
  // is outstanding, so this is not a hint: the UI must raise a TRUE MODAL on it with no
  // dismiss path. Without one the board silently stops responding and the game looks frozen.
  "pending_reward_city": -1,

  // Is there ANYTHING at all left to do — units, cities, tiles AND research. This, and never
  // "have my units moved", is what lights up End Turn. Unit exhaustion alone rushes the
  // player past buildings, harvests and tech they can still afford.
  "has_actions": true,

  // KNOWLEDGE-FILTERED. Score and city count are permanently public; stars, income and
  // techs are NEVER visible for anyone but yourself, and tribe identity requires having
  // met them (or Diplomacy). Unknown fields are null, not zero — a zero is
  // indistinguishable from a real value and is still a leak the moment it stops being zero.
  "players": [
    {"id": 0, "known": true,  "tribe": "imperius", "human": true, "alive": true,
     "score": 1200, "cities": 1, "stars": 5, "income": 3, "techs": ["organization"]},
    {"id": 1, "known": false, "tribe": null,       "human": false, "alive": true,
     "score": 1100, "cities": 1, "stars": null, "income": null, "techs": null}
  ],

  // Only explored tiles when fog is on. Absent index = unexplored, render as cloud.
  "tiles": [
    {"i": 47, "terrain": "field", "resource": "fruit", "improvement": "none",
     "owner": 0, "city": 2, "road": false}
  ],

  "units": [
    {"id": 3, "type": "warrior", "owner": 0, "tile": 47,
     "hp": 10.0, "max_hp": 10.0, "moves_left": 1,
     "veteran": false, "turn_ended": false, "carried": null}
  ],

  "cities": [
    {"id": 2, "owner": 0, "tile": 47, "level": 2,
     // population and next_level_at are null on cities you do not own.
     "population": 3, "next_level_at": 4,     // population may be NEGATIVE
     "capital": true, "walled": false, "besieged": false,
     "units": 1, "capacity": 3, "pending_rewards": 0,
     "production": 3}
  ]
}
```

### Rules the UI must honour

1. **Every discovered tile is selectable.** Selection is NOT gated on any action set. That was
   the core interaction bug in v1: tiles with no legal action became unclickable, so most of
   the board was inert.
2. **Render the full option set.** Show `cannot_afford` in red with the price still legible.
   Show `missing_tech` with the requirement and no commit button — there is no padlock icon in
   this game. Annotate `blocked` in place, e.g. a full city still LISTS the unit, suffixed
   "(too many)". Never delete an option silently.
3. **Never construct a target index by arithmetic.** Use the `target` from an option.
4. **Re-render entirely from each `StateView`.** Do not mutate a local copy of the game state.
   The engine is the single source of truth; a client-side prediction that drifts is a bug that
   looks like a rules bug.
5. `population` can be negative — render it as such (red), never clamp it.
6. `hp` is a float and can land on `.5`.
7. **`pending_reward_city` is blocking.** Raise a modal with no way out but an answer. Every
   other verb is refused until it is resolved, so anything less presents as a frozen game.
8. **`has_actions` owns the End Turn cue.** Do not re-derive "am I finished" from `turn_ended`.

### Known gaps (engine side, harmless to the current UI)

- The StateView still carries the v1 `legal` array. Nothing reads it; it is dead payload.
- `production` and `pending_rewards` are serialised for cities you do not own. The UI renders
  neither for an enemy city, but the knowledge filter should be covering them like
  `population`.
