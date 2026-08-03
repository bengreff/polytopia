/*
 * app.js — bootstrap and RPC client.
 *
 * Flow: send a request to POST /rpc, get a StateView back, throw the old state
 * away, rebuild everything from the new one. There is no local game model and
 * no prediction anywhere in this UI; the engine is the only source of truth.
 */

import { GameState } from './state.js';
import * as P from './projection.js';
import { render } from './render.js';
import { initInput } from './input.js';
import { initHud, updateHud, appendLog, showBanner, hideBanner, setEngineBadge } from './hud.js';

const DEFAULT_CONFIG = {
  width: 11,
  height: 11,
  seed: 12345,
  mode: 'domination',
  turn_limit: 0,
  fog: true,
  players: [
    { tribe: 'imperius', human: true, difficulty: 'normal' },
    { tribe: 'bardur', human: false, difficulty: 'normal' },
  ],
};

class App {
  constructor() {
    this.canvas = document.getElementById('map');
    this.ctx = this.canvas.getContext('2d');
    this.state = null;
    this.ui = { hover: null, showIds: false, dpr: window.devicePixelRatio || 1 };
    this.rpcId = 0;
    this.busy = false;
    this.lastConfig = DEFAULT_CONFIG;
  }

  // --- transport ----------------------------------------------------------

  async rpc(method, params = {}) {
    const id = ++this.rpcId;
    let res;
    try {
      res = await fetch('/rpc', {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify({ id, method, params }),
      });
    } catch (e) {
      throw new Error(`cannot reach the bridge server: ${e.message}`);
    }
    let body;
    try {
      body = await res.json();
    } catch (e) {
      throw new Error(`bridge returned a non-JSON response (HTTP ${res.status})`);
    }
    if (body && body.error) {
      const err = new Error(body.error);
      err.hint = body.hint;
      err.engineMissing = !!body.engine_missing;
      throw err;
    }
    if (!body || typeof body.result === 'undefined') {
      throw new Error('bridge returned a response with no result');
    }
    return body.result;
  }

  async guarded(fn) {
    if (this.busy) return null;
    this.busy = true;
    try {
      const out = await fn();
      hideBanner();
      return out;
    } catch (e) {
      const msg = e.hint ? `${e.message}\n${e.hint}` : e.message;
      showBanner(msg, 'error');
      setEngineBadge(e.engineMissing ? 'NO ENGINE' : 'RPC ERROR', 'error');
      appendLog(msg, 'err');
      return null;
    } finally {
      this.busy = false;
    }
  }

  // --- state --------------------------------------------------------------

  setState(view, opts = {}) {
    const first = !this.state;
    this.state = new GameState(view);
    P.setDims(this.state.width, this.state.height);
    if (first || opts.refit) this.fit(false);
    this.resize();
    updateHud(this);
    this.draw();
  }

  /**
   * Bots run automatically whenever the decision belongs to a non-human player.
   * The log lines come straight from advance_bots.
   */
  async runBotsIfNeeded() {
    let guard = 0;
    while (this.state && !this.state.outcome && guard++ < 32) {
      const cur = this.state.currentPlayerObj();
      if (!cur || cur.human) break;
      const out = await this.rpc('advance_bots', {});
      if (!out) break;
      const lines = Array.isArray(out.log) ? out.log : [];
      for (const line of lines) appendLog(line, 'bot', this.state ? this.state.turn : undefined);
      if (out.state) this.setState(out.state);
      else break;
    }
  }

  // --- commands -----------------------------------------------------------

  async newGame(config) {
    this.lastConfig = config || this.lastConfig;
    await this.guarded(async () => {
      const view = await this.rpc('new_game', this.lastConfig);
      document.getElementById('log').textContent = '';
      this.setState(view, { refit: true });
      appendLog(
        `New game — ${this.lastConfig.width}x${this.lastConfig.height}, seed ${this.lastConfig.seed}, ` +
          `${this.lastConfig.mode}, fog ${this.lastConfig.fog ? 'on' : 'off'}`,
        'sys'
      );
      await this.runBotsIfNeeded();
    });
  }

  /**
   * Apply a legal action. `entry` MUST be an object that came out of the
   * engine's `legal` array; we send entry.index verbatim and re-render from the
   * StateView we get back.
   */
  async applyAction(entry) {
    if (!entry || typeof entry.index !== 'number') return;
    if (!this.state || !this.state.legal.includes(entry)) {
      // Defensive: a stale button from a previous StateView. Ignore it rather
      // than gamble that the index is still meaningful.
      appendLog(`ignored a stale action (#${entry.index})`, 'sys');
      return;
    }
    await this.guarded(async () => {
      const view = await this.rpc('apply', { index: entry.index });
      this.setState(view);
      await this.runBotsIfNeeded();
    });
  }

  async endTurn() {
    if (!this.state) return;
    const entry = this.state.endTurnAction();
    if (!entry) {
      appendLog('End Turn is not legal right now.', 'sys');
      return;
    }
    await this.applyAction(entry);
  }

  async deselect() {
    if (!this.state) return;
    const entry = this.state.deselectAction();
    if (!entry) return; // Deselect is not legal in the select phase; that is fine.
    await this.applyAction(entry);
  }

  async reloadRules() {
    await this.guarded(async () => {
      const out = await this.rpc('reload_rules', {});
      appendLog(`Rules reloaded: ${out && out.version ? out.version : 'ok'}`, 'sys');
      showBanner(`Rules reloaded (${out && out.version ? out.version : 'ok'})`, 'ok');
      setTimeout(hideBanner, 2500);
      const view = await this.rpc('state', { player: 0 });
      this.setState(view);
    });
  }

  flashIllegal(tileIndex) {
    // Deliberately quiet: an illegal click is a non-event, not an error.
    const hint = document.getElementById('hint');
    hint.textContent = `Tile ${tileIndex} is not a legal choice right now`;
    hint.hidden = false;
    clearTimeout(this._illegalTimer);
    this._illegalTimer = setTimeout(() => updateHud(this), 900);
  }

  // --- view ---------------------------------------------------------------

  fit(redraw = true) {
    this.resize();
    P.fitToViewport(28);
    if (redraw) this.draw();
  }

  resize() {
    const stage = document.getElementById('stage');
    const w = stage.clientWidth;
    const h = stage.clientHeight;
    const dpr = window.devicePixelRatio || 1;
    this.ui.dpr = dpr;
    if (this.canvas.width !== Math.round(w * dpr) || this.canvas.height !== Math.round(h * dpr)) {
      this.canvas.width = Math.round(w * dpr);
      this.canvas.height = Math.round(h * dpr);
    }
    P.setViewport(w, h);
  }

  draw() {
    if (this._raf) return;
    this._raf = requestAnimationFrame(() => {
      this._raf = null;
      render(this.ctx, this.state, this.ui);
      updateHudPosition(this);
    });
  }
}

// The floating verb menu is anchored to a tile, so it must follow pan/zoom.
function updateHudPosition(app) {
  if (app.state && app.state.phase === 'verb') updateHud(app);
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

  try {
    const health = await (await fetch('/health')).json();
    if (health.mock) setEngineBadge('MOCK ENGINE', '');
    else if (health.cli_exists) setEngineBadge('ENGINE', 'live');
    else setEngineBadge('NO ENGINE', 'error');
  } catch (e) {
    setEngineBadge('NO BRIDGE', 'error');
  }

  await app.newGame(DEFAULT_CONFIG);
}

main();
