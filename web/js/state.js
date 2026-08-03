/*
 * state.js — StateView handling, wire protocol v2.
 *
 * A StateView from the engine is treated as immutable and total: every render is built from
 * the most recent response and nothing else. There is no local game state, no prediction, no
 * patching. If something looks stale the fix is to ask the engine again, never to mutate here.
 *
 * v1's `legal` array is GONE and nothing in this file replaces it. Selection is never gated
 * on an action set; `options` is fetched per selected tile and rendered whole.
 *
 * The only thing this module adds is indexing, plus the presentation vocabulary that turns
 * engine enums into English. Everything here is either an index or a label — no rules.
 */

/** Normalised label, so "Free Spirit", "free_spirit" and "freespirit" all match. */
export function key(s) {
  return String(s == null ? '' : s).trim().toLowerCase().replace(/[^a-z0-9]+/g, '');
}

/** Title Case from an engine enum name like "lumber_hut". */
export function pretty(s) {
  return String(s == null ? '' : s)
    .replace(/[_-]+/g, ' ')
    .replace(/\b[a-z]/g, (c) => c.toUpperCase());
}

/** Verbs that are map markers, not tray buttons (ui-reference.md §4.1). */
export const MARKER_VERBS = new Set(['move', 'attack']);

export class GameState {
  constructor(view) {
    /** The raw StateView exactly as received. Never mutated. */
    this.raw = view || {};
    const v = this.raw;

    this.width = (v.dims && v.dims.width) || 0;
    this.height = (v.dims && v.dims.height) || 0;
    this.turn = v.turn ?? 0;
    this.currentPlayer = v.current_player ?? 0;
    this.phase = v.phase || 'select';
    this.outcome = v.outcome || null;

    /**
     * The city holding an unresolved level-up, or -1. The engine REFUSES every other verb
     * while one is outstanding, so this is not a hint — it is the only thing the player may
     * answer, and the UI raises a true modal on it.
     */
    this.pendingRewardCity = typeof v.pending_reward_city === 'number' ? v.pending_reward_city : -1;

    /**
     * "Is there anything at all left to do" — units, cities, tiles AND research, answered by
     * the engine. This, and never "have my units moved", is what lights up End Turn.
     * Absent (an older engine) is read as "yes, something remains", which is the safe way to
     * be wrong: it never tells you you are finished when you are not.
     */
    this.hasActions = v.has_actions !== false;

    this.players = Array.isArray(v.players) ? v.players : [];
    this.tiles = Array.isArray(v.tiles) ? v.tiles : [];
    this.units = Array.isArray(v.units) ? v.units : [];
    this.cities = Array.isArray(v.cities) ? v.cities : [];

    this.tileByIndex = new Map();
    for (const t of this.tiles) this.tileByIndex.set(t.i, t);

    this.unitsByTile = new Map();
    for (const u of this.units) {
      const list = this.unitsByTile.get(u.tile);
      if (list) list.push(u);
      else this.unitsByTile.set(u.tile, [u]);
    }

    this.cityByTile = new Map();
    this.cityById = new Map();
    for (const c of this.cities) {
      this.cityByTile.set(c.tile, c);
      this.cityById.set(c.id, c);
    }

    this.playerById = new Map();
    for (const p of this.players) this.playerById.set(p.id, p);

    /** The human seat. There is exactly one in this build; the first wins if not. */
    const human = this.players.find((p) => p.human);
    this.me = human ? human.id : 0;
  }

  // --- indexing -------------------------------------------------------------

  tileIndex(x, y) { return y * this.width + x; }
  tileXY(i) { return { x: i % this.width, y: Math.floor(i / this.width) }; }

  /** Fog is applied server-side: an absent tile is an unexplored tile. */
  isExplored(i) { return this.tileByIndex.has(i); }

  tileAt(i) { return this.tileByIndex.get(i) || null; }
  unitsAt(i) { return this.unitsByTile.get(i) || []; }
  unitAt(i) { const l = this.unitsByTile.get(i); return l && l.length ? l[0] : null; }
  cityAt(i) { return this.cityByTile.get(i) || null; }

  // --- the viewing player ---------------------------------------------------

  myPlayer() { return this.playerById.get(this.me) || null; }
  isMyTurn() { return this.currentPlayer === this.me && !this.outcome; }
  isMine(owner) { return owner === this.me; }

  myUnits() { return this.units.filter((u) => u.owner === this.me); }
  enemyUnits() { return this.units.filter((u) => u.owner !== this.me); }

  /** A unit is spent when it cannot act again this turn. */
  static spent(u) { return !!u.turn_ended; }

  activeUnits() { return this.myUnits().filter((u) => !GameState.spent(u)); }

  /**
   * The city whose level-up must be answered before anything else can happen, or null.
   * Read from `pending_reward_city`, the engine's own answer — not re-derived from
   * `pending_rewards` counters, which say how many are queued but not whose turn it is.
   */
  pendingReward() {
    if (this.pendingRewardCity < 0 || !this.isMyTurn()) return null;
    const c = this.cityById.get(this.pendingRewardCity);
    return c && c.owner === this.me ? c : null;
  }
}
