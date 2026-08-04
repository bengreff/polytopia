/*
 * render.js — ALL drawing. No coordinate maths lives here: every position comes
 * from projection.js, so the isometric swap does not touch this file's layout
 * logic. Every visual element is drawn by a named glyph function below, so a
 * sprite sheet can replace a glyph body without disturbing anything else.
 *
 * Art direction: original vector/canvas drawing only, no game assets. Heavy
 * text labelling is deliberate — legibility beats prettiness at this stage.
 */

import * as P from './projection.js';

// --- palette ---------------------------------------------------------------

export const PLAYER_COLORS = [
  '#3fa9f5', // 0 blue
  '#ef4b4b', // 1 red
  '#f5c542', // 2 yellow
  '#a970ff', // 3 purple
  '#3ddc97', // 4 green
  '#ff8a3d', // 5 orange
];

export function playerColor(id) {
  if (id === undefined || id === null || id < 0) return '#8b8f96';
  return PLAYER_COLORS[id % PLAYER_COLORS.length];
}

const TERRAIN = {
  field: { fill: '#86ad5d', edge: '#6d914a', label: 'field' },
  grass: { fill: '#86ad5d', edge: '#6d914a', label: 'grass' },
  forest: { fill: '#39703f', edge: '#2a5730', label: 'forest' },
  mountain: { fill: '#9aa0a8', edge: '#7b818a', label: 'mtn' },
  water: { fill: '#4f9dd4', edge: '#3d80b2', label: 'water' },
  shallow: { fill: '#4f9dd4', edge: '#3d80b2', label: 'water' },
  ocean: { fill: '#2b5f9e', edge: '#224b7d', label: 'ocean' },
  ice: { fill: '#d5e6f4', edge: '#adc7dc', label: 'ice' },
  swamp: { fill: '#6d7d47', edge: '#57663a', label: 'swamp' },
  flooded: { fill: '#3f86bd', edge: '#31699a', label: 'flood' },
};

/** Stable fallback colour for a terrain the UI has not been taught yet. */
function hashColor(str) {
  let h = 0;
  for (let i = 0; i < str.length; i++) h = (h * 31 + str.charCodeAt(i)) | 0;
  const hue = Math.abs(h) % 360;
  return { fill: `hsl(${hue} 32% 52%)`, edge: `hsl(${hue} 32% 40%)`, label: str.slice(0, 6) };
}

export function terrainStyle(name) {
  return TERRAIN[name] || hashColor(String(name || 'unknown'));
}

/**
 * Water of any depth. The StateView collapses the Road and Bridge tile flags into one `road`
 * bool, so terrain is what separates the two — a road over water is a bridge. That is a
 * reading of state the engine already sent, not an invented field.
 */
const WATER_TERRAIN = new Set(['water', 'shallow', 'ocean', 'flooded']);
export function isWaterTerrain(name) {
  return WATER_TERRAIN.has(String(name || '').toLowerCase());
}

const RESOURCE_GLYPH = {
  fruit: { text: 'FRU', color: '#ff5f9e' },
  crop: { text: 'CRP', color: '#ffe066' },
  game: { text: 'GAM', color: '#c98b4b' },
  fish: { text: 'FSH', color: '#9fe6ff' },
  whale: { text: 'WHL', color: '#cfe9ff' },
  metal: { text: 'MTL', color: '#dfe3ea' },
  starfish: { text: 'STR', color: '#ffb3c1' },
  ruin: { text: 'RUIN', color: '#f0e2b6' },
  ruins: { text: 'RUIN', color: '#f0e2b6' },
};

const IMPROVEMENT_GLYPH = {
  farm: 'FARM', mine: 'MINE', port: 'PORT', forge: 'FORGE',
  lumber_hut: 'LUMBR', lumber: 'LUMBR', sawmill: 'SAWML',
  windmill: 'WINDM', temple: 'TEMPL', market: 'MARKT',
  customs_house: 'CUSTM', road: 'ROAD',
};

const UNIT_ABBREV = {
  warrior: 'WARR', archer: 'ARCH', rider: 'RIDE', defender: 'DEFN',
  swordsman: 'SWRD', catapult: 'CATA', knight: 'KNGT', giant: 'GIANT',
  mind_bender: 'MIND', cloak: 'CLOAK', dagger: 'DAGGR', raft: 'RAFT',
  scout: 'SCOUT', rammer: 'RAMMR', bomber: 'BOMBR', juggernaut: 'JUGGR',
  dinghy: 'DNGHY', pirate: 'PIRAT',
};

export function unitAbbrev(type) {
  const t = String(type || '?').toLowerCase();
  return UNIT_ABBREV[t] || t.replace(/[^a-z]/g, '').slice(0, 5).toUpperCase() || '?';
}

/**
 * The naval hulls. A unit on water is a HULL CARRYING A LAND UNIT, not a different soldier,
 * so it is drawn as a boat rather than as the standard land plate — the silhouette is what
 * tells you at a glance that this stack is at sea and cannot do land things.
 */
const NAVAL_TYPES = new Set(['raft', 'scout', 'rammer', 'bomber', 'juggernaut', 'dinghy', 'pirate']);

export function isNaval(type) {
  return NAVAL_TYPES.has(String(type || '').toLowerCase());
}

// --- canvas helpers --------------------------------------------------------

function polyPath(ctx, poly) {
  ctx.beginPath();
  ctx.moveTo(poly[0][0], poly[0][1]);
  for (let i = 1; i < poly.length; i++) ctx.lineTo(poly[i][0], poly[i][1]);
  ctx.closePath();
}

/** A boat silhouette in the same box a land unit's plate would occupy. */
function hullPath(ctx, x, y, w, h) {
  ctx.beginPath();
  ctx.moveTo(x, y);                              // deck, port side
  ctx.lineTo(x + w, y);                          // deck, starboard
  ctx.lineTo(x + w - h * 0.25, y + h * 0.62);    // starboard tumblehome
  ctx.quadraticCurveTo(x + w * 0.5, y + h * 1.28, x + h * 0.25, y + h * 0.62);
  ctx.closePath();
}

function roundRect(ctx, x, y, w, h, r) {
  const rr = Math.min(r, w / 2, h / 2);
  ctx.beginPath();
  ctx.moveTo(x + rr, y);
  ctx.arcTo(x + w, y, x + w, y + h, rr);
  ctx.arcTo(x + w, y + h, x, y + h, rr);
  ctx.arcTo(x, y + h, x, y, rr);
  ctx.arcTo(x, y, x + w, y, rr);
  ctx.closePath();
}

function label(ctx, text, cx, cy, size, color, weight = '700', align = 'center') {
  ctx.font = `${weight} ${size}px ui-sans-serif, system-ui, -apple-system, "Segoe UI", sans-serif`;
  ctx.textAlign = align;
  ctx.textBaseline = 'middle';
  ctx.lineWidth = Math.max(2, size * 0.18);
  ctx.strokeStyle = 'rgba(0,0,0,0.72)';
  ctx.lineJoin = 'round';
  ctx.strokeText(text, cx, cy);
  ctx.fillStyle = color;
  ctx.fillText(text, cx, cy);
}

// --- glyphs (swap these bodies for sprites later) --------------------------

export function drawTerrainGlyph(ctx, x, y, tile, opts = {}) {
  const st = terrainStyle(tile.terrain);
  const poly = P.tilePolygon(x, y);
  polyPath(ctx, poly);
  ctx.fillStyle = st.fill;
  ctx.fill();
  ctx.strokeStyle = st.edge;
  ctx.lineWidth = 1;
  ctx.stroke();

  const px = P.tilePixels();
  // The terrain name is dropped on occupied tiles: unit HP text owns that row.
  if (px >= 44 && opts.showLabel !== false) {
    const box = P.tileBox(x, y);
    ctx.font = `500 ${Math.round(px * 0.15)}px ui-sans-serif, system-ui, sans-serif`;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'bottom';
    ctx.fillStyle = 'rgba(255,255,255,0.42)';
    ctx.fillText(st.label, box.x + box.w / 2, box.y + box.h - px * 0.04);
  }
}

export function drawFogGlyph(ctx, x, y) {
  const poly = P.tilePolygon(x, y);
  polyPath(ctx, poly);
  ctx.fillStyle = '#1b2130';
  ctx.fill();
  ctx.strokeStyle = '#141926';
  ctx.lineWidth = 1;
  ctx.stroke();

  const px = P.tilePixels();
  if (px < 26) return;
  const c = P.tileToScreen(x, y);
  ctx.fillStyle = 'rgba(255,255,255,0.10)';
  const r = px * 0.13;
  ctx.beginPath();
  ctx.arc(c.x - r * 0.9, c.y, r, 0, Math.PI * 2);
  ctx.arc(c.x + r * 0.7, c.y - r * 0.3, r * 1.15, 0, Math.PI * 2);
  ctx.arc(c.x + r * 0.1, c.y + r * 0.5, r * 0.9, 0, Math.PI * 2);
  ctx.fill();
}

export function drawOwnerOverlay(ctx, x, y, ownerId) {
  const col = playerColor(ownerId);
  const poly = P.tilePolygon(x, y);
  polyPath(ctx, poly);
  ctx.save();
  ctx.globalAlpha = 0.16;
  ctx.fillStyle = col;
  ctx.fill();
  ctx.restore();
  ctx.strokeStyle = col;
  ctx.globalAlpha = 0.55;
  ctx.lineWidth = Math.max(1, P.tilePixels() * 0.045);
  polyPath(ctx, poly);
  ctx.stroke();
  ctx.globalAlpha = 1;
}

/**
 * One tile's share of the road network.
 *
 * `links` is the subset of P.NEIGHBOURS that also carries a road, decided by the caller from
 * the StateView. Each link is drawn from the tile CENTRE to the shared edge (P.tileEdgePoint),
 * so the neighbour's matching stub lands on the same point and the two read as one continuous
 * line. Drawing a fixed plus-sign per tile — the previous behaviour — produced a field of
 * disconnected crosses in which a network was completely illegible, and left a road dangling
 * into empty ground at every dead end.
 *
 * A road flag on a WATER tile is a bridge. The StateView collapses Road and Bridge into one
 * `road` bool, but terrain recovers the distinction without inventing anything.
 */
export function drawRoadGlyph(ctx, x, y, links, isBridge) {
  const c = P.tileToScreen(x, y);
  const px = P.tilePixels();
  const surface = isBridge ? '#c79a5e' : '#e2d6b6';
  const casing = isBridge ? 'rgba(60,38,16,0.75)' : 'rgba(40,32,16,0.55)';

  const trace = () => {
    ctx.beginPath();
    if (!links.length) {
      // An isolated road still has to be visible, so it gets a short stub rather than nothing.
      ctx.moveTo(c.x - px * 0.16, c.y);
      ctx.lineTo(c.x + px * 0.16, c.y);
      return;
    }
    for (const [dx, dy] of links) {
      const e = P.tileEdgePoint(x, y, dx, dy);
      ctx.moveTo(c.x, c.y);
      ctx.lineTo(e.x, e.y);
    }
  };

  ctx.save();
  ctx.lineCap = 'round';
  ctx.lineJoin = 'round';
  // Dark casing first, pale surface over it: the contrast is what makes the network legible
  // over both the green of a field and the blue of water.
  ctx.strokeStyle = casing;
  ctx.lineWidth = Math.max(3.5, px * 0.16);
  trace();
  ctx.stroke();
  ctx.strokeStyle = surface;
  ctx.lineWidth = Math.max(2, px * 0.095);
  trace();
  ctx.stroke();

  // A bridge gets cross planks, so it is not just "a road that happens to be on water".
  if (isBridge && px >= 30) {
    ctx.strokeStyle = 'rgba(90,58,26,0.8)';
    ctx.lineWidth = Math.max(1, px * 0.02);
    for (const [dx, dy] of links) {
      const e = P.tileEdgePoint(x, y, dx, dy);
      const mx = (c.x + e.x) / 2;
      const my = (c.y + e.y) / 2;
      const len = Math.hypot(e.x - c.x, e.y - c.y) || 1;
      const nx = -(e.y - c.y) / len;
      const ny = (e.x - c.x) / len;
      ctx.beginPath();
      ctx.moveTo(mx - nx * px * 0.05, my - ny * px * 0.05);
      ctx.lineTo(mx + nx * px * 0.05, my + ny * px * 0.05);
      ctx.stroke();
    }
  }
  // A junction cap hides the seam where several stubs meet at the centre.
  if (links.length > 1) {
    ctx.fillStyle = surface;
    ctx.beginPath();
    ctx.arc(c.x, c.y, Math.max(1.5, px * 0.05), 0, Math.PI * 2);
    ctx.fill();
  }
  ctx.restore();
}

export function drawResourceGlyph(ctx, x, y, resource) {
  const g = RESOURCE_GLYPH[resource] || { text: String(resource).slice(0, 4).toUpperCase(), color: '#ffffff' };
  const b = P.tileBox(x, y);
  const px = P.tilePixels();
  if (px < 30) {
    ctx.fillStyle = g.color;
    ctx.beginPath();
    ctx.arc(b.x + b.w * 0.78, b.y + b.h * 0.22, Math.max(2, px * 0.07), 0, Math.PI * 2);
    ctx.fill();
    return;
  }
  const w = px * 0.34;
  const h = px * 0.17;
  const rx = b.x + b.w - w - px * 0.05;
  const ry = b.y + px * 0.05;
  roundRect(ctx, rx, ry, w, h, h * 0.35);
  ctx.fillStyle = 'rgba(0,0,0,0.55)';
  ctx.fill();
  ctx.font = `700 ${Math.round(h * 0.72)}px ui-sans-serif, system-ui, sans-serif`;
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';
  ctx.fillStyle = g.color;
  ctx.fillText(g.text, rx + w / 2, ry + h / 2 + 0.5);
}

export function drawImprovementGlyph(ctx, x, y, improvement) {
  const text = IMPROVEMENT_GLYPH[improvement] || String(improvement).slice(0, 5).toUpperCase();
  const b = P.tileBox(x, y);
  const px = P.tilePixels();
  if (px < 30) return;
  const w = px * 0.42;
  const h = px * 0.17;
  const rx = b.x + px * 0.05;
  const ry = b.y + px * 0.05;
  roundRect(ctx, rx, ry, w, h, h * 0.3);
  ctx.fillStyle = 'rgba(20,16,10,0.65)';
  ctx.fill();
  ctx.strokeStyle = 'rgba(255,255,255,0.25)';
  ctx.lineWidth = 1;
  ctx.stroke();
  ctx.font = `700 ${Math.round(h * 0.68)}px ui-sans-serif, system-ui, sans-serif`;
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';
  ctx.fillStyle = '#f3e6c8';
  ctx.fillText(text, rx + w / 2, ry + h / 2 + 0.5);
}

export function drawCityGlyph(ctx, x, y, city) {
  const px = P.tilePixels();
  const c = P.tileToScreen(x, y);
  const col = playerColor(city.owner);
  const w = px * 0.86;
  const h = px * 0.27;
  const cx = c.x - w / 2;
  const cy = c.y - px * 0.46;

  roundRect(ctx, cx, cy, w, h, px * 0.06);
  ctx.fillStyle = '#181d29';
  ctx.fill();
  ctx.strokeStyle = col;
  ctx.lineWidth = city.walled ? Math.max(2.5, px * 0.055) : Math.max(1.2, px * 0.022);
  ctx.stroke();

  if (px < 26) return;
  const mid = cy + h / 2;
  const fs = Math.round(h * 0.66);

  // Level, with a capital star and a besieged bang.
  const lvl = (city.capital ? '★' : '') + `L${city.level}` + (city.besieged ? '!' : '');
  label(ctx, lvl, cx + px * 0.05, mid, fs, city.capital ? '#ffd76a' : '#ffffff', '800', 'left');

  // Population, which CAN BE NEGATIVE and is never clamped — red when below zero.
  // An enemy city's population is NULL, not zero, and nothing is drawn for it: printing a 0
  // there would be a fabricated number the player would read as real.
  if (px >= 34 && typeof city.population === 'number') {
    const pop = city.population;
    const txt = city.next_level_at != null ? `${pop}/${city.next_level_at}` : `${pop}`;
    label(ctx, txt, cx + w - px * 0.05, mid, fs, pop < 0 ? '#ff6b6b' : '#cfe0f5', '700', 'right');
  }
}

export function drawUnitGlyph(ctx, x, y, unit, opts = {}) {
  const px = P.tilePixels();
  const c = P.tileToScreen(x, y);
  const col = playerColor(unit.owner);
  const w = px * 0.72;
  const h = px * 0.30;
  const ux = c.x - w / 2;
  // Shifted down when a city plate occupies the top of the tile.
  const uy = c.y - h * 0.72 + (opts.shifted ? px * 0.1 : 0);

  ctx.save();
  // A spent unit is greyed, hard. ui-reference.md §3.5: the white outline vanishes when a
  // unit can no longer act, and an untouched unit carries a cyan glow.
  const spent = opts.spent !== undefined ? !!opts.spent : !!unit.turn_ended;
  if (spent) ctx.globalAlpha = 0.42;
  else if (opts.mine) {
    ctx.shadowColor = 'rgba(73,226,255,0.95)';
    ctx.shadowBlur = px * 0.22;
  }

  // A naval unit gets a HULL, not a plate: flat deck, angled sides, pointed bow. Shape is the
  // primary cue because it survives greying, the player-colour fill and colour blindness.
  const naval = isNaval(unit.type);
  if (naval) hullPath(ctx, ux, uy, w, h);
  else roundRect(ctx, ux, uy, w, h, h * 0.32);
  ctx.fillStyle = spent ? '#5c6472' : col;
  ctx.fill();
  ctx.shadowBlur = 0;
  // Veterans carry a thick gold border in place of the white one. Promotion is permanent and
  // changes the unit's maximum HP, so it has to be readable on the map, not only in the panel.
  ctx.strokeStyle = spent
    ? 'rgba(0,0,0,0.6)'
    : unit.veteran ? '#ffd05a' : 'rgba(255,255,255,0.92)';
  ctx.lineWidth = Math.max(1, px * (spent ? 0.02 : unit.veteran ? 0.05 : 0.028));
  ctx.stroke();

  // A mast, so a hull is unmistakable even at the smallest zoom that still draws units.
  if (naval && px >= 26) {
    ctx.strokeStyle = spent ? 'rgba(0,0,0,0.5)' : 'rgba(255,255,255,0.85)';
    ctx.lineWidth = Math.max(1, px * 0.02);
    ctx.beginPath();
    ctx.moveTo(c.x, uy);
    ctx.lineTo(c.x, uy - h * 0.55);
    ctx.stroke();
    ctx.fillStyle = spent ? '#8b91a0' : '#ffffff';
    ctx.beginPath();
    ctx.moveTo(c.x, uy - h * 0.55);
    ctx.lineTo(c.x + w * 0.20, uy - h * 0.28);
    ctx.lineTo(c.x, uy - h * 0.14);
    ctx.closePath();
    ctx.fill();
  }

  if (px >= 26) {
    ctx.font = `800 ${Math.round(h * 0.62)}px ui-sans-serif, system-ui, sans-serif`;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    ctx.fillStyle = '#0d1016';
    ctx.fillText(unitAbbrev(unit.type), c.x, uy + h / 2 + 0.5);
  }

  // HP bar + numeric HP (hp is a float and may end in .5).
  if (px >= 34) {
    const bw = w;
    const bh = px * 0.075;
    const by = uy + h + px * 0.025;
    const frac = unit.max_hp > 0 ? Math.max(0, Math.min(1, unit.hp / unit.max_hp)) : 0;
    roundRect(ctx, ux, by, bw, bh, bh / 2);
    ctx.fillStyle = 'rgba(0,0,0,0.65)';
    ctx.fill();
    roundRect(ctx, ux + 1, by + 1, Math.max(0, (bw - 2) * frac), bh - 2, (bh - 2) / 2);
    ctx.fillStyle = frac > 0.5 ? '#4ade80' : frac > 0.25 ? '#facc15' : '#ef4444';
    ctx.fill();
    const hpText = `${formatHp(unit.hp)}/${formatHp(unit.max_hp)}`;
    label(ctx, hpText, c.x, by + bh + px * 0.085, Math.round(px * 0.15), '#ffffff');
  }

  // Veteran pip, remaining moves, and an explicit word for a spent unit — colour alone is
  // not enough when the whole complaint about v1 was "I cannot tell what I can do".
  if (px >= 34) {
    // Gold chevron over the plate. Redundant with the gold border on purpose: shape plus
    // colour plus position, so veterancy survives greying and reads without colour vision.
    if (unit.veteran) {
      ctx.save();
      ctx.strokeStyle = spent ? 'rgba(255,208,90,0.55)' : '#ffd05a';
      ctx.lineWidth = Math.max(1.5, px * 0.035);
      ctx.lineJoin = 'round';
      ctx.lineCap = 'round';
      const cy = uy - px * 0.055;
      const cw = w * 0.20;
      ctx.beginPath();
      ctx.moveTo(c.x - cw, cy);
      ctx.lineTo(c.x, cy - px * 0.075);
      ctx.lineTo(c.x + cw, cy);
      ctx.stroke();
      ctx.restore();
    }
    // What the hull is carrying, which is the only thing that says what will step ashore.
    if (unit.carried) {
      label(ctx, unitAbbrev(unit.carried), c.x, uy + h + px * 0.30,
            Math.round(px * 0.13), '#cfe9ff');
    }
    if (spent) {
      // Lifted clear of the chevron when there is one, so the two never overprint.
      const sy = uy - px * (unit.veteran ? 0.20 : 0.11);
      label(ctx, 'SPENT', c.x, sy, Math.round(px * 0.13), '#c7cedb');
    } else if (typeof unit.moves_left === 'number') {
      label(ctx, `${unit.moves_left}`, ux + w + px * 0.03, uy + h * 0.5, Math.round(px * 0.16), '#e6f0ff');
    }
  }
  ctx.restore();
}

export function formatHp(v) {
  if (typeof v !== 'number') return '?';
  return Number.isInteger(v) ? String(v) : v.toFixed(1);
}

// --- movement / attack markers ---------------------------------------------
//
// ui-reference.md §4.1: concentric ring decals laid FLAT on the tile. Pale cyan-teal on move
// destinations, orange-red UNDER enemy units for attack targets. They differ in hue AND in
// placement, so they stay distinguishable without colour vision.

export function drawMoveRing(ctx, x, y) {
  const c = P.tileToScreen(x, y);
  const px = P.tilePixels();
  ctx.save();
  ctx.strokeStyle = 'rgba(73,226,255,0.95)';
  ctx.lineWidth = Math.max(2, px * 0.055);
  ctx.beginPath();
  ctx.arc(c.x, c.y, px * 0.31, 0, Math.PI * 2);
  ctx.stroke();
  ctx.strokeStyle = 'rgba(73,226,255,0.42)';
  ctx.lineWidth = Math.max(1, px * 0.03);
  ctx.beginPath();
  ctx.arc(c.x, c.y, px * 0.19, 0, Math.PI * 2);
  ctx.stroke();
  ctx.fillStyle = 'rgba(73,226,255,0.10)';
  ctx.beginPath();
  ctx.arc(c.x, c.y, px * 0.31, 0, Math.PI * 2);
  ctx.fill();
  ctx.restore();
}

/**
 * Attack marker. `preview` is the engine's two-way readout; a guaranteed one-hit kill that
 * costs you nothing gets a DISTINCT bright ring plus a skull, because it is a different
 * decision from a trade.
 */
export function drawAttackRing(ctx, x, y, preview) {
  const c = P.tileToScreen(x, y);
  const px = P.tilePixels();
  const cleanKill = preview && preview.defender_dies && !preview.attacker_dies;
  const suicide = preview && preview.attacker_dies;

  ctx.save();
  ctx.lineWidth = Math.max(2.5, px * (cleanKill ? 0.085 : 0.06));
  ctx.strokeStyle = cleanKill ? '#ffe14d' : suicide ? '#ff2d2d' : '#ff6a3d';
  ctx.beginPath();
  ctx.arc(c.x, c.y, px * 0.42, 0, Math.PI * 2);
  ctx.stroke();
  ctx.fillStyle = cleanKill ? 'rgba(255,225,77,0.20)' : 'rgba(255,90,60,0.20)';
  ctx.beginPath();
  ctx.arc(c.x, c.y, px * 0.42, 0, Math.PI * 2);
  ctx.fill();

  if (cleanKill && px >= 30) {
    // A skull, drawn not fetched: two eyes and a jaw under the ring.
    const r = px * 0.1;
    const sy = c.y + px * 0.3;
    ctx.fillStyle = '#fffbe6';
    ctx.beginPath();
    ctx.arc(c.x, sy, r, Math.PI, 0);
    ctx.rect(c.x - r, sy, r * 2, r * 0.85);
    ctx.fill();
    ctx.fillStyle = '#2a1c00';
    ctx.beginPath();
    ctx.arc(c.x - r * 0.42, sy - r * 0.12, r * 0.26, 0, Math.PI * 2);
    ctx.arc(c.x + r * 0.42, sy - r * 0.12, r * 0.26, 0, Math.PI * 2);
    ctx.fill();
  } else if (suicide && px >= 30) {
    // Black-and-red ring: retaliation kills the attacker (ui-reference.md §4.2).
    ctx.setLineDash([px * 0.09, px * 0.07]);
    ctx.strokeStyle = '#000';
    ctx.lineWidth = Math.max(2, px * 0.05);
    ctx.beginPath();
    ctx.arc(c.x, c.y, px * 0.42, 0, Math.PI * 2);
    ctx.stroke();
  }
  ctx.restore();
}

/**
 * Mind Bender convert target. Deliberately NOT a red attack ring: converting takes the unit
 * rather than killing it, and a player who reads it as an attack will mis-plan the turn.
 * Violet, dashed, and drawn at a different radius so it is separable from both other markers
 * by shape alone.
 */
export function drawConvertRing(ctx, x, y) {
  const c = P.tileToScreen(x, y);
  const px = P.tilePixels();
  ctx.save();
  ctx.setLineDash([px * 0.10, px * 0.06]);
  ctx.strokeStyle = '#c07bff';
  ctx.lineWidth = Math.max(2.5, px * 0.065);
  ctx.beginPath();
  ctx.arc(c.x, c.y, px * 0.37, 0, Math.PI * 2);
  ctx.stroke();
  ctx.setLineDash([]);
  ctx.fillStyle = 'rgba(192,123,255,0.18)';
  ctx.beginPath();
  ctx.arc(c.x, c.y, px * 0.37, 0, Math.PI * 2);
  ctx.fill();
  ctx.restore();
}

/** Faint dot on every discovered tile: everything here is clickable, and it must look it. */
export function drawSelectableDot(ctx, x, y) {
  const px = P.tilePixels();
  if (px < 26) return;
  const c = P.tileToScreen(x, y);
  ctx.save();
  ctx.fillStyle = 'rgba(255,255,255,0.13)';
  ctx.beginPath();
  ctx.arc(c.x, c.y + px * 0.34, Math.max(1, px * 0.022), 0, Math.PI * 2);
  ctx.fill();
  ctx.restore();
}

export function drawHighlight(ctx, x, y, color, alpha = 0.35) {
  const poly = P.tilePolygon(x, y);
  polyPath(ctx, poly);
  ctx.save();
  ctx.globalAlpha = alpha;
  ctx.fillStyle = color;
  ctx.fill();
  ctx.restore();
  polyPath(ctx, poly);
  ctx.strokeStyle = color;
  ctx.lineWidth = Math.max(2, P.tilePixels() * 0.06);
  ctx.stroke();
}

export function drawSelectionRing(ctx, x, y) {
  const poly = P.tilePolygon(x, y);
  polyPath(ctx, poly);
  ctx.strokeStyle = '#ffd76a';
  ctx.lineWidth = Math.max(3, P.tilePixels() * 0.08);
  ctx.stroke();
}

export function drawHoverRing(ctx, x, y) {
  const poly = P.tilePolygon(x, y);
  polyPath(ctx, poly);
  ctx.strokeStyle = 'rgba(255,255,255,0.9)';
  ctx.lineWidth = Math.max(1.5, P.tilePixels() * 0.03);
  ctx.stroke();
}

export function drawTileIdLabel(ctx, x, y, i) {
  const b = P.tileBox(x, y);
  const px = P.tilePixels();
  ctx.font = `500 ${Math.round(px * 0.14)}px ui-monospace, monospace`;
  ctx.textAlign = 'left';
  ctx.textBaseline = 'top';
  ctx.fillStyle = 'rgba(255,255,255,0.55)';
  ctx.fillText(String(i), b.x + px * 0.05, b.y + px * 0.05);
}

// --- frame -----------------------------------------------------------------

/**
 * Draw one whole frame from a GameState. Pure function of (state, ui): nothing is cached
 * between frames, which is what makes "re-render entirely from each StateView" true rather
 * than aspirational.
 *
 * `ui` carries only presentation and engine-supplied overlays:
 *   selected   tile index of the current selection, or -1
 *   reachable  Set<tileIdx> straight from the `reachable` RPC — never computed here
 *   attackable Map<tileIdx, preview> straight from `preview_combat`
 *   convertible Set<tileIdx> adjacent enemies, gated on the engine's own Convert option
 *   hover      {x, y}
 */
export function render(ctx, state, ui = {}) {
  const { width: cw, height: ch } = ctx.canvas;
  const dpr = ui.dpr || 1;
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, cw / dpr, ch / dpr);

  ctx.fillStyle = '#0d111a';
  ctx.fillRect(0, 0, cw / dpr, ch / dpr);

  if (!state || !state.width) return;

  const order = P.drawOrder();
  const reachable = ui.reachable || new Set();
  const attackable = ui.attackable || new Map();

  // 1. terrain / fog
  for (const { x, y } of order) {
    const i = state.tileIndex(x, y);
    const tile = state.tileAt(i);
    if (!tile) {
      drawFogGlyph(ctx, x, y);
      continue;
    }
    const occupied = state.unitsAt(i).length > 0 || !!state.cityAt(i);
    drawTerrainGlyph(ctx, x, y, tile, { showLabel: !occupied });
  }

  // 2. territory, roads, resources, improvements
  for (const { x, y } of order) {
    const i = state.tileIndex(x, y);
    const tile = state.tileAt(i);
    if (!tile) continue;
    if (typeof tile.owner === 'number' && tile.owner >= 0) drawOwnerOverlay(ctx, x, y, tile.owner);
    if (tile.road) {
      // Which way this road actually goes. Adjacency is 8-way, exactly as the engine's own
      // road flood is, so what is drawn is the network the engine will route through.
      // An UNEXPLORED neighbour contributes no link: guessing one would paint a road into
      // fog the player has not earned, which is a (small) map leak.
      const links = [];
      for (const [dx, dy] of P.NEIGHBOURS) {
        if (!P.inBounds(x + dx, y + dy)) continue;
        const n = state.tileAt(state.tileIndex(x + dx, y + dy));
        if (n && n.road) links.push([dx, dy]);
      }
      drawRoadGlyph(ctx, x, y, links, isWaterTerrain(tile.terrain));
    }
    if (tile.resource && tile.resource !== 'none') drawResourceGlyph(ctx, x, y, tile.resource);
    if (tile.improvement && tile.improvement !== 'none' && tile.improvement !== 'city') {
      drawImprovementGlyph(ctx, x, y, tile.improvement);
    }
  }

  // 3. affordance layer.
  //    EVERY discovered tile carries a click dot: selection is never gated on an action set,
  //    which is the bug that made v1 unplayable. Move and attack rings sit on top of that.
  for (const { x, y } of order) {
    const i = state.tileIndex(x, y);
    if (state.isExplored(i)) drawSelectableDot(ctx, x, y);
  }
  for (const t of reachable) {
    const { x, y } = state.tileXY(t);
    if (P.inBounds(x, y)) drawMoveRing(ctx, x, y);
  }
  for (const [t, preview] of attackable) {
    const { x, y } = state.tileXY(t);
    if (P.inBounds(x, y)) drawAttackRing(ctx, x, y, preview);
  }
  for (const t of ui.convertible || []) {
    const { x, y } = state.tileXY(t);
    if (P.inBounds(x, y)) drawConvertRing(ctx, x, y);
  }

  // 4. cities then units
  for (const { x, y } of order) {
    const i = state.tileIndex(x, y);
    const city = state.cityAt(i);
    if (city) drawCityGlyph(ctx, x, y, city);
  }
  for (const { x, y } of order) {
    const i = state.tileIndex(x, y);
    const units = state.unitsAt(i);
    if (!units.length) continue;
    const shifted = !!state.cityAt(i);
    const mine = units[0].owner === state.me;
    // "Spent" is the ENGINE's answer to "has this unit anything available", not turn_ended:
    // a Dash unit that has moved keeps turn_ended false but is finished if nothing is in range.
    // Only YOUR units are greyed — an enemy's leftover turn_ended flag says nothing useful
    // about what it will do on its own turn, and dimming it would imply otherwise.
    const spent = mine && (units[0].turn_ended || (ui.idle instanceof Set && !ui.idle.has(i)));
    drawUnitGlyph(ctx, x, y, units[0], { shifted, mine, spent });
    if (units.length > 1 && P.tilePixels() >= 30) {
      const b = P.tileBox(x, y);
      label(ctx, `+${units.length - 1}`, b.x + b.w * 0.12, b.y + b.h * 0.88, Math.round(P.tilePixels() * 0.16), '#ffffff');
    }
  }

  // 5. selection / hover
  if (typeof ui.selected === 'number' && ui.selected >= 0) {
    const { x, y } = state.tileXY(ui.selected);
    if (P.inBounds(x, y)) drawSelectionRing(ctx, x, y);
  }
  if (ui.hover && P.inBounds(ui.hover.x, ui.hover.y)) {
    drawHoverRing(ctx, ui.hover.x, ui.hover.y);
  }

  // 6. debug ids
  if (ui.showIds && P.tilePixels() >= 30) {
    for (const { x, y } of order) drawTileIdLabel(ctx, x, y, state.tileIndex(x, y));
  }
}
