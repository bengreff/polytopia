# web/ — browser UI and Node bridge

The Phase 0 way to actually play the engine. Zero npm dependencies, zero build step, zero
network access required: `server.js` uses only Node built-ins, and the UI is plain ES modules,
canvas 2D, and one stylesheet. Nothing is fetched from a CDN and nothing is bundled.

This whole directory is scaffolding around the C++ core and is deleted when the WASM target
lands and the renderer talks to the module directly (docs/phase0-plan.md §6).

## Running it

```sh
# against the real engine (default: ./build/cli/poly-cli --rpc)
node web/server.js

# against the built-in fixture engine — no C++ build needed
node web/server.js --mock

# options
node web/server.js --port 9000 --host 0.0.0.0 --cli /path/to/poly-cli
PORT=9000 POLY_CLI=/path/to/poly-cli node web/server.js
```

Then open <http://localhost:8080/>.

`GET /health` reports which engine is in use, whether the binary exists, and the last engine
error. The badge in the top-left of the UI shows the same thing (`MOCK ENGINE` / `ENGINE` /
`NO ENGINE`).

If the binary is missing or dies, `POST /rpc` answers with a normal
`{"id":…, "error":"…"}` envelope explaining what happened — the server never crashes on a
missing or misbehaving child, and the next request retries the spawn.

## `--mock`

`--mock` replaces the child process with a small fixture engine implemented inside
`server.js`. It speaks the same wire protocol (`new_game`, `state`, `apply`, `advance_bots`,
`reload_rules`) and implements a real SELECT → VERB → TARGET phase machine, fog, bot turns with
log lines, fractional HP, and a capital that starts on **negative population** — so the UI's
harder requirements are exercised without the C++ side existing.

It is a UI test fixture, **not** a rules implementation. Nothing in it should ever be treated as
game behaviour.

## Playing

- Click a highlighted tile to select a unit or city (SELECT).
- Pick an action from the floating menu or the Actions panel (VERB).
- Click one of the highlighted target tiles (TARGET).
- `Esc` or right-click sends Deselect when it is legal.
- `Enter` ends the turn, `F` fits the board, `I` toggles tile ids.
- Mouse wheel zooms, drag pans.

Bots run automatically via `advance_bots` whenever the decision belongs to a non-human player;
their log lines land in the Log panel.

**The UI never constructs an action index.** A click is converted to a tile index, that index is
looked up in the engine's `legal` array, and the entry found there is sent back verbatim. A click
with no matching legal entry does nothing. Every button in the HUD carries an engine-supplied
`legal` entry. Legality is entirely the engine's job.

**There is no client-side game state.** Every response replaces the previous `StateView` outright
and the frame is redrawn from it. No prediction, no patching, no local mutation.

## Files

| File | Job |
|---|---|
| `server.js` | static files + `POST /rpc` bridge + `--mock` fixture engine |
| `js/app.js` | bootstrap, RPC client, command methods, bot loop |
| `js/state.js` | `StateView` indexing and legal-action lookup |
| `js/render.js` | **all** canvas drawing; one named function per glyph |
| `js/projection.js` | **all** tile ↔ screen maths; nothing else knows the coordinate system |
| `js/input.js` | mouse and keyboard |
| `js/hud.js` | DOM panels, dialogs, floating verb menu |

## Swapping to isometric

`projection.js` is the only file that knows a tile is a square. To move to the isometric view the
real game uses, reimplement these and change nothing else:

- `tileToScreen(x, y)` → screen px of the tile's visual centre
- `screenToTile(px, py)` → tile coords under a screen point (the inverse)
- `tilePolygon(x, y)` → the tile outline, a rhombus instead of a square
- `tileBox(x, y)` → the largest useful axis-aligned box inside the tile, for glyphs and text
- `worldSize()` → pixel extent of the board
- `drawOrder()` → iteration order; iso needs back-to-front painting, the square grid does not care

`render.js` fills `tilePolygon()` rather than rectangles it computes itself, and places every
glyph relative to `tileBox()` / `tileToScreen()` / `tilePixels()`, so tile shape and depth order
change with the projection and the layout logic does not move. `hud.js` anchors the floating verb
menu with `tileToScreen()` for the same reason.

Sprites replace glyphs the same way: rewrite the body of `drawTerrainGlyph`, `drawUnitGlyph`,
`drawCityGlyph`, `drawResourceGlyph`, `drawImprovementGlyph`, `drawRoadGlyph`, `drawFogGlyph`.
The frame loop at the bottom of `render.js` does not need to change.
