/*
 * app.js — bootstrap, RPC client, and the selection state machine.
 *
 * Wire protocol v2. There is no `legal` array and nothing here replaces it:
 *
 *   1. EVERY discovered tile is clickable. Selection is never gated on an action set. This
 *      was the bug that made v1 "nearly unplayable".
 *   2. Clicking a tile asks the engine `layers`, then `options` for the first layer. Clicking
 *      the same tile again cycles to the next layer. A city tile has no `tile` layer and we
 *      do not invent one.
 *   3. The FULL option set is rendered, annotated. Nothing is ever silently dropped.
 *   4. Actions go out through `act` ({tile, verb, target}). Targets come from the engine.
 *   5. Every StateView replaces the world. There is no client-side game state and no
 *      prediction — the only things cached between frames are the engine's own answers to
 *      `options`, `reachable` and `preview_combat` for the current selection, and they are
 *      thrown away and re-fetched the moment anything changes.
 */

import { GameState, pretty } from './state.js';
import * as P from './projection.js';
import { render } from './render.js';
import { initInput } from './input.js';
import {
  initHud, updateHud, renderSelection, appendLog, showBanner, hideBanner,
  showCombatPreview, hideCombatPreview, openOption, closePopup, popupOpen,
  renderTechTree, closeTechTree, techTreeOpen,
  openLevelUp, closeLevelUp, levelUpOpen, levelUpCity,
  showResult, closeResult, resultOpen, parseRuinLine,
} from './hud.js';

// No turn_limit key: games end by domination, and Perfection sets its own limit engine-side.
// The opponent's tribe is "random" — the engine draws an unused one from a seeded generator
// and hides its identity until you meet them.
const DEFAULT_CONFIG = {
  width: 11,
  height: 11,
  seed: 12345,
  mode: 'domination',
  fog: true,
  players: [
    { tribe: 'imperius', human: true, difficulty: 'normal' },
    { tribe: 'random', human: false, difficulty: 'normal' },
  ],
};

/** How many enemy units we will probe with preview_combat on one selection. */
const MAX_PREVIEWS = 40;

/** How many of your units the per-unit readiness scan will probe on one StateView. */
const SCAN_BUDGET = 40;

/**
 * Engine log lines the UI has already said, in more detail, from the option it committed.
 * Everything else the engine reports is kept verbatim.
 */
const ENGINE_LINE_ALREADY_SAID = /^(Trained a unit|Researched a technology|Captured a village)$/;

class App {
  constructor() {
    this.canvas = document.getElementById('map');
    this.ctx = this.canvas.getContext('2d');
    this.state = null;
    /** The viewing seat. EVERY state-returning RPC carries it, see rpc(). */
    this.me = 0;
    this.sel = null;
    this.reachable = new Set();
    this.attackable = new Map();
    /** Adjacent enemy tiles a selected Mind Bender may convert. Gated on the engine's option. */
    this.convertible = new Set();
    this.ui = { hover: null, dpr: window.devicePixelRatio || 1, showIds: false };
    this.rpcId = 0;
    this.busy = false;
    this.lastConfig = DEFAULT_CONFIG;
  }

  // --- transport ------------------------------------------------------------

  /**
   * PROTOCOL NOTE, and it matters: the engine defaults the fog viewer to
   * `g_state.current_player`. The instant you end your turn that is the BOT, so a request
   * without an explicit `player` would hand you the bot's view of the map. Every call
   * therefore pins the viewer to our own seat.
   */
  async rpc(method, params = {}) {
    const id = ++this.rpcId;
    const body = { id, method, params: { player: this.me, ...params } };
    let res;
    try {
      res = await fetch('/rpc', {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify(body),
      });
    } catch (e) {
      throw new Error(`cannot reach the bridge server: ${e.message}`);
    }
    let parsed;
    try {
      parsed = await res.json();
    } catch (e) {
      throw new Error(`bridge returned a non-JSON response (HTTP ${res.status})`);
    }
    if (parsed && parsed.error) {
      const err = new Error(parsed.error);
      err.hint = parsed.hint;
      throw err;
    }
    if (!parsed || typeof parsed.result === 'undefined') {
      throw new Error('bridge returned a response with no result');
    }
    return parsed.result;
  }

  /**
   * Error boundary for one user-initiated operation. It is REENTRANT: an operation such as
   * `commit` legitimately calls `loadSelection` inside itself, and if the inner call were
   * silently dropped by a busy flag the UI would go stale after every action — which is
   * exactly the class of bug that made v1 feel broken. Nested calls just run; the outermost
   * frame owns error reporting.
   */
  async guarded(fn) {
    if (this.busy) return fn();
    this.busy = true;
    try {
      return await fn();
    } catch (e) {
      const msg = e.hint ? `${e.message}\n${e.hint}` : e.message;
      showBanner(msg, '');
      appendLog(msg, 'err');
      return null;
    } finally {
      this.busy = false;
    }
  }

  // --- state ----------------------------------------------------------------

  async setState(view, opts = {}) {
    const first = !this.state;
    this.state = new GameState(view);
    this.me = this.state.me;
    P.setDims(this.state.width, this.state.height);
    if (first || opts.refit) this.fit(false);
    this.resize();
    await this.syncReadiness();
    updateHud(this);
    this.draw();
    await this.syncLevelUp();
  }

  /**
   * FIX A — the level-up is a TRUE MODAL.
   *
   * `pending_reward_city` is not advice. The engine refuses every other verb while a reward
   * is outstanding, so if the UI leaves the choice buried in a city's option tray the board
   * simply stops responding and the game looks frozen. Every StateView is checked, so a
   * level-up caused by a bot's turn, by a harvest, or by a chain of two level-ups all raise
   * the same blocking dialog the moment they happen.
   */
  async syncLevelUp() {
    const s = this.state;
    const city = s ? s.pendingReward() : null;
    if (!city) { closeLevelUp(); return; }
    if (levelUpOpen() && levelUpCity() === city.id) return; // already asking about this one
    const r = await this.rpc('options', { tile: city.tile, layer: 'city', include_unavailable: true });
    const rewards = (Array.isArray(r.options) ? r.options : []).filter((o) => o.verb === 'choose_reward');
    if (!rewards.length) {
      // A dismissless dialog with no answer in it would be a dead end. Say so loudly instead:
      // this can only mean the engine and the UI disagree, and the game IS stuck either way.
      const msg = `City ${city.id} has a level-up pending but the engine offered no rewards.`;
      showBanner(msg, '');
      appendLog(msg, 'err');
      closeLevelUp();
      return;
    }
    // Everything else is closed under it: a half-open confirm popup behind a modal is a way
    // to commit an action the engine is currently refusing.
    closePopup();
    closeTechTree();
    openLevelUp(this, city, rewards);
  }

  /** Answer the level-up. This is the ONLY action reachable while the modal is up. */
  async chooseReward(city, o) {
    if (!this.state || !o || o.status !== 'available') return;
    await this.guarded(async () => {
      const view = await this.rpc('act', {
        tile: city.tile, verb: 'choose_reward', target: this.targetFor(o),
      });
      closeLevelUp();
      appendLog(`Level-up: chose ${o.label}`, 'you', this.state.turn);
      await this.setState(view);   // may immediately raise the NEXT pending level-up
      if (this.sel) await this.loadSelection(this.sel.tile, this.sel.li);
      await this.runBotsIfNeeded();
    });
  }

  /** True while a surface is up that the rest of the UI must not act behind at all. */
  modalOpen() { return levelUpOpen(); }

  /** True while the ruin/event result is up; dismissable, but only on purpose. */
  resultOpen() { return resultOpen(); }

  /**
   * Which of your units still have something they can do — asked of the ENGINE, one
   * `options` call per unit with include_unavailable=false, so it is exactly the set the
   * engine would accept. It drives the greyed-out unit glyphs and the Space hotkey ONLY;
   * whether the TURN is finished is `has_actions`, which also weighs cities, tiles and tech.
   *
   * `turn_ended` alone is the wrong signal for this too: a Warrior has Dash, so moving leaves
   * turn_ended false even when there is nothing left in range to attack.
   */
  async syncReadiness() {
    this.idleUnits = new Set();
    const s = this.state;
    if (!s || !s.isMyTurn()) return;
    const units = s.myUnits();
    for (let n = 0; n < units.length; n++) {
      const u = units[n];
      if (u.turn_ended) continue;
      // Past the scan budget, a unit is assumed ACTIVE rather than spent. Greying a unit we
      // never asked about would tell the player it is finished on no evidence at all.
      if (n >= SCAN_BUDGET) { this.idleUnits.add(u.tile); continue; }
      try {
        const r = await this.rpc('options', { tile: u.tile, layer: 'unit', include_unavailable: false });
        if (Array.isArray(r.options) && r.options.length) this.idleUnits.add(u.tile);
      } catch (_) { /* a unit that errors is treated as done, not as a crash */ }
    }
  }

  async runBotsIfNeeded() {
    let guard = 0;
    while (this.state && !this.state.outcome && guard++ < 32) {
      if (this.state.isMyTurn()) break;
      const out = await this.rpc('advance_bots', {});
      if (!out) break;
      for (const line of Array.isArray(out.log) ? out.log : []) {
        appendLog(line, 'bot', this.state ? this.state.turn : undefined);
      }
      if (!out.state) break;
      await this.setState(out.state);
    }
    if (this.state && this.state.isMyTurn() && this._announcedTurn !== this.state.turn) {
      this._announcedTurn = this.state.turn;
      appendLog(`Turn ${this.state.turn} — your move.`, 'sys');
    }
  }

  // --- selection ------------------------------------------------------------

  clearSelection() {
    this.sel = null;
    this.reachable = new Set();
    this.attackable = new Map();
    this.convertible = new Set();
    this.ui.selected = -1;
    hideCombatPreview();
    renderSelection(this);
    this.draw();
  }

  deselect() { this.clearSelection(); }

  /**
   * The one entry point for a map click. Order matters and is deliberately the real game's,
   * including the misclick it causes: with a unit selected, clicking a reachable tile MOVES
   * rather than selects (ui-reference.md §2.2, rebuild plan §5).
   */
  async onTileClick(i) {
    if (!this.state || i < 0) return;
    if (this.modalOpen() || popupOpen() || techTreeOpen() || resultOpen()) return;

    const sel = this.sel;
    if (sel && sel.layer === 'unit' && this.state.isMyTurn()) {
      // Convert is checked BEFORE attack: a Mind Bender's convert ring and an attack ring can
      // land on the same enemy, and converting is the reason you brought a Mind Bender.
      if (this.convertible.has(i)) { this.confirmConvert(i); return; }
      if (this.attackable.has(i)) { this.confirmAttack(i); return; }
      if (this.reachable.has(i)) { await this.doMove(i); return; }
    }

    const sameTile = sel && sel.tile === i;
    await this.loadSelection(i, sameTile ? sel.li + 1 : 0, sameTile ? sel.layers : null);
  }

  /** Switch layer from the tab strip. */
  async setLayer(idx) {
    if (!this.sel) return;
    await this.loadSelection(this.sel.tile, idx, this.sel.layers);
  }

  /**
   * Select tile `i` and load its options. NOTHING here can refuse the selection: an
   * unexplored cloud, an enemy city and an empty ocean tile all select and all say something.
   */
  async loadSelection(i, layerIndex = 0, knownLayers = null) {
    const s = this.state;
    if (!s) return;
    hideCombatPreview();
    this.reachable = new Set();
    this.attackable = new Map();
    this.convertible = new Set();
    this.ui.selected = i;

    if (!s.isExplored(i)) {
      this.sel = { tile: i, layers: [], li: 0, layer: null, options: [] };
      renderSelection(this);
      this.draw();
      return;
    }

    await this.guarded(async () => {
      const layers = knownLayers || (await this.rpc('layers', { tile: i })).layers || [];
      const li = layers.length ? ((layerIndex % layers.length) + layers.length) % layers.length : 0;
      const layer = layers[li] || null;

      let options = [];
      if (layer) {
        const r = await this.rpc('options', { tile: i, layer, include_unavailable: true });
        options = Array.isArray(r.options) ? r.options : [];
      }
      this.sel = { tile: i, layers, li, layer, options };

      // Movement and attack markers are ENGINE answers, never client geometry: zone of
      // control from a fog-hidden enemy truncates movement, and recomputing it here would
      // both draw markers the engine rejects and leak the hidden unit's position.
      const unit = s.unitAt(i);
      if (layer === 'unit' && unit && unit.owner === s.me && s.isMyTurn()) {
        // PROTOCOL NOTE: `reachable` answers "where could this unit step", not "may it step
        // now" — it returns tiles for a unit that is spent or was trained this turn, and the
        // move is then refused. The Move option's own status is the authority, so the rings
        // are gated on it. Both facts come from the engine; neither is recomputed here.
        const move = options.find((o) => o.verb === 'move');
        if (move && move.status === 'available') {
          const reach = await this.rpc('reachable', { tile: i });
          this.reachable = new Set(Array.isArray(reach.tiles) ? reach.tiles : []);
        }
        await this.loadAttackPreviews(i);
        this.loadConvertTargets(i, options);
      }
      renderSelection(this);
      this.draw();
    });
  }

  /**
   * Which enemies a Mind Bender may convert.
   *
   * Convert has no per-target engine query — there is no `preview_combat` equivalent for it —
   * so the ADJACENCY is worked out here from tiles the player can already see, using the same
   * 8-way offsets the engine uses. Nothing is inferred about legality: the ring set is empty
   * unless the engine's own Convert option is `available`, so the engine still decides whether
   * converting is possible at all and this only decides where to draw.
   *
   * That also means no leak. A fog-hidden enemy is absent from the StateView, so it cannot
   * produce a ring; unlike movement, adjacency has no zone-of-control subtlety that would make
   * a client-side answer differ from the engine's.
   */
  loadConvertTargets(from, options) {
    this.convertible = new Set();
    const s = this.state;
    const opt = (options || []).find((o) => o.verb === 'convert');
    if (!opt || opt.status !== 'available') return;
    const { x, y } = s.tileXY(from);
    for (const [dx, dy] of P.NEIGHBOURS) {
      const nx = x + dx;
      const ny = y + dy;
      if (!P.inBounds(nx, ny)) continue;
      const t = s.tileIndex(nx, ny);
      const u = s.unitAt(t);
      if (u && u.owner !== s.me) this.convertible.add(t);
    }
  }

  /** Converting is confirmed like an attack: it is irreversible and it spends the turn. */
  confirmConvert(to) {
    const victim = this.state.unitAt(to);
    if (!victim) return;
    openOption(this, {
      verb: 'convert',
      label: `Convert ${prettyType(victim.type)}`,
      status: 'available',
      cost: 0,
      target: to,
    });
    document.getElementById('pop-commit').textContent = 'CONVERT';
  }

  /** Ask the engine for a two-way damage readout against every visible enemy unit. */
  async loadAttackPreviews(from) {
    const s = this.state;
    this.attackable = new Map();
    const enemies = s.enemyUnits().slice(0, MAX_PREVIEWS);
    for (const e of enemies) {
      const p = await this.rpc('preview_combat', { from, to: e.tile });
      if (p && p.valid) this.attackable.set(e.tile, p);
    }
  }

  selectNextIdleUnit() {
    if (!this.state || this.modalOpen()) return;
    const ready = this.idleUnits;
    const idle = this.state.myUnits().filter((u) => (ready ? ready.has(u.tile) : !u.turn_ended));
    if (!idle.length) { showBanner('No units left to move — end your turn.', 'ok'); return; }
    const cur = this.sel ? this.sel.tile : -1;
    const at = idle.findIndex((u) => u.tile === cur);
    const next = idle[(at + 1) % idle.length];
    this.centreOn(next.tile);
    this.loadSelection(next.tile, 0);
  }

  centreOn(i) {
    if (!this.state) return;
    const { x, y } = this.state.tileXY(i);
    const pt = P.tileToScreen(x, y);
    P.panBy(window.innerWidth / 2 - pt.x, window.innerHeight / 2 - pt.y);
  }

  // --- acting ---------------------------------------------------------------

  /**
   * The target index for an option: the engine's own `target`, and nothing else.
   *
   * The Capture/Examine special case that used to live here is GONE — the engine now fills
   * Option::action for the self-targeting verbs too, so there is no verb the UI has to know
   * anything about. Move and attack still carry a meaningless 0, but neither reaches this
   * function: their targets come from `reachable` and `preview_combat`.
   */
  targetFor(o) {
    return typeof o.target === 'number' ? o.target : 0;
  }

  /**
   * Surface anything the engine reported about an action it just resolved. Ruin rewards are
   * rolled SERVER-SIDE, so this log line is the only place the answer exists — and it must be
   * shown, not filed in the log panel under the bot chatter.
   */
  reportActionLog(view) {
    const lines = view && Array.isArray(view.log) ? view.log : [];
    for (const line of lines) {
      // The engine's generic summaries duplicate the line the UI just wrote, which already
      // names the unit, the tech and the price. Its SPECIFIC lines — level-ups, ruin
      // rewards, damage — say things the UI cannot know and are always kept.
      if (ENGINE_LINE_ALREADY_SAID.test(String(line))) continue;
      appendLog(line, 'sys', this.state ? this.state.turn : undefined);
    }
    const ruin = lines.find((l) => /^Ruin:/i.test(String(l)));
    if (ruin) {
      const { title, body } = parseRuinLine(ruin);
      showResult('ANCIENT RUINS', title, body);
    }
    return lines;
  }

  /** Commit an option that the engine reported as `available`. */
  async commit(o) {
    if (!this.state || !o) return;
    if (o.status !== 'available') return; // the popup already explained why
    if (this.modalOpen()) return;
    const global = o.verb === 'research' || o.verb === 'end_turn';
    const tile = global ? -1 : (this.sel ? this.sel.tile : -1);
    const target = this.targetFor(o);
    const keepTile = this.sel ? this.sel.tile : -1;
    const keepLayer = this.sel ? this.sel.li : 0;
    // The unit that is about to act, so we can follow it if the engine MOVES it — a melee
    // unit advances onto the tile of what it killed, and re-selecting the tile it left would
    // show an empty square and quietly drop the selection.
    const actor = keepTile >= 0 ? this.state.unitAt(keepTile) : null;
    const actorId = actor && actor.owner === this.state.me ? actor.id : null;

    await this.guarded(async () => {
      const view = await this.rpc('act', { tile, verb: o.verb, target });
      await this.setState(view);
      // A negative cost is a REFUND (Disband). "(★-1)" would read as a price of minus one.
      const money = !o.cost ? '' : o.cost < 0 ? ` (+★${-o.cost})` : ` (★${o.cost})`;
      appendLog(`${labelForLog(o)}${money}`, 'you', this.state.turn);
      this.reportActionLog(view);
      if (techTreeOpen()) await this.refreshTechTree();
      if (!this.modalOpen() && keepTile >= 0) {
        await this.loadSelection(this.followTile(actorId, keepTile), keepLayer);
      }
      await this.runBotsIfNeeded();
    });
  }

  /**
   * Where the actor ended up. The engine advances a melee unit onto the tile it cleared, so
   * "the tile I clicked" is not where the unit is any more. Found by id from the new
   * StateView — never predicted, never computed from a direction.
   */
  followTile(actorId, fallback) {
    if (actorId == null || !this.state) return fallback;
    const u = this.state.units.find((x) => x.id === actorId);
    return u ? u.tile : fallback;
  }

  async doMove(to) {
    const from = this.sel.tile;
    await this.guarded(async () => {
      const view = await this.rpc('act', { tile: from, verb: 'move', target: to });
      await this.setState(view);
      appendLog(`Moved ${from} → ${to}`, 'you', this.state.turn);
      this.reportActionLog(view);
      // Follow the unit: it is almost always what you want to look at next.
      if (!this.modalOpen()) await this.loadSelection(to, 0);
    });
  }

  /**
   * Attacking is confirmed, never instant: there is no undo, and the two-way damage readout
   * is the single most useful thing the engine can tell you before you commit.
   */
  confirmAttack(to) {
    const preview = this.attackable.get(to);
    const defender = this.state.unitAt(to);
    if (!preview || !defender) return;

    openOption(this, {
      verb: 'attack',
      label: `Attack ${prettyType(defender.type)}`,
      status: 'available',
      cost: 0,
      target: to,
    });

    // Splice the numbers into the popup body: they are the whole reason to confirm.
    const body = document.getElementById('pop-body');
    const nums = document.createElement('div');
    nums.textContent =
      `You deal ${preview.attacker_damage} damage. ` +
      `Retaliation deals ${preview.defender_damage} damage to you.`;
    body.appendChild(nums);
    if (preview.defender_dies) {
      const d = document.createElement('div');
      d.textContent = `${prettyType(defender.type)} DIES from this attack.`;
      d.style.color = '#8affc0';
      d.style.fontWeight = '800';
      body.appendChild(d);
    }
    if (preview.attacker_dies) {
      const d = document.createElement('div');
      d.className = 'warn';
      d.textContent = 'YOUR UNIT DIES to the retaliation.';
      d.style.fontWeight = '800';
      body.appendChild(d);
    }
    document.getElementById('pop-commit').textContent = 'ATTACK';
  }

  async endTurn() {
    if (!this.state || !this.state.isMyTurn()) return;
    // The engine refuses end_turn while a level-up is outstanding; the modal is already up,
    // so there is nothing to explain — just refuse to act behind it.
    if (this.modalOpen()) return;
    // Enter is both "end turn" and "acknowledge"; acknowledging first is never destructive.
    if (resultOpen()) { closeResult(); return; }
    await this.guarded(async () => {
      const view = await this.rpc('act', { tile: -1, verb: 'end_turn', target: 0 });
      appendLog('Ended turn.', 'you', this.state.turn);
      await this.setState(view);
      this.clearSelection();
      this.reportActionLog(view);
      await this.runBotsIfNeeded();
    });
  }

  // --- tech tree ------------------------------------------------------------

  async openTechTree(highlight) {
    if (this.modalOpen()) return;
    await this.guarded(async () => {
      const r = await this.rpc('research_options', { include_unavailable: true });
      renderTechTree(this, Array.isArray(r.options) ? r.options : [], highlight);
    });
  }

  async refreshTechTree() {
    const r = await this.rpc('research_options', { include_unavailable: true });
    renderTechTree(this, Array.isArray(r.options) ? r.options : [], null);
  }

  toggleTechTree() {
    if (this.modalOpen()) return;
    if (techTreeOpen()) closeTechTree();
    else this.openTechTree();
  }

  popupOpen() { return popupOpen(); }

  onEscape() {
    // Esc does NOT dismiss the level-up. It is the one thing in this UI with no way out but
    // an answer, because that is what the engine enforces.
    if (this.modalOpen()) return;
    if (resultOpen()) { closeResult(); return; }
    if (popupOpen()) { closePopup(); return; }
    if (techTreeOpen()) { closeTechTree(); return; }
    this.deselect();
  }

  // --- hover ----------------------------------------------------------------

  onHover(t) {
    if (!t || !this.state) {
      if (this.ui.hover) { this.ui.hover = null; this.draw(); }
      hideCombatPreview();
      return;
    }
    const changed = !this.ui.hover || this.ui.hover.x !== t.x || this.ui.hover.y !== t.y;
    this.ui.hover = { x: t.x, y: t.y };
    if (changed) this.draw();

    const i = this.state.tileIndex(t.x, t.y);
    const preview = this.attackable.get(i);
    if (preview && this.sel) {
      const defender = this.state.unitAt(i);
      const attacker = this.state.unitAt(this.sel.tile);
      showCombatPreview(
        preview,
        t.screen,
        attacker ? prettyType(attacker.type) : 'Your unit',
        defender ? prettyType(defender.type) : 'Enemy',
        defender ? defender.hp - preview.attacker_damage : null
      );
    } else {
      hideCombatPreview();
    }
  }

  // --- view -----------------------------------------------------------------

  fit(redraw = true) {
    this.resize();
    P.fitToViewport(90);
    if (redraw) { this.draw(); this.repositionFloaters(); }
  }

  resize() {
    const w = window.innerWidth;
    const h = window.innerHeight;
    const dpr = window.devicePixelRatio || 1;
    this.ui.dpr = dpr;
    if (this.canvas.width !== Math.round(w * dpr) || this.canvas.height !== Math.round(h * dpr)) {
      this.canvas.width = Math.round(w * dpr);
      this.canvas.height = Math.round(h * dpr);
    }
    P.setViewport(w, h);
  }

  repositionFloaters() { /* nothing floats in world space any more */ }

  draw() {
    if (this._raf) return;
    this._raf = requestAnimationFrame(() => {
      this._raf = null;
      this.ui.reachable = this.reachable;
      this.ui.attackable = this.attackable;
      this.ui.convertible = this.convertible;
      this.ui.idle = this.idleUnits;
      this.ui.selected = this.sel ? this.sel.tile : -1;
      render(this.ctx, this.state, this.ui);
      this.repositionFloaters();
    });
  }

  // --- lifecycle ------------------------------------------------------------

  async newGame(config) {
    this.lastConfig = config || this.lastConfig;
    this.me = 0;
    await this.guarded(async () => {
      // Nothing from the previous game may survive into the next one.
      closeLevelUp();
      closeResult();
      closePopup();
      closeTechTree();
      const view = await this.rpc('new_game', this.lastConfig);
      document.getElementById('log').textContent = '';
      this.state = null;
      this.idleUnits = new Set();
      this.clearSelection();
      await this.setState(view, { refit: true });
      hideBanner();
      const foes = Math.max(0, (this.lastConfig.players || []).length - 1);
      appendLog(
        `New game — ${this.lastConfig.width}x${this.lastConfig.height}, seed ${this.lastConfig.seed}, ` +
        `${this.lastConfig.mode}, fog ${this.lastConfig.fog ? 'on' : 'off'}, ` +
        `${foes} opponent${foes === 1 ? '' : 's'} of unknown tribe`,
        'sys'
      );
      appendLog('Click any discovered tile to inspect it. Space selects your next idle unit.', 'sys');
      await this.runBotsIfNeeded();
    });
  }
}

function prettyType(t) {
  return String(t || '?').replace(/[_-]+/g, ' ').replace(/\b[a-z]/g, (c) => c.toUpperCase());
}

function labelForLog(o) {
  switch (o.verb) {
    case 'train': return `Trained ${o.label}`;
    case 'build': return `Built ${o.label}`;
    case 'harvest': return 'Harvested resource';
    case 'research': return `Researched ${o.label}`;
    case 'capture': return 'Captured city';
    case 'examine': return 'Examined ruins';
    case 'choose_reward': return `Chose ${o.label}`;
    case 'attack': return o.label;
    case 'convert': return o.label;
    case 'recover': return 'Recovered';
    case 'promote': return 'Promoted to Veteran';
    case 'heal': return 'Healed adjacent units';
    case 'destroy': return 'Destroyed building';
    case 'disband': return 'Disbanded unit';
    case 'upgrade_naval': return `Upgraded to ${o.label}`;
    default: return `${pretty(o.verb)} ${o.label}`;
  }
}

async function main() {
  const app = new App();
  window.polyApp = app; // handy for console poking during development
  initHud(app);
  initInput(app, app.canvas);

  window.addEventListener('resize', () => {
    app.resize();
    app.draw();
    updateHud(app);
  });

  await app.newGame(DEFAULT_CONFIG);
}

main();
