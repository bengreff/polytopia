/*
 * projection.js — THE ONLY FILE THAT KNOWS THE COORDINATE SYSTEM.
 *
 * Every tile <-> screen conversion in the renderer, the input handler and the
 * HUD goes through this module. Nothing else may multiply a tile coordinate by
 * a tile size, and nothing else may assume a tile is a square.
 *
 * v1 is a top-down square grid. The real game is isometric. Swapping to iso is
 * a change to THIS FILE ONLY: reimplement tileToScreen / screenToTile /
 * tilePolygon / tileBox / worldSize / drawOrder and everything downstream
 * follows, because render.js draws tile shapes via tilePolygon() and places
 * glyphs via tileBox(), never via rectangles it computes itself.
 *
 * Contract:
 *   tileToScreen(x, y)  -> {x, y}  screen px of the tile's visual CENTRE
 *   screenToTile(px, py)-> {x, y}  tile coords (may be out of bounds; caller checks)
 *   tilePolygon(x, y)   -> [[px,py], ...] the tile's outline, closed implicitly
 *   tileBox(x, y)       -> {x, y, w, h} largest useful axis-aligned box inside
 *                          the tile, for text and glyphs
 *   worldSize()         -> {w, h} pixel extent of the whole board
 *   drawOrder()         -> [{x, y}, ...] painter's-algorithm order (iso needs
 *                          back-to-front; square does not care)
 */

const view = {
  tileSize: 64, // unscaled px per tile edge
  zoom: 1,
  panX: 0,
  panY: 0,
  vw: 800, // viewport (canvas CSS px)
  vh: 600,
  width: 11, // board dims in tiles
  height: 11,
};

const MIN_ZOOM = 0.35;
const MAX_ZOOM = 3.0;

export function setDims(width, height) {
  view.width = Math.max(1, width | 0);
  view.height = Math.max(1, height | 0);
}

export function setViewport(cssWidth, cssHeight) {
  view.vw = cssWidth;
  view.vh = cssHeight;
}

export function getView() {
  return { ...view };
}

/** px per tile edge, after zoom. Internal to this module by design. */
function scale() {
  return view.tileSize * view.zoom;
}

/** Pixel extent of the whole board. */
export function worldSize() {
  const s = scale();
  return { w: view.width * s, h: view.height * s };
}

/** Top-left corner of the board in screen space, board centred plus pan. */
function origin() {
  const { w, h } = worldSize();
  return {
    x: Math.round((view.vw - w) / 2 + view.panX),
    y: Math.round((view.vh - h) / 2 + view.panY),
  };
}

/** Tile (x, y) -> screen px of its visual centre. */
export function tileToScreen(x, y) {
  const s = scale();
  const o = origin();
  return { x: o.x + x * s + s / 2, y: o.y + y * s + s / 2 };
}

/** Screen px -> tile coords. May be out of bounds; callers must range-check. */
export function screenToTile(px, py) {
  const s = scale();
  const o = origin();
  return { x: Math.floor((px - o.x) / s), y: Math.floor((py - o.y) / s) };
}

/** The tile's outline. A square now; a rhombus under an iso projection. */
export function tilePolygon(x, y) {
  const s = scale();
  const c = tileToScreen(x, y);
  const h = s / 2;
  return [
    [c.x - h, c.y - h],
    [c.x + h, c.y - h],
    [c.x + h, c.y + h],
    [c.x - h, c.y + h],
  ];
}

/**
 * Largest useful axis-aligned box inside the tile, for glyphs and text.
 * Under iso this shrinks (a rhombus holds a smaller box) but callers do not
 * need to know that.
 */
export function tileBox(x, y) {
  const s = scale();
  const c = tileToScreen(x, y);
  return { x: c.x - s / 2, y: c.y - s / 2, w: s, h: s };
}

/** Nominal glyph radius; renderers size text and icons off this. */
export function tileRadius() {
  return scale() / 2;
}

/** Current on-screen tile edge in px. Renderers use it for LOD decisions. */
export function tilePixels() {
  return scale();
}

/** Painter's-algorithm iteration order over the board. */
export function drawOrder() {
  const out = [];
  for (let y = 0; y < view.height; y++) {
    for (let x = 0; x < view.width; x++) out.push({ x, y });
  }
  return out;
}

/** True if a tile coordinate is on the board. */
export function inBounds(x, y) {
  return x >= 0 && y >= 0 && x < view.width && y < view.height;
}

/**
 * The eight neighbour offsets, in the SAME order and with the same diagonal-inclusive meaning
 * as the engine's kNeighbourOffsets (core/include/poly/types.hpp). Adjacency in this game is
 * 8-way — Mind Bender heal/convert range, road connectivity and the capital flood all use it —
 * so a 4-way version here would silently disagree with the engine about what "adjacent" means.
 */
export const NEIGHBOURS = [
  [-1, -1], [0, -1], [1, -1], [-1, 0], [1, 0], [-1, 1], [0, 1], [1, 1],
];

/**
 * Screen point half-way between this tile's centre and its neighbour's — i.e. where a path
 * drawn between two adjacent tiles crosses their shared boundary. Roads are drawn as centre
 * -> edge stubs, so two adjacent road tiles meet exactly and read as one continuous line.
 * Defined by interpolation rather than by adding half a tile width, so it stays correct under
 * an isometric projection where "half a tile east" is not a horizontal offset.
 */
export function tileEdgePoint(x, y, dx, dy) {
  const a = tileToScreen(x, y);
  const b = tileToScreen(x + dx, y + dy);
  return { x: (a.x + b.x) / 2, y: (a.y + b.y) / 2 };
}

// --- camera -----------------------------------------------------------------

export function panBy(dx, dy) {
  view.panX += dx;
  view.panY += dy;
}

export function resetCamera() {
  view.panX = 0;
  view.panY = 0;
  view.zoom = 1;
}

/** Zoom about a screen point so the tile under the cursor stays put. */
export function zoomAt(px, py, factor) {
  const before = screenToTileFloat(px, py);
  view.zoom = Math.min(MAX_ZOOM, Math.max(MIN_ZOOM, view.zoom * factor));
  const after = screenToTileFloat(px, py);
  const s = scale();
  view.panX += (after.x - before.x) * s;
  view.panY += (after.y - before.y) * s;
}

function screenToTileFloat(px, py) {
  const s = scale();
  const o = origin();
  return { x: (px - o.x) / s, y: (py - o.y) / s };
}

/** Scale the board so it fits the viewport with a margin. */
export function fitToViewport(marginPx = 24) {
  const usableW = Math.max(64, view.vw - marginPx * 2);
  const usableH = Math.max(64, view.vh - marginPx * 2);
  const need = Math.max(view.width, view.height) * view.tileSize;
  const fit = Math.min(usableW, usableH) / need;
  view.zoom = Math.min(MAX_ZOOM, Math.max(MIN_ZOOM, fit));
  view.panX = 0;
  view.panY = 0;
}
