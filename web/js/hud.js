/*
 * hud.js — DOM panels. Reads the current GameState, writes the DOM. Holds no
 * game state of its own and never computes an action index; every button it
 * makes carries a legal entry handed over by the engine.
 */

import * as P from './projection.js';
import { playerColor, formatHp, unitAbbrev } from './render.js';

const TRIBES = [
  'imperius', 'bardur', 'xin-xi', 'oumaji', 'kickoo', 'hoodrick',
  'luxidoor', 'vengir', 'zebasi', 'ai-mo', 'quetzali', 'yadakk',
];

const el = (id) => document.getElementById(id);

function title(s) {
  return String(s || '').replace(/[-_]/g, ' ').replace(/\b\w/g, (c) => c.toUpperCase());
}

export function initHud(app) {
  const tribeSel = el('ng-tribe');
  const opTribeSel = el('ng-optribe');
  for (const t of TRIBES) {
    tribeSel.appendChild(new Option(title(t), t));
    opTribeSel.appendChild(new Option(title(t), t));
  }
  tribeSel.value = 'imperius';
  opTribeSel.value = 'bardur';

  el('btn-newgame').addEventListener('click', () => el('newgame-dialog').showModal());
  el('ng-cancel').addEventListener('click', (e) => {
    e.preventDefault();
    el('newgame-dialog').close();
  });
  el('newgame-form').addEventListener('submit', (e) => {
    if (e.submitter && e.submitter.value === 'cancel') return;
    e.preventDefault();
    el('newgame-dialog').close();
    app.newGame(readNewGameForm());
  });

  el('btn-endturn').addEventListener('click', () => app.endTurn());
  el('btn-deselect').addEventListener('click', () => app.deselect());
  el('btn-reload').addEventListener('click', () => app.reloadRules());
  el('btn-fit').addEventListener('click', () => app.fit());
  el('chk-ids').addEventListener('change', (e) => {
    app.ui.showIds = e.target.checked;
    app.draw();
  });
}

function readNewGameForm() {
  const num = (id, dflt) => {
    const v = Number(el(id).value);
    return Number.isFinite(v) ? v : dflt;
  };
  const opponents = num('ng-opponents', 1);
  const myTribe = el('ng-tribe').value;
  const opTribe = el('ng-optribe').value;
  const difficulty = el('ng-difficulty').value;

  const players = [{ tribe: myTribe, human: true, difficulty }];
  for (let i = 0; i < opponents; i++) {
    const t = i === 0 ? opTribe : TRIBES[(TRIBES.indexOf(opTribe) + i) % TRIBES.length];
    players.push({ tribe: t, human: false, difficulty });
  }

  return {
    width: num('ng-width', 11),
    height: num('ng-height', 11),
    seed: num('ng-seed', 12345),
    mode: el('ng-mode').value,
    turn_limit: num('ng-turnlimit', 0),
    fog: el('ng-fog').checked,
    players,
  };
}

export function setEngineBadge(text, cls) {
  const b = el('engine-badge');
  b.textContent = text;
  b.className = 'badge' + (cls ? ' ' + cls : '');
}

export function showBanner(text, cls) {
  const b = el('banner');
  b.textContent = text;
  b.className = 'banner' + (cls ? ' ' + cls : '');
  b.hidden = false;
}

export function hideBanner() {
  el('banner').hidden = true;
}

export function appendLog(text, cls, turn) {
  const box = el('log');
  const line = document.createElement('div');
  line.className = 'log-line ' + (cls || '');
  if (turn !== undefined && turn !== null) {
    const t = document.createElement('span');
    t.className = 'log-turn';
    t.textContent = `T${turn}`;
    line.appendChild(t);
  }
  line.appendChild(document.createTextNode(text));
  box.prepend(line);
  while (box.childElementCount > 400) box.lastElementChild.remove();
}

// ---------------------------------------------------------------------------

export function updateHud(app) {
  const s = app.state;
  if (!s) return;

  el('turn-value').textContent = s.turn;
  const cur = s.currentPlayerObj();
  const pv = el('player-value');
  pv.textContent = cur ? `${title(cur.tribe)} (${cur.human ? 'you' : 'bot'})` : '–';
  pv.style.color = playerColor(s.currentPlayer);
  const phase = el('phase-value');
  phase.textContent = s.phase;
  phase.className = 'chip ' + s.phase;

  renderPlayers(s);
  renderSelection(s);
  renderActions(app, s);
  renderVerbMenu(app, s);
  renderHint(s);

  const endTurn = s.endTurnAction();
  el('btn-endturn').disabled = !endTurn;
  el('btn-deselect').disabled = !s.deselectAction();

  if (s.outcome) {
    const w = s.playerById.get(s.outcome.winner);
    showBanner(
      `GAME OVER — ${w ? title(w.tribe) : 'player ' + s.outcome.winner} wins ` +
        `by ${s.outcome.how} on turn ${s.outcome.turn}`,
      'win'
    );
  }
}

function renderPlayers(s) {
  const box = el('players');
  box.textContent = '';
  for (const p of s.players) {
    const row = document.createElement('div');
    row.className =
      'player-row' + (p.id === s.currentPlayer ? ' active' : '') + (p.alive === false ? ' dead' : '');

    const sw = document.createElement('div');
    sw.className = 'swatch';
    sw.style.background = playerColor(p.id);
    row.appendChild(sw);

    const name = document.createElement('div');
    name.className = 'player-name';
    name.textContent = title(p.tribe);
    const tag = document.createElement('span');
    tag.className = 'player-tag';
    tag.textContent = (p.human ? 'human' : 'bot') + (p.alive === false ? ' · out' : '');
    name.appendChild(tag);
    row.appendChild(name);

    const stats = document.createElement('div');
    stats.className = 'player-stats';
    const techs = Array.isArray(p.techs) ? p.techs : [];
    stats.appendChild(span('s-stars', `${p.stars}★`, 'stars'));
    stats.appendChild(span('s-income', `+${p.income}`, 'income per turn'));
    stats.appendChild(span('s-score', `${p.score}`, 'score'));
    stats.appendChild(span('s-tech', `⌬${techs.length}`, 'techs: ' + (techs.join(', ') || 'none')));
    row.appendChild(stats);

    box.appendChild(row);
  }
}

function span(cls, text, tip) {
  const e = document.createElement('span');
  e.className = cls;
  e.textContent = text;
  if (tip) e.title = tip;
  return e;
}

function dl(pairs) {
  const d = document.createElement('dl');
  d.className = 'sel-grid';
  for (const [k, v, cls] of pairs) {
    const dt = document.createElement('dt');
    dt.textContent = k;
    const dd = document.createElement('dd');
    dd.textContent = v;
    if (cls) dd.className = cls;
    d.appendChild(dt);
    d.appendChild(dd);
  }
  return d;
}

function flags(list) {
  const box = document.createElement('div');
  box.className = 'flags';
  for (const f of list) {
    const e = document.createElement('span');
    e.className = 'flag';
    e.textContent = f;
    box.appendChild(e);
  }
  return box;
}

function renderSelection(s) {
  const box = el('selection');
  box.textContent = '';

  if (s.selected < 0) {
    box.className = 'muted';
    box.textContent =
      s.phase === 'select'
        ? 'Click a highlighted tile to select a unit or city.'
        : 'Nothing selected.';
    return;
  }
  box.className = '';

  const { x, y } = s.tileXY(s.selected);
  const tile = s.tileAt(s.selected);
  const head = document.createElement('div');
  head.className = 'sel-block';
  const t = document.createElement('div');
  t.className = 'sel-title';
  t.textContent = `Tile ${s.selected} (${x}, ${y})`;
  head.appendChild(t);
  if (tile) {
    head.appendChild(
      dl([
        ['Terrain', title(tile.terrain)],
        ['Resource', tile.resource && tile.resource !== 'none' ? title(tile.resource) : '–'],
        ['Improvement', tile.improvement && tile.improvement !== 'none' ? title(tile.improvement) : '–'],
        ['Owner', tile.owner >= 0 ? title((s.playerById.get(tile.owner) || {}).tribe || tile.owner) : 'neutral'],
      ])
    );
  }
  box.appendChild(head);

  const unit = s.selectedUnit();
  if (unit) {
    const blk = document.createElement('div');
    blk.className = 'sel-block';
    const ttl = document.createElement('div');
    ttl.className = 'sel-title';
    ttl.style.color = playerColor(unit.owner);
    ttl.textContent = `${title(unit.type)} [${unitAbbrev(unit.type)}] #${unit.id}`;
    blk.appendChild(ttl);
    blk.appendChild(
      dl([
        ['HP', `${formatHp(unit.hp)} / ${formatHp(unit.max_hp)}`],
        ['Moves left', String(unit.moves_left)],
        ['Owner', title((s.playerById.get(unit.owner) || {}).tribe || unit.owner)],
        ['Carrying', unit.carried ? title(unit.carried) : '–'],
      ])
    );
    const f = [];
    if (unit.veteran) f.push('veteran');
    if (unit.turn_ended) f.push('turn ended');
    if (f.length) blk.appendChild(flags(f));
    box.appendChild(blk);
  }

  const city = s.selectedCity();
  if (city) {
    const blk = document.createElement('div');
    blk.className = 'sel-block';
    const ttl = document.createElement('div');
    ttl.className = 'sel-title';
    ttl.style.color = playerColor(city.owner);
    ttl.textContent = `${city.name ? title(city.name) : 'City'} #${city.id} — level ${city.level}`;
    blk.appendChild(ttl);
    // population may be negative; render it red and never clamp it.
    blk.appendChild(
      dl([
        ['Level', String(city.level)],
        ['Population', `${city.population}${city.next_level_at !== undefined ? ' / ' + city.next_level_at : ''}`,
          city.population < 0 ? 'neg' : ''],
        ['Capacity', `${city.units ?? '?'} / ${city.capacity ?? '?'}`],
        ['Production', String(city.production ?? '–')],
        ['Pending rewards', String(city.pending_rewards ?? 0)],
      ])
    );
    const f = [];
    if (city.capital) f.push('capital');
    if (city.walled) f.push('walled');
    if (city.besieged) f.push('besieged');
    if (f.length) blk.appendChild(flags(f));
    box.appendChild(blk);
  }
}

/** One button per legal action. `entry` is the engine's own object, passed through untouched. */
function actionButton(app, entry) {
  const b = document.createElement('button');
  b.className = 'action-btn';
  b.textContent = entry.label || `#${entry.index}`;
  const idx = document.createElement('span');
  idx.className = 'idx';
  idx.textContent = `#${entry.index}`;
  b.appendChild(idx);
  b.title = `${entry.kind} → apply(index=${entry.index})`;
  b.addEventListener('click', () => app.applyAction(entry));
  return b;
}

function renderActions(app, s) {
  const box = el('actions');
  box.textContent = '';
  el('actions-count').textContent = `${s.legal.length} legal`;

  const verbs = s.verbActions();
  if (!verbs.length) {
    box.className = 'muted';
    box.textContent = s.outcome ? 'Game over.' : 'No non-map actions available.';
    return;
  }
  box.className = '';

  const groups = new Map();
  for (const a of verbs) {
    const k = a.kind || 'other';
    if (!groups.has(k)) groups.set(k, []);
    groups.get(k).push(a);
  }
  for (const [kind, list] of groups) {
    const g = document.createElement('div');
    g.className = 'action-group';
    const lbl = document.createElement('div');
    lbl.className = 'glabel';
    lbl.textContent = kind;
    g.appendChild(lbl);
    const wrap = document.createElement('div');
    wrap.className = 'action-list';
    for (const a of list) wrap.appendChild(actionButton(app, a));
    g.appendChild(wrap);
    box.appendChild(g);
  }
}

/**
 * The floating verb menu. Anchored via projection.tileToScreen so an isometric
 * swap moves it correctly with no change here.
 */
function renderVerbMenu(app, s) {
  const menu = el('verbmenu');
  menu.textContent = '';

  const showable = s.phase === 'verb' && s.selected >= 0;
  if (!showable) {
    menu.hidden = true;
    return;
  }
  const verbs = s.verbActions();
  if (!verbs.length) {
    menu.hidden = true;
    return;
  }

  const ttl = document.createElement('div');
  ttl.className = 'vm-title';
  const u = s.selectedUnit();
  const c = s.selectedCity();
  ttl.textContent = u ? title(u.type) : c ? `City L${c.level}` : 'Actions';
  menu.appendChild(ttl);
  for (const a of verbs) menu.appendChild(actionButton(app, a));

  menu.hidden = false;

  const { x, y } = s.tileXY(s.selected);
  const pos = P.tileToScreen(x, y);
  const stage = el('stage');
  const sw = stage.clientWidth;
  const sh = stage.clientHeight;
  const mw = menu.offsetWidth || 140;
  const mh = menu.offsetHeight || 120;
  let left = pos.x + P.tileRadius() + 8;
  let top = pos.y - mh / 2;
  if (left + mw > sw - 8) left = pos.x - P.tileRadius() - 8 - mw;
  menu.style.left = Math.max(8, Math.min(sw - mw - 8, left)) + 'px';
  menu.style.top = Math.max(8, Math.min(sh - mh - 8, top)) + 'px';
}

function renderHint(s) {
  const hint = el('hint');
  if (s.outcome) {
    hint.hidden = true;
    return;
  }
  if (s.phase === 'target') {
    hint.textContent = `Click a highlighted tile to ${title(s.pendingVerb || 'act')} · Esc or right-click to cancel`;
    hint.hidden = false;
  } else if (s.phase === 'verb') {
    hint.textContent = 'Pick an action · Esc or right-click to deselect';
    hint.hidden = false;
  } else {
    hint.hidden = true;
  }
}
