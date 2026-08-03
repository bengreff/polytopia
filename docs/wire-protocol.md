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

### `new_game`
```jsonc
params: {
  "width": 11, "height": 11, "seed": 12345,
  "mode": "domination",              // domination | perfection | glory | creative
  "turn_limit": 0,                   // 0 = none
  "fog": true,
  "players": [
    {"tribe": "imperius", "human": true,  "difficulty": "normal"},
    {"tribe": "bardur",   "human": false, "difficulty": "normal"}
  ]
}
result: <StateView>
```

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
result: <StateView>
```

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
  ],

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
2. **Re-render entirely from each `StateView`.** Do not mutate a local copy of the game state.
   The engine is the single source of truth; a client-side prediction that drifts is a bug that
   looks like a rules bug.
3. `population` can be negative — render it as such (red), never clamp it.
4. `hp` is a float and can land on `.5`.
5. The three phases drive the interaction: click a tile (SELECT) → a verb menu appears, filled
   from `legal` (VERB) → legal targets highlight (TARGET). `Deselect` always returns to SELECT.
