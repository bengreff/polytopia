# web/ — browser UI and Node bridge

The Phase 0 way to actually play the engine. Zero npm dependencies, zero build step, zero
network access required: `server.js` uses only Node built-ins, and the UI is plain ES modules,
canvas 2D, and one stylesheet. Nothing is fetched from a CDN and nothing is bundled.

This whole directory is scaffolding around the C++ core and is deleted when the WASM target
lands and the renderer talks to the module directly (docs/phase0-plan.md §6).

**This UI speaks wire protocol v2** (`docs/wire-protocol.md`). v1's `legal` array is gone and
nothing here replaces it — see `docs/ui-rebuild-plan.md` §1 for why that array was the bug.

## Running it

```sh
node web/server.js                       # against ./build/cli/poly-cli --rpc
node web/server.js --port 9000 --host 0.0.0.0 --cli /path/to/poly-cli
PORT=9000 POLY_CLI=/path/to/poly-cli node web/server.js
```

Then open <http://localhost:8080/>. Or just `./run.sh`, which builds first.

`GET /health` reports which engine is in use, whether the binary exists, and the last engine
error. If the binary is missing or dies, `POST /rpc` answers with a normal
`{"id":…, "error":"…"}` envelope — the server never crashes on a missing or misbehaving child,
and the next request retries the spawn.

⚠️ **`--mock` is stale.** The fixture engine inside `server.js` still answers only the v1 method
set (`new_game`, `state`, `apply`, `advance_bots`, `reload_rules`). It does not implement
`options`, `layers`, `reachable`, `preview_combat`, `research_options` or `act`, so the current
UI cannot run against it. Build the C++ engine instead; `--mock` needs porting to v2 or deleting.

## Playing

- **Click any discovered tile.** Selection is never gated on an action set — every explored
  tile selects and says something about itself, including enemy tiles and empty ocean.
- **Click the same tile again to cycle its layers** (unit → city). A city tile has no `tile`
  layer, by design: you cannot build under your own city.
- The bottom-left card is the **description panel**; under it is the **option tray**, which
  renders the *entire* option set the engine returns:
  - normal blue button with a gold price — available
  - dark red with the price still legible — you cannot afford it; clicking explains
  - grey italic `(locked)` with the required tech — no commit button, offers a `TECH TREE` jump
  - dimmed with the reason in place, e.g. `Warrior (too many)` — blocked

  Nothing is ever silently removed, and there is no padlock icon anywhere.
- A **round action button floats over the selected tile** carrying its primary action.
- **Cyan rings** on the map are move destinations; **red rings under enemies** are attack
  targets; a **bright yellow ring plus a skull** is a guaranteed one-hit kill. Hovering an
  attackable enemy shows the two-way damage preview with a death warning for both sides.
- Every commit is **two taps** — an info popup, then `DO IT`. There is no undo in this game.
- `Esc` deselects (and closes the popup / tech tree), `Enter` ends the turn, `T` opens the tech
  tree, `Space` jumps to your next unit that still has something to do, `F` fits the board.
  Mouse wheel zooms, drag pans, right-click deselects.

Bots run automatically via `advance_bots` whenever the decision belongs to a non-human player;
their log lines land in the Log panel.

**The UI never constructs an action index.** Targets come from the `target` field of an option
or from the tile list returned by `reachable`. The one exception is documented in `js/state.js`
(`SELF_TARGET_VERBS`) and `js/app.js` (`targetFor`): the engine leaves `target` at 0 for the
self-targeting verbs, so the UI falls back to the tile the engine already told it was selected.
That is a protocol gap, not a licence to compute indices.

**Knowledge.** The StateView is already filtered server-side. The UI shows only *your* stars and
income; an opponent's stars render as `?`, never as `0`, and their income and techs are not
displayed at all. Score and city count are public and are shown. Every RPC pins `player` to your
own seat, because the engine otherwise defaults the fog viewer to whoever is to move — which is
the bot the moment you end your turn.

**There is no client-side game state.** Every response replaces the previous `StateView` outright
and the frame is redrawn from it. No prediction, no patching, no local mutation. The only things
cached between frames are the engine's own answers to `options`, `reachable` and
`preview_combat` for the current selection, and they are discarded on every change.

## Files

| File | Job |
|---|---|
| `server.js` | static files + `POST /rpc` bridge (+ a stale v1 `--mock` fixture) |
| `js/app.js` | RPC client, selection state machine, `act` dispatch, bot loop |
| `js/state.js` | `StateView` indexing + the label vocabulary. No rules. |
| `js/render.js` | **all** canvas drawing; one named function per glyph |
| `js/projection.js` | **all** tile ↔ screen maths; nothing else knows the coordinate system |
| `js/input.js` | pointer and keyboard → tile indices |
| `js/hud.js` | HUD, description panel, option tray, confirm popup, tech tree overlay |

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
change with the projection and the layout logic does not move. `hud.js` anchors the floating tile
action button with `tileToScreen()` for the same reason, via `positionTileAction()`.

Sprites replace glyphs the same way: rewrite the body of `drawTerrainGlyph`, `drawUnitGlyph`,
`drawCityGlyph`, `drawResourceGlyph`, `drawImprovementGlyph`, `drawRoadGlyph`, `drawFogGlyph`,
`drawMoveRing`, `drawAttackRing`. The frame loop at the bottom of `render.js` does not change.
