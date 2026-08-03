# Wire protocol

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

### `apply`
```jsonc
params: {"index": 47}    // an action slot index; must be legal
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

  "players": [
    {"id": 0, "tribe": "imperius", "human": true, "alive": true,
     "stars": 5, "score": 1200, "income": 3,
     "techs": ["organization", "riding"]}
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
     "population": 3, "next_level_at": 4,     // population may be NEGATIVE
     "capital": true, "walled": false, "besieged": false,
     "units": 1, "capacity": 3, "pending_rewards": 0,
     "production": 3}
  ],

  // The legal action set for the CURRENT decision only.
  "legal": [
    {"index": 47, "kind": "tile",   "label": "Warrior"},
    {"index": 1030, "kind": "verb", "label": "Move"},
    {"index": 1024, "kind": "train","label": "Warrior (2*)"}
  ]
}
```

### Rules the UI must honour

1. **Never construct an action index by arithmetic.** Only ever send an `index` that came from
   `legal`. Legality is the engine's job; the UI ranks and displays, nothing more. This is the
   same contract the agent will operate under, which is the point — the human plays through the
   identical interface, so a UI that feels wrong is evidence the action space is wrong.
2. **Re-render entirely from each `StateView`.** Do not mutate a local copy of the game state.
   The engine is the single source of truth; a client-side prediction that drifts is a bug that
   looks like a rules bug.
3. `population` can be negative — render it as such (red), never clamp it.
4. `hp` is a float and can land on `.5`.
5. The three phases drive the interaction: click a tile (SELECT) → a verb menu appears, filled
   from `legal` (VERB) → legal targets highlight (TARGET). `Deselect` always returns to SELECT.
