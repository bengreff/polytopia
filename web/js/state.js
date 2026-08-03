/*
 * state.js — StateView handling.
 *
 * A StateView from the engine is treated as immutable and total: every render
 * is built from the most recent response and nothing else. There is no local
 * game state, no prediction, no patching. If something looks stale, the fix is
 * to ask the engine again, never to mutate here.
 *
 * The only thing this module adds is indexing (maps keyed by tile index / id)
 * so the renderer does not do linear scans per tile.
 */

/** Normalised label, for the few places the protocol only identifies an action by its text. */
export function labelKey(s) {
  return String(s || '').trim().toLowerCase();
}

/**
 * The tile a legal action refers to.
 *
 * Per docs/wire-protocol.md, entries of kind "tile" carry the tile index as
 * their action index (the SELECT example uses index 47 for tile 47). We also
 * honour an explicit `tile` field if a future engine supplies one, so the
 * action index can diverge from the tile index without a UI change.
 * Returns null for non-tile actions.
 */
export function actionTile(entry) {
  if (!entry || entry.kind !== 'tile') return null;
  if (typeof entry.tile === 'number') return entry.tile;
  return typeof entry.index === 'number' ? entry.index : null;
}

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
    this.selected = typeof v.selected === 'number' ? v.selected : -1;
    this.pendingVerb = v.pending_verb || null;
    this.outcome = v.outcome || null;

    this.players = Array.isArray(v.players) ? v.players : [];
    this.tiles = Array.isArray(v.tiles) ? v.tiles : [];
    this.units = Array.isArray(v.units) ? v.units : [];
    this.cities = Array.isArray(v.cities) ? v.cities : [];
    this.legal = Array.isArray(v.legal) ? v.legal : [];

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

    // Legal actions, indexed for O(1) lookup.
    this.legalTileIndices = new Map(); // tile index -> legal entry
    this.legalVerbs = []; // everything that is not a map tile
    for (const a of this.legal) {
      const t = actionTile(a);
      if (t !== null) {
        if (!this.legalTileIndices.has(t)) this.legalTileIndices.set(t, a);
      } else {
        this.legalVerbs.push(a);
      }
    }
  }

  tileIndex(x, y) {
    return y * this.width + x;
  }

  tileXY(i) {
    return { x: i % this.width, y: Math.floor(i / this.width) };
  }

  isExplored(i) {
    // Fog is applied server-side: an absent tile is an unexplored tile.
    return this.tileByIndex.has(i);
  }

  tileAt(i) {
    return this.tileByIndex.get(i) || null;
  }

  unitsAt(i) {
    return this.unitsByTile.get(i) || [];
  }

  unitAt(i) {
    const list = this.unitsByTile.get(i);
    return list && list.length ? list[0] : null;
  }

  cityAt(i) {
    return this.cityByTile.get(i) || null;
  }

  /**
   * The legal action for clicking tile index `i`, or null.
   * NOTE: we return the entry so the caller sends entry.index — the value that
   * came from the engine. The UI never fabricates an index.
   */
  legalForTile(i) {
    return this.legalTileIndices.get(i) || null;
  }

  /** Non-tile legal actions: verbs, train options, anything else the engine offers. */
  verbActions() {
    return this.legalVerbs;
  }

  /**
   * Find a legal action by its label. The protocol identifies Deselect and End
   * Turn only by their label text, so this is the sanctioned lookup for those
   * two; it is never used to construct an index, only to find one.
   */
  findByLabel(label) {
    const want = labelKey(label);
    return this.legal.find((a) => labelKey(a.label) === want) || null;
  }

  deselectAction() {
    return this.findByLabel('deselect');
  }

  endTurnAction() {
    return this.findByLabel('end turn') || this.findByLabel('endturn');
  }

  selectedUnit() {
    return this.selected >= 0 ? this.unitAt(this.selected) : null;
  }

  selectedCity() {
    return this.selected >= 0 ? this.cityAt(this.selected) : null;
  }

  currentPlayerObj() {
    return this.playerById.get(this.currentPlayer) || null;
  }
}
