#!/usr/bin/env node
/*
 * web/server.js — static file server + JSON-RPC bridge to `poly-cli --rpc`.
 *
 * ZERO npm dependencies. Built-ins only: http, fs, path, child_process, url.
 *
 *   node web/server.js                 # bridge to ./build/cli/poly-cli --rpc
 *   node web/server.js --mock          # no child process; serve a fixture engine
 *   node web/server.js --port 9000 --cli /path/to/poly-cli
 *
 * Env: PORT, POLY_CLI, POLY_HOST.
 *
 * This whole file is throwaway: it is deleted when the WASM target lands and the
 * renderer talks to the module directly. See docs/phase0-plan.md §6.
 */

'use strict';

const http = require('http');
const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

const WEB_ROOT = __dirname;
const REPO_ROOT = path.resolve(__dirname, '..');

function parseArgs(argv) {
  const opt = {
    mock: false,
    port: Number(process.env.PORT || 8080),
    host: process.env.POLY_HOST || '127.0.0.1',
    cli: process.env.POLY_CLI || path.join(REPO_ROOT, 'build', 'cli', 'poly-cli'),
  };
  for (let i = 2; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--mock') opt.mock = true;
    else if (a === '--port' || a === '-p') opt.port = Number(argv[++i]);
    else if (a.startsWith('--port=')) opt.port = Number(a.slice(7));
    else if (a === '--host') opt.host = argv[++i];
    else if (a.startsWith('--host=')) opt.host = a.slice(7);
    else if (a === '--cli') opt.cli = argv[++i];
    else if (a.startsWith('--cli=')) opt.cli = a.slice(6);
    else if (a === '--help' || a === '-h') {
      process.stdout.write(
        'usage: node web/server.js [--mock] [--port N] [--host H] [--cli PATH]\n'
      );
      process.exit(0);
    } else {
      process.stderr.write(`[server] ignoring unknown argument: ${a}\n`);
    }
  }
  if (!Number.isFinite(opt.port)) opt.port = 8080;
  opt.cli = path.resolve(REPO_ROOT, opt.cli);
  return opt;
}

const OPT = parseArgs(process.argv);

// ---------------------------------------------------------------------------
// Child process bridge
// ---------------------------------------------------------------------------
//
// One long-lived child. Requests are written one-JSON-per-line to stdin and
// answered one-JSON-per-line on stdout, strictly FIFO.

const RPC_TIMEOUT_MS = 30000;

const child = {
  proc: null,
  buf: '',
  pending: [], // {resolve, reject, timer}
  stderr: [],
  lastError: null,
};

function noteStderr(chunk) {
  const text = String(chunk);
  child.stderr.push(text);
  if (child.stderr.length > 50) child.stderr.shift();
  process.stderr.write(`[poly-cli] ${text.replace(/\n$/, '')}\n`);
}

function failAllPending(message) {
  const pend = child.pending.splice(0, child.pending.length);
  for (const p of pend) {
    clearTimeout(p.timer);
    p.reject(new Error(message));
  }
}

function killChild(reason) {
  if (child.proc) {
    try {
      child.proc.kill();
    } catch (_) {
      /* already gone */
    }
    child.proc = null;
  }
  child.buf = '';
  failAllPending(reason);
}

function ensureChild() {
  if (child.proc) return child.proc;

  if (!fs.existsSync(OPT.cli)) {
    const err = new Error(
      `engine binary not found at ${OPT.cli} — build it (cmake --build build) ` +
        `or run the server with --mock, or point --cli at the right path`
    );
    err.code = 'ENGINE_MISSING';
    throw err;
  }

  let proc;
  try {
    proc = spawn(OPT.cli, ['--rpc'], { stdio: ['pipe', 'pipe', 'pipe'] });
  } catch (e) {
    const err = new Error(`failed to spawn ${OPT.cli} --rpc: ${e.message}`);
    err.code = 'ENGINE_SPAWN_FAILED';
    throw err;
  }

  child.proc = proc;
  child.buf = '';
  child.lastError = null;

  proc.stdout.setEncoding('utf8');
  proc.stdout.on('data', (chunk) => {
    child.buf += chunk;
    let nl;
    while ((nl = child.buf.indexOf('\n')) >= 0) {
      const line = child.buf.slice(0, nl).trim();
      child.buf = child.buf.slice(nl + 1);
      if (!line) continue;
      const p = child.pending.shift();
      if (!p) {
        process.stderr.write(`[server] unsolicited line from engine: ${line}\n`);
        continue;
      }
      clearTimeout(p.timer);
      p.resolve(line);
    }
    if (child.buf.length > 64 * 1024 * 1024) {
      killChild('engine produced an unterminated line larger than 64MB');
    }
  });

  proc.stderr.setEncoding('utf8');
  proc.stderr.on('data', noteStderr);

  proc.on('error', (e) => {
    child.lastError = e.message;
    killChild(`engine process error: ${e.message}`);
  });

  proc.on('exit', (code, signal) => {
    const tail = child.stderr.join('').trim().split('\n').slice(-5).join(' | ');
    const why =
      `engine exited (code=${code}, signal=${signal})` + (tail ? `: ${tail}` : '');
    child.lastError = why;
    // Drop the handle so the next request tries a fresh spawn.
    child.proc = null;
    child.buf = '';
    failAllPending(why);
  });

  proc.stdin.on('error', (e) => {
    child.lastError = `engine stdin: ${e.message}`;
  });

  return proc;
}

function callChild(requestLine) {
  return new Promise((resolve, reject) => {
    let proc;
    try {
      proc = ensureChild();
    } catch (e) {
      reject(e);
      return;
    }
    const timer = setTimeout(() => {
      const idx = child.pending.findIndex((p) => p.timer === timer);
      if (idx >= 0) child.pending.splice(idx, 1);
      reject(new Error(`engine did not answer within ${RPC_TIMEOUT_MS}ms`));
    }, RPC_TIMEOUT_MS);
    child.pending.push({ resolve, reject, timer });
    try {
      proc.stdin.write(requestLine + '\n');
    } catch (e) {
      clearTimeout(timer);
      const idx = child.pending.findIndex((p) => p.timer === timer);
      if (idx >= 0) child.pending.splice(idx, 1);
      reject(new Error(`could not write to engine stdin: ${e.message}`));
    }
  });
}

// ---------------------------------------------------------------------------
// Mock engine
// ---------------------------------------------------------------------------
//
// A deliberately small stand-in that implements the wire protocol well enough
// to develop and test the whole UI (SELECT -> VERB -> TARGET, fog, negative
// population, fractional HP, bot logs) before the C++ engine exists. It is NOT
// a rules implementation and makes no claim to be one.

const MOCK = (() => {
  const TRIBES = [
    'imperius', 'bardur', 'xin-xi', 'oumaji', 'kickoo', 'hoodrick',
    'luxidoor', 'vengir', 'zebasi', 'ai-mo', 'quetzali', 'yadakk',
  ];

  // Opaque action-index bands. The UI must never reconstruct these; it only
  // ever echoes back an index it received in `legal`.
  const A_ENDTURN = 900;
  const A_DESELECT = 901;
  const A_VERB = 1000; // 1000 + k
  const A_TRAIN = 3000; // 3000 + k
  // Target-phase actions of kind "tile" use the TILE INDEX itself as the action
  // index, matching the SELECT-phase example in docs/wire-protocol.md. That is
  // what lets the UI highlight a target on the map without doing arithmetic.

  const UNIT_KINDS = {
    warrior: { cost: 2, hp: 10, atk: 2, def: 2, move: 1 },
    rider: { cost: 3, hp: 10, atk: 2, def: 1, move: 2 },
    archer: { cost: 3, hp: 10, atk: 2, def: 1, move: 1 },
    defender: { cost: 3, hp: 15, atk: 1, def: 3, move: 1 },
  };

  let S = null;
  let nextUnitId = 1;
  let nextCityId = 1;

  function rngFactory(seed) {
    let s = (seed >>> 0) || 1;
    return function rand() {
      s ^= s << 13; s >>>= 0;
      s ^= s >> 17;
      s ^= s << 5; s >>>= 0;
      return s / 4294967296;
    };
  }

  function idx(x, y) { return y * S.width + x; }
  function xy(i) { return { x: i % S.width, y: Math.floor(i / S.width) }; }
  function inBounds(x, y) { return x >= 0 && y >= 0 && x < S.width && y < S.height; }
  function tileAt(i) { return S.tiles[i]; }
  function unitAt(i) { return S.units.find((u) => u.tile === i && u.alive); }
  function cityAt(i) { return S.cities.find((c) => c.tile === i); }
  function passable(i) {
    const t = tileAt(i);
    return t && t.terrain !== 'ocean' && t.terrain !== 'water' && t.terrain !== 'mountain';
  }
  function neighbours(i, r) {
    const { x, y } = xy(i);
    const out = [];
    const rad = r || 1;
    for (let dy = -rad; dy <= rad; dy++) {
      for (let dx = -rad; dx <= rad; dx++) {
        if (!dx && !dy) continue;
        if (inBounds(x + dx, y + dy)) out.push(idx(x + dx, y + dy));
      }
    }
    return out;
  }

  function newGame(params) {
    const p = params || {};
    const width = Math.max(4, Math.min(40, p.width || 11));
    const height = Math.max(4, Math.min(40, p.height || 11));
    const seed = p.seed === undefined ? 12345 : (p.seed | 0);
    const rand = rngFactory(seed || 1);
    const playerCfg =
      Array.isArray(p.players) && p.players.length
        ? p.players
        : [
            { tribe: 'imperius', human: true, difficulty: 'normal' },
            { tribe: 'bardur', human: false, difficulty: 'normal' },
          ];

    S = {
      width, height, seed,
      mode: p.mode || 'domination',
      turn_limit: p.turn_limit || 0,
      fog: p.fog === undefined ? true : !!p.fog,
      turn: 1,
      current_player: 0,
      phase: 'select',
      selected: -1,
      pending_verb: null,
      outcome: null,
      players: [],
      tiles: [],
      units: [],
      cities: [],
      explored: [],
      log: [],
    };
    nextUnitId = 1;
    nextCityId = 1;

    for (let i = 0; i < width * height; i++) {
      const r = rand();
      let terrain = 'field';
      if (r < 0.16) terrain = 'ocean';
      else if (r < 0.3) terrain = 'water';
      else if (r < 0.52) terrain = 'forest';
      else if (r < 0.62) terrain = 'mountain';
      let resource = 'none';
      const rr = rand();
      if (terrain === 'field' && rr < 0.2) resource = 'fruit';
      else if (terrain === 'field' && rr < 0.32) resource = 'crop';
      else if (terrain === 'forest' && rr < 0.22) resource = 'game';
      else if (terrain === 'mountain' && rr < 0.3) resource = 'metal';
      else if (terrain === 'water' && rr < 0.25) resource = 'fish';
      else if (terrain === 'ocean' && rr < 0.1) resource = 'whale';
      else if (rr > 0.96) resource = 'ruin';
      S.tiles.push({
        i, terrain, resource, improvement: 'none',
        owner: -1, city: -1, road: false,
      });
    }

    S.players = playerCfg.slice(0, 4).map((c, id) => ({
      id,
      tribe: TRIBES.includes(c.tribe) ? c.tribe : TRIBES[id % TRIBES.length],
      human: !!c.human,
      difficulty: c.difficulty || 'normal',
      alive: true,
      stars: 5 + id,
      score: 1000 + id * 120,
      income: 2 + id,
      techs: id === 0 ? ['organization', 'riding'] : ['hunting'],
    }));

    // Deterministic-ish starting spots: near opposite corners.
    const spots = [
      idx(2, 2),
      idx(width - 3, height - 3),
      idx(width - 3, 2),
      idx(2, height - 3),
    ];

    S.players.forEach((pl, id) => {
      const home = spots[id % spots.length];
      // Guarantee the capital sits on buildable land.
      for (const t of [home].concat(neighbours(home))) {
        const tile = tileAt(t);
        if (tile) { tile.terrain = 'field'; tile.resource = 'none'; }
      }
      const cid = nextCityId++;
      const cityTile = tileAt(home);
      cityTile.city = cid;
      cityTile.owner = id;
      cityTile.improvement = 'city';
      for (const n of neighbours(home)) {
        tileAt(n).owner = id;
        tileAt(n).city = cid;
      }
      S.cities.push({
        id: cid, owner: id, tile: home, level: id === 0 ? 2 : 1,
        // Player 0's capital deliberately starts with a NEGATIVE population so
        // the "never clamp, render red" requirement is exercised on load.
        population: id === 0 ? -2 : 1,
        next_level_at: id === 0 ? 4 : 2,
        capital: true, walled: id === 1, besieged: false,
        units: 1, capacity: 3, pending_rewards: id === 0 ? 1 : 0,
        production: id === 0 ? 3 : 2,
        name: `${S.players[id].tribe} capital`,
      });
      spawnUnit(id === 0 ? 'warrior' : 'defender', id, home);
      const second = neighbours(home)[1];
      if (second !== undefined && passable(second)) {
        const u = spawnUnit(id === 0 ? 'rider' : 'warrior', id, second);
        if (id === 0) u.hp = 5.5; // fractional HP must render
      }
    });

    recomputeExplored();
    return view(0);
  }

  function spawnUnit(type, owner, tile) {
    const k = UNIT_KINDS[type] || UNIT_KINDS.warrior;
    const u = {
      id: nextUnitId++, type, owner, tile,
      hp: k.hp, max_hp: k.hp, moves_left: k.move,
      veteran: false, turn_ended: false, carried: null, alive: true,
    };
    S.units.push(u);
    return u;
  }

  function recomputeExplored() {
    S.explored = S.players.map((pl, id) => {
      const seen = new Set();
      for (const t of S.tiles) if (t.owner === id) seen.add(t.i);
      for (const u of S.units) {
        if (u.owner !== id || !u.alive) continue;
        seen.add(u.tile);
        for (const n of neighbours(u.tile, 2)) seen.add(n);
      }
      for (const c of S.cities) {
        if (c.owner !== id) continue;
        seen.add(c.tile);
        for (const n of neighbours(c.tile, 2)) seen.add(n);
      }
      return seen;
    });
  }

  function currentSelectionUnit() {
    return S.selected >= 0 ? unitAt(S.selected) : null;
  }
  function currentSelectionCity() {
    return S.selected >= 0 ? cityAt(S.selected) : null;
  }

  function verbList() {
    // [verbKey, label] in a stable order; index is A_VERB + position.
    return ['move', 'attack', 'recover', 'disband'];
  }

  function moveTargets(u) {
    return neighbours(u.tile, u.moves_left > 1 ? 2 : 1).filter(
      (n) => passable(n) && !unitAt(n)
    );
  }
  function attackTargets(u) {
    return neighbours(u.tile, u.type === 'archer' ? 2 : 1).filter((n) => {
      const e = unitAt(n);
      return e && e.owner !== u.owner;
    });
  }

  function legal() {
    const out = [];
    const me = S.current_player;
    if (S.outcome) return out;

    if (S.phase === 'select') {
      const seen = new Set();
      for (const u of S.units) {
        if (u.owner !== me || !u.alive || u.turn_ended) continue;
        const c = cityAt(u.tile);
        out.push({
          index: u.tile, kind: 'tile',
          label: c ? `${title(u.type)} / City` : title(u.type),
        });
        seen.add(u.tile);
      }
      for (const c of S.cities) {
        if (c.owner !== me || seen.has(c.tile)) continue;
        out.push({ index: c.tile, kind: 'tile', label: `City lvl ${c.level}` });
        seen.add(c.tile);
      }
      out.push({ index: A_ENDTURN, kind: 'verb', label: 'End Turn' });
      return out;
    }

    if (S.phase === 'verb') {
      const u = currentSelectionUnit();
      const c = currentSelectionCity();
      if (u && u.owner === me) {
        verbList().forEach((v, k) => {
          if (v === 'move' && (u.moves_left <= 0 || !moveTargets(u).length)) return;
          if (v === 'attack' && !attackTargets(u).length) return;
          if (v === 'recover' && u.hp >= u.max_hp) return;
          out.push({ index: A_VERB + k, kind: 'verb', label: title(v) });
        });
      }
      if (c && c.owner === me) {
        Object.keys(UNIT_KINDS).forEach((t, k) => {
          const kind = UNIT_KINDS[t];
          if (S.players[me].stars < kind.cost) return;
          out.push({
            index: A_TRAIN + k, kind: 'train',
            label: `${title(t)} (${kind.cost}★)`,
          });
        });
      }
      out.push({ index: A_DESELECT, kind: 'verb', label: 'Deselect' });
      return out;
    }

    if (S.phase === 'target') {
      const u = currentSelectionUnit();
      if (u) {
        const targets =
          S.pending_verb === 'attack' ? attackTargets(u) : moveTargets(u);
        for (const t of targets) {
          out.push({
            index: t, kind: 'tile',
            label: S.pending_verb === 'attack' ? 'Attack' : 'Move here',
          });
        }
      }
      out.push({ index: A_DESELECT, kind: 'verb', label: 'Deselect' });
      return out;
    }
    return out;
  }

  function title(s) {
    return String(s).replace(/(^|[-_ ])(\w)/g, (m, a, b) => (a ? ' ' : '') + b.toUpperCase());
  }

  function apply(index) {
    const ok = legal().some((a) => a.index === index);
    if (!ok) throw new Error(`illegal action index ${index} in phase ${S.phase}`);

    if (index === A_DESELECT) {
      S.phase = 'select';
      S.selected = -1;
      S.pending_verb = null;
      return view(0);
    }

    if (index === A_ENDTURN) {
      endTurn();
      return view(0);
    }

    if (S.phase === 'select') {
      S.selected = index;
      S.phase = 'verb';
      S.pending_verb = null;
      return view(0);
    }

    if (S.phase === 'verb') {
      if (index >= A_TRAIN) {
        const t = Object.keys(UNIT_KINDS)[index - A_TRAIN];
        const c = currentSelectionCity();
        const spot = [c.tile].concat(neighbours(c.tile)).find((n) => passable(n) && !unitAt(n));
        if (spot !== undefined) {
          spawnUnit(t, S.current_player, spot);
          S.players[S.current_player].stars -= UNIT_KINDS[t].cost;
          c.units += 1;
          S.log.push(`${title(S.players[S.current_player].tribe)} trains ${title(t)}`);
        }
        S.phase = 'select';
        S.selected = -1;
        recomputeExplored();
        return view(0);
      }
      const verb = verbList()[index - A_VERB];
      if (verb === 'recover') {
        const u = currentSelectionUnit();
        u.hp = Math.min(u.max_hp, u.hp + 2);
        u.turn_ended = true;
        S.phase = 'select';
        S.selected = -1;
        return view(0);
      }
      if (verb === 'disband') {
        const u = currentSelectionUnit();
        u.alive = false;
        S.phase = 'select';
        S.selected = -1;
        recomputeExplored();
        return view(0);
      }
      S.pending_verb = verb;
      S.phase = 'target';
      return view(0);
    }

    if (S.phase === 'target') {
      const tile = index;
      const u = currentSelectionUnit();
      if (S.pending_verb === 'attack') {
        const e = unitAt(tile);
        if (e) {
          e.hp -= 5.5;
          if (e.hp <= 0) {
            e.alive = false;
            S.log.push(`${title(u.type)} kills ${title(e.type)}`);
          } else {
            S.log.push(`${title(u.type)} hits ${title(e.type)} for 5.5`);
          }
        }
        u.turn_ended = true;
        u.moves_left = 0;
      } else {
        u.tile = tile;
        u.moves_left = 0;
        u.turn_ended = true;
        const c = cityAt(tile);
        if (c && c.owner !== u.owner) {
          c.owner = u.owner;
          S.log.push(`${title(u.type)} captures a city`);
        }
      }
      S.phase = 'select';
      S.selected = -1;
      S.pending_verb = null;
      recomputeExplored();
      checkOutcome();
      return view(0);
    }

    return view(0);
  }

  function endTurn() {
    S.current_player = (S.current_player + 1) % S.players.length;
    S.phase = 'select';
    S.selected = -1;
    S.pending_verb = null;
    if (S.current_player === 0) startRound();
  }

  function startRound() {
    S.turn += 1;
    for (const u of S.units) {
      u.turn_ended = false;
      u.moves_left = (UNIT_KINDS[u.type] || UNIT_KINDS.warrior).move;
    }
    for (const p of S.players) p.stars += p.income;
    for (const c of S.cities) {
      c.population += c.owner === 0 ? 1 : 2;
      if (c.population >= c.next_level_at) {
        c.level += 1;
        c.population -= c.next_level_at;
        c.next_level_at += 1;
        c.capacity += 1;
        c.pending_rewards += 1;
        S.log.push(`City reaches level ${c.level}`);
      }
      const owner = S.players[c.owner];
      if (owner) owner.score += 20;
    }
    if (S.turn_limit && S.turn > S.turn_limit) {
      const best = S.players.slice().sort((a, b) => b.score - a.score)[0];
      S.outcome = { how: 'score', winner: best.id, turn: S.turn };
    }
  }

  function checkOutcome() {
    for (const p of S.players) {
      p.alive = S.cities.some((c) => c.owner === p.id);
    }
    const alive = S.players.filter((p) => p.alive);
    if (alive.length === 1 && S.players.length > 1) {
      S.outcome = { how: 'domination', winner: alive[0].id, turn: S.turn };
    }
  }

  function advanceBots() {
    const log = [];
    let guard = 0;
    while (!S.outcome && !S.players[S.current_player].human && guard++ < 64) {
      const me = S.current_player;
      const tribe = title(S.players[me].tribe);
      const mine = S.units.filter((u) => u.owner === me && u.alive && !u.turn_ended);
      for (const u of mine) {
        const atk = attackTargets(u);
        if (atk.length) {
          const e = unitAt(atk[0]);
          e.hp -= 4;
          if (e.hp <= 0) {
            e.alive = false;
            log.push(`${tribe} ${title(u.type)} destroys ${title(e.type)}`);
          } else {
            log.push(`${tribe} ${title(u.type)} attacks ${title(e.type)} (${e.hp.toFixed(1)} hp left)`);
          }
        } else {
          const mv = moveTargets(u);
          if (mv.length) {
            u.tile = mv[Math.floor(mv.length / 2)];
            log.push(`${tribe} moves ${title(u.type)}`);
          }
        }
        u.turn_ended = true;
      }
      const city = S.cities.find((c) => c.owner === me);
      if (city && S.players[me].stars >= 2) {
        const spot = [city.tile].concat(neighbours(city.tile)).find((n) => passable(n) && !unitAt(n));
        if (spot !== undefined) {
          spawnUnit('warrior', me, spot);
          S.players[me].stars -= 2;
          log.push(`${tribe} trains Warrior`);
        }
      }
      log.push(`${tribe} ends turn`);
      endTurn();
      recomputeExplored();
      checkOutcome();
    }
    S.log = S.log.concat(log).slice(-200);
    return { state: view(0), log };
  }

  function view(playerArg) {
    const player = playerArg === undefined ? 0 : playerArg;
    const godView = player === -1 || player === null;
    const useFog = S.fog && !godView;
    const seen = useFog ? S.explored[player] || new Set() : null;

    const tiles = S.tiles
      .filter((t) => !useFog || seen.has(t.i))
      .map((t) => ({
        i: t.i, terrain: t.terrain, resource: t.resource,
        improvement: t.improvement, owner: t.owner, city: t.city, road: t.road,
      }));

    const units = S.units
      .filter((u) => u.alive && (!useFog || seen.has(u.tile)))
      .map((u) => ({
        id: u.id, type: u.type, owner: u.owner, tile: u.tile,
        hp: u.hp, max_hp: u.max_hp, moves_left: u.moves_left,
        veteran: u.veteran, turn_ended: u.turn_ended, carried: u.carried,
      }));

    const cities = S.cities
      .filter((c) => !useFog || seen.has(c.tile))
      .map((c) => ({
        id: c.id, owner: c.owner, tile: c.tile, level: c.level,
        population: c.population, next_level_at: c.next_level_at,
        capital: c.capital, walled: c.walled, besieged: c.besieged,
        units: c.units, capacity: c.capacity,
        pending_rewards: c.pending_rewards, production: c.production,
        name: c.name,
      }));

    return {
      dims: { width: S.width, height: S.height },
      turn: S.turn,
      current_player: S.current_player,
      phase: S.phase,
      selected: S.selected,
      pending_verb: S.pending_verb,
      outcome: S.outcome,
      players: S.players.map((p) => ({
        id: p.id, tribe: p.tribe, human: p.human, alive: p.alive,
        stars: p.stars, score: p.score, income: p.income, techs: p.techs.slice(),
      })),
      tiles, units, cities,
      legal: legal(),
    };
  }

  function handle(req) {
    const params = req.params || {};
    switch (req.method) {
      case 'new_game':
        return newGame(params);
      case 'state':
        if (!S) newGame({});
        return view(params.player === undefined ? 0 : params.player);
      case 'apply':
        if (!S) throw new Error('no game in progress; call new_game first');
        return apply(params.index);
      case 'advance_bots':
        if (!S) throw new Error('no game in progress; call new_game first');
        return advanceBots();
      case 'reload_rules':
        return { ok: true, version: 'mock-fixture-0 (no rules file read)' };
      default:
        throw new Error(`unknown method '${req.method}'`);
    }
  }

  return { handle };
})();

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.ico': 'image/x-icon',
  '.txt': 'text/plain; charset=utf-8',
  '.md': 'text/plain; charset=utf-8',
};

function sendJson(res, status, obj) {
  const body = Buffer.from(JSON.stringify(obj));
  res.writeHead(status, {
    'content-type': 'application/json; charset=utf-8',
    'content-length': body.length,
    'cache-control': 'no-store',
  });
  res.end(body);
}

function serveStatic(req, res) {
  let urlPath;
  try {
    urlPath = decodeURIComponent(req.url.split('?')[0]);
  } catch (_) {
    res.writeHead(400).end('bad url');
    return;
  }
  if (urlPath === '/' || urlPath === '') urlPath = '/index.html';

  const target = path.resolve(WEB_ROOT, '.' + urlPath);
  if (target !== WEB_ROOT && !target.startsWith(WEB_ROOT + path.sep)) {
    res.writeHead(403).end('forbidden');
    return;
  }
  fs.stat(target, (err, st) => {
    if (err || !st.isFile()) {
      res.writeHead(404, { 'content-type': 'text/plain; charset=utf-8' });
      res.end(`404 ${urlPath}`);
      return;
    }
    res.writeHead(200, {
      'content-type': MIME[path.extname(target).toLowerCase()] || 'application/octet-stream',
      'content-length': st.size,
      'cache-control': 'no-store',
    });
    fs.createReadStream(target).pipe(res);
  });
}

function handleRpc(req, res) {
  let body = '';
  let tooBig = false;
  req.setEncoding('utf8');
  req.on('data', (c) => {
    if (tooBig) return;
    body += c;
    if (body.length > 8 * 1024 * 1024) {
      tooBig = true;
      sendJson(res, 413, { id: null, error: 'request body too large' });
      req.destroy();
    }
  });
  req.on('end', async () => {
    if (tooBig) return;
    let parsed;
    try {
      parsed = JSON.parse(body);
    } catch (e) {
      sendJson(res, 400, { id: null, error: `malformed JSON request: ${e.message}` });
      return;
    }
    const id = parsed && typeof parsed.id !== 'undefined' ? parsed.id : null;

    if (OPT.mock) {
      try {
        sendJson(res, 200, { id, result: MOCK.handle(parsed) });
      } catch (e) {
        sendJson(res, 200, { id, error: String(e.message || e) });
      }
      return;
    }

    // Single line in, single line out. Newlines inside the payload would break
    // the framing, so re-serialise rather than forwarding the raw body.
    let line;
    try {
      line = JSON.stringify(parsed);
    } catch (e) {
      sendJson(res, 400, { id, error: `unserialisable request: ${e.message}` });
      return;
    }

    try {
      const reply = await callChild(line);
      let obj;
      try {
        obj = JSON.parse(reply);
      } catch (e) {
        sendJson(res, 200, {
          id,
          error: `engine returned a non-JSON line: ${reply.slice(0, 400)}`,
        });
        return;
      }
      if (obj && typeof obj === 'object' && typeof obj.id === 'undefined') obj.id = id;
      sendJson(res, 200, obj);
    } catch (e) {
      sendJson(res, 200, {
        id,
        error: String(e.message || e),
        engine_missing: e.code === 'ENGINE_MISSING' || undefined,
        hint:
          e.code === 'ENGINE_MISSING'
            ? 'run `node web/server.js --mock` to develop the UI without the engine'
            : undefined,
      });
    }
  });
  req.on('error', () => {
    /* client hung up */
  });
}

const server = http.createServer((req, res) => {
  const urlPath = (req.url || '/').split('?')[0];

  if (urlPath === '/rpc') {
    if (req.method !== 'POST') {
      sendJson(res, 405, { id: null, error: 'POST only' });
      return;
    }
    handleRpc(req, res);
    return;
  }

  if (urlPath === '/health') {
    sendJson(res, 200, {
      ok: true,
      mock: OPT.mock,
      cli: OPT.cli,
      cli_exists: fs.existsSync(OPT.cli),
      child_running: !!child.proc,
      last_engine_error: child.lastError,
    });
    return;
  }

  if (req.method !== 'GET' && req.method !== 'HEAD') {
    res.writeHead(405).end('method not allowed');
    return;
  }
  serveStatic(req, res);
});

server.on('clientError', (err, socket) => {
  if (socket.writable) socket.end('HTTP/1.1 400 Bad Request\r\n\r\n');
});

server.listen(OPT.port, OPT.host, () => {
  const where = `http://${OPT.host}:${OPT.port}/`;
  if (OPT.mock) {
    process.stdout.write(`[server] MOCK engine (no child process)\n`);
  } else {
    process.stdout.write(`[server] engine: ${OPT.cli} --rpc\n`);
    if (!fs.existsSync(OPT.cli)) {
      process.stdout.write(
        `[server] NOTE: that binary does not exist yet. RPC calls will return a ` +
          `clear JSON error until it does; use --mock meanwhile.\n`
      );
    }
  }
  process.stdout.write(`[server] listening on ${where}\n`);
});

function shutdown() {
  killChild('server shutting down');
  server.close(() => process.exit(0));
  setTimeout(() => process.exit(0), 500).unref();
}
process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);
