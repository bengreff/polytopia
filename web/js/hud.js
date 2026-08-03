/*
 * hud.js — every DOM surface. Reads the current GameState plus the current option set and
 * writes the DOM. It holds no game state of its own and never fabricates a target index.
 *
 * The three surfaces of the real game's InteractionBar (ui-reference.md §2.1a) are all here:
 *   ① description panel, bottom-left — what is selected and, critically, WHY YOU CAN'T
 *   ② floating action button in world space above the selected tile
 *   ③ ability / option tray, bottom-left under the description
 * plus the HUD, the rivals strip, the two-step confirm popup and the tech tree overlay.
 *
 * THE RULE THIS FILE EXISTS TO ENFORCE: an option is never dropped. Whatever the engine
 * returns is rendered, annotated with its price and its refusal.
 */

import * as P from './projection.js';
import { GameState, key, pretty, MARKER_VERBS } from './state.js';
import { playerColor, unitAbbrev } from './render.js';

const $ = (id) => document.getElementById(id);

// --- presentation vocabulary ------------------------------------------------
// Text only. Nothing here decides anything; the engine already did.

const REASON_TEXT = {
  'too many':
    "This city can't support any more units. Upgrade the city to get more space for units.",
  'tile occupied':
    'A unit is already standing here, so a newly trained one would have nowhere to appear. ' +
    'Move it out first.',
  'wrong terrain': 'This cannot be built on this kind of terrain.',
  'no resource': 'There is no resource here to work with.',
  'already built': 'Something is already built on this tile.',
  'one per city': 'You may only have one of these per city.',
  'not your territory': 'This tile is outside of your empire.',
  'already acted': "No actions left. Press 'End Turn' to move this unit again.",
  'trained this turn': 'This unit was trained this turn and cannot act until next turn.',
  'must start the turn here': 'The unit must have started the turn on this tile to do this.',
  'nothing in range': 'There is nothing in range.',
  'resolve the level-up first': 'Resolve this city\'s level-up choice first.',
};

const VERB_GLYPH = {
  train: 'TRAIN', build: 'BUILD', harvest: 'HARV', capture: 'CAPT',
  examine: 'RUIN', research: 'TECH', choose_reward: 'LVL', move: 'MOVE', attack: 'ATK',
  heal: 'HEAL', convert: 'CONV', destroy: 'DEST', disband: 'DISB', upgrade_naval: 'SHIP',
};

/** The "what does this do" line of the info popup. */
function effectLine(o, ctx) {
  switch (o.verb) {
    case 'train': return `Trains a ${o.label} in this city.`;
    case 'build': return `Builds a ${o.label} on this tile.`;
    case 'harvest':
      return `Extracts the ${pretty(ctx.resource || 'resource')} and produces population for ` +
             `${ctx.cityName || 'the nearest city'}.`;
    case 'capture': return 'Adds this city to your empire. Capturing ends the unit\'s turn.';
    case 'examine': return 'Enters the ruins and takes whatever is inside.';
    case 'research': return `Unlocks the ${o.label} technology for the rest of the game.`;
    case 'choose_reward': return `Takes ${o.label} as this city's level-up reward.`;
    case 'attack': return 'Attacks the enemy unit. The defender retaliates if it survives.';
    default: return `${pretty(o.verb)} — ${o.label}.`;
  }
}

/** The requirement line, which the real game prints whether or not it is satisfied. */
function requirementLine(o, ctx) {
  switch (o.verb) {
    case 'train': return 'The city must have a free unit slot and a free tile to place the unit on.';
    case 'build': return 'Must be built on a tile inside your own borders.';
    case 'harvest': return `Must be on a tile with ${pretty(ctx.resource || 'a resource')} inside your borders.`;
    case 'capture': return 'The unit must have started the turn standing on the city.';
    case 'examine': return 'The unit must have started the turn standing on the ruins.';
    case 'research': return 'Costs stars; the price rises with the number of cities you own.';
    default: return '';
  }
}

// The tech tree is 5 branches of 5, root then two chains of two (core/content.hpp).
// This table is LAYOUT ONLY — names, positions and nothing else. Status, price and
// prerequisite all come from `research_options`.
const TECH_BRANCHES = [
  { title: 'Climbing', nodes: [['Climbing', 1], ['Mining', 2], ['Smithery', 3], ['Meditation', 2], ['Philosophy', 3]] },
  { title: 'Fishing', nodes: [['Fishing', 1], ['Sailing', 2], ['Navigation', 3], ['Ramming', 2], ['Aquatism', 3]] },
  { title: 'Hunting', nodes: [['Hunting', 1], ['Archery', 2], ['Spiritualism', 3], ['Forestry', 2], ['Mathematics', 3]] },
  { title: 'Organization', nodes: [['Organization', 1], ['Farming', 2], ['Construction', 3], ['Strategy', 2], ['Diplomacy', 3]] },
  { title: 'Riding', nodes: [['Riding', 1], ['Roads', 2], ['Trade', 3], ['Free Spirit', 2], ['Chivalry', 3]] },
];

const TECH_UNLOCKS = {
  climbing: 'Mountain movement, Metal visible', mining: 'Mine', smithery: 'Forge, Swordsman',
  meditation: 'Mountain Temple', philosophy: 'Mind Bender, cheaper techs',
  fishing: 'Harvest Fish', sailing: 'Port, Rafts', navigation: 'Ocean travel, Bomber',
  ramming: 'Rammer', aquatism: 'Water Temple',
  hunting: 'Harvest Game', archery: 'Archer', spiritualism: 'Forest Temple',
  forestry: 'Lumber Hut, Clear Forest', mathematics: 'Sawmill, Catapult',
  organization: 'Harvest Fruit, Crop visible', farming: 'Farm', construction: 'Windmill',
  strategy: 'Defender', diplomacy: 'Cloak, Embassy',
  riding: 'Rider', roads: 'Roads', trade: 'Market',
  freespirit: 'Temple, Disband', chivalry: 'Knight',
};

// --- small helpers -----------------------------------------------------------

function el(tag, cls, text) {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
}

function starPrice(cost) {
  return `★${cost}`;
}

function tribeName(p) {
  if (!p) return '?';
  if (p.tribe == null) return 'Unknown tribe';
  return pretty(p.tribe);
}

// --- log / banner ------------------------------------------------------------

export function appendLog(text, cls = '', turn) {
  const log = $('log');
  if (!log) return;
  const line = el('div', `e ${cls}`, turn != null ? `T${turn} · ${text}` : text);
  log.appendChild(line);
  while (log.childNodes.length > 220) log.removeChild(log.firstChild);
  log.scrollTop = log.scrollHeight;
}

let bannerTimer = null;
export function showBanner(text, cls = '') {
  const b = $('banner');
  b.textContent = text;
  b.className = `banner ${cls}`;
  b.hidden = false;
  clearTimeout(bannerTimer);
  // Everything self-dismisses. A banner that sticks becomes furniture and stops being read —
  // and worse, an error from three actions ago looks like an error about the current one.
  bannerTimer = setTimeout(hideBanner, cls === 'ok' ? 2600 : 6000);
}
export function hideBanner() { $('banner').hidden = true; }

// --- HUD ---------------------------------------------------------------------

export function updateHud(app) {
  const s = app.state;
  if (!s) return;
  const me = s.myPlayer();

  // Your economy, and only yours.
  $('stat-stars').querySelector('b').textContent = me && me.stars != null ? me.stars : '?';
  $('stat-income').querySelector('b').textContent = me && me.income != null ? me.income : '?';
  $('stat-score').querySelector('b').textContent = me && me.score != null ? me.score : '?';
  $('stat-turn').querySelector('b').textContent = s.turn;
  const tribeEl = $('stat-tribe');
  tribeEl.textContent = tribeName(me);
  tribeEl.style.color = playerColor(s.me);

  // END TURN lights up on the ENGINE's `has_actions`, and on nothing else.
  //
  // "Have my units moved" is the wrong question and was the bug: it declares you finished
  // while you still have stars for a Farm, a tech you can afford and a city that can train.
  // has_actions scans units, cities, tiles AND research, so the cue fires exactly when the
  // player can do literally nothing. The idle-unit count is kept only as a secondary hint.
  const btn = $('btn-endturn');
  const mine = s.isMyTurn();
  const idle = app.idleUnits instanceof Set ? app.idleUnits.size : s.activeUnits().length;
  const done = mine && !s.hasActions;
  btn.disabled = !mine;
  btn.classList.toggle('ready', done);
  btn.textContent = !mine
    ? 'WAITING…'
    : done
      ? 'NOTHING LEFT TO DO — END TURN'
      : idle > 0
        ? `END TURN (${idle} unit${idle === 1 ? '' : 's'} still active)`
        : 'END TURN';

  renderRivals(s);

  if (s.outcome) {
    const winner = s.playerById.get(s.outcome.winner);
    showBanner(
      `Game over — ${s.outcome.how || 'finished'}, winner: ` +
      `${tribeName(winner)}${s.outcome.winner === s.me ? ' (you)' : ''}` +
      (s.outcome.turn != null ? ` on turn ${s.outcome.turn}` : ''),
      'ok'
    );
  }
}

function renderRivals(s) {
  const box = $('rivals');
  box.replaceChildren();
  for (const p of s.players) {
    if (p.id === s.me) continue;
    const card = el('div', 'rival');
    card.style.borderLeftColor = playerColor(p.id);
    card.appendChild(el('div', 'rname', `${tribeName(p)}${p.alive === false ? ' (defeated)' : ''}`));
    const row = el('div', 'rrow');
    // Score and city count are permanently public. Stars, income and techs are NEVER shown —
    // and an unknown value is "?", never 0: a zero is indistinguishable from a real value.
    row.appendChild(el('span', '', ''));
    row.lastChild.innerHTML = `◆ <b>${p.score != null ? p.score : '?'}</b>`;
    const c = el('span');
    c.innerHTML = `⌂ <b>${p.cities != null ? p.cities : '?'}</b>`;
    row.appendChild(c);
    const st = el('span', 'hidden-val', '★ ?');
    st.title = "Another tribe's stars and income are never visible.";
    row.appendChild(st);
    card.appendChild(row);
    box.appendChild(card);
  }
}

// --- ① description panel + ③ tray -------------------------------------------

/**
 * Render the current selection. `sel` is app.sel:
 *   {tile, layers[], li, layer, options[], reachable:Set, attackable:Map}
 * `options` is the FULL annotated set from the engine and is rendered whole.
 */
export function renderSelection(app) {
  const s = app.state;
  const sel = app.sel;
  const wrap = $('interaction');
  if (!s || !sel || sel.tile < 0) {
    wrap.hidden = true;
    $('tileaction').hidden = true;
    return;
  }
  wrap.hidden = false;

  const i = sel.tile;
  const tile = s.tileAt(i);
  const unit = s.unitAt(i);
  const city = s.cityAt(i);
  const layer = sel.layer;

  // --- head
  let badge = '···';
  let title = `Tile ${i}`;
  let sub = '';
  if (!tile) {
    badge = '☁';
    title = 'Unexplored';
    sub = 'Cloud';
  } else if (layer === 'unit' && unit) {
    badge = unitAbbrev(unit.type);
    title = pretty(unit.type);
    sub = unit.owner === s.me ? 'Your unit' : `${tribeName(s.playerById.get(unit.owner))} unit — enemy`;
  } else if (layer === 'city' && city) {
    badge = city.capital ? '★' : '⌂';
    title = city.capital ? 'Capital' : 'City';
    sub = city.owner === s.me ? 'Yours' : `${tribeName(s.playerById.get(city.owner))} city`;
  } else {
    badge = (tile.terrain || '??').slice(0, 4).toUpperCase();
    title = pretty(tile.terrain) + (tile.resource && tile.resource !== 'none' ? `, ${pretty(tile.resource)}` : '');
    sub = tile.owner === s.me ? 'Inside your empire'
        : tile.owner >= 0 ? `${tribeName(s.playerById.get(tile.owner))} territory`
        : 'Unclaimed land';
  }
  $('desc-badge').textContent = badge;
  $('desc-title').textContent = title;
  $('desc-sub').textContent = sub;

  // --- body lines
  const lines = $('desc-lines');
  lines.replaceChildren();
  const add = (text, cls = '') => { if (text) lines.appendChild(el('div', `l ${cls}`, text)); };

  if (!tile) {
    add('Explore this area to see what it holds!');
  } else if (layer === 'unit' && unit) {
    add(`HP ${fmt(unit.hp)}/${fmt(unit.max_hp)} · moves left ${unit.moves_left ?? '?'}` +
        (unit.veteran ? ' · veteran' : ''));
    if (unit.owner === s.me) {
      // What this unit can ACTUALLY do, straight from the option set — not a stock sentence.
      const opts = sel.options || [];
      const mv = opts.find((o) => o.verb === 'move');
      const at = opts.find((o) => o.verb === 'attack');
      const can = [];
      if (mv && mv.status === 'available') can.push('Click a cyan ring to move.');
      if (at && at.status === 'available') can.push('Click a red ring to attack.');
      if (GameState.spent(unit)) {
        add("No actions left. Press 'End Turn' to move this unit again.", 'warn');
      } else if (can.length) {
        add(can.join(' '), 'good');
      } else {
        const why = (mv && mv.reason) || (at && at.reason) || 'nothing available';
        add(`This unit has nothing it can do right now (${why}).`, 'warn');
      }
    } else {
      add('This is the enemy!', 'bad');
      add('Enemy units are inspectable, never commandable.');
    }
  } else if (layer === 'city' && city) {
    add(`Level ${city.level}${city.walled ? ' · walled' : ''}${city.besieged ? ' · BESIEGED' : ''}`);
    if (city.owner === s.me) {
      // population may be NEGATIVE and is never clamped.
      const pop = city.population;
      lines.appendChild(popLine(pop, city.next_level_at));
      add(`Units ${city.units}/${city.capacity} · produces ★${city.production}/turn`);
      if (city.pending_rewards > 0) add('This city has a level-up reward waiting — choose it below.', 'warn');
    } else {
      // Enemy cities: name plate only. No income, no population (ui-reference.md §5.7).
      add('Population and income of an enemy city are not visible to you.');
      add('Move a unit here to capture this city!', 'warn');
    }
  } else {
    if (tile.improvement && tile.improvement !== 'none') add(`Improvement: ${pretty(tile.improvement)}`);
    if (tile.resource === 'village') add('Move a unit here to capture this city!', 'warn');
    else if (tile.resource === 'ruin' || tile.resource === 'ruins') add('Move a unit here and examine these ancient ruins.', 'warn');
    else if (tile.resource && tile.resource !== 'none') {
      if (tile.owner !== s.me) add('This resource is outside of your empire.', 'warn');
      else add('Extract this resource to upgrade your city.');
    }
    if (tile.owner !== s.me && !(tile.resource && tile.resource !== 'none')) {
      add(tile.owner >= 0 ? 'This tile belongs to another tribe.' : 'This land is unclaimed.');
    }
  }

  // --- layer tabs (tap-cycle order; a city tile deliberately has NO tile layer)
  const tabs = $('layer-tabs');
  tabs.replaceChildren();
  if (sel.layers.length > 1) {
    sel.layers.forEach((name, idx) => {
      const t = el('button', `layer-tab${idx === sel.li ? ' on' : ''}`, name);
      t.addEventListener('click', () => app.setLayer(idx));
      tabs.appendChild(t);
    });
    tabs.appendChild(el('span', 'layer-hint', 'click the tile again to cycle'));
  } else if (sel.layers.length === 1) {
    tabs.appendChild(el('span', 'layer-tab on', sel.layers[0]));
  }

  renderTray(app);
  renderTileAction(app);
}

function popLine(pop, next) {
  const d = el('div', 'l');
  const val = el('b');
  val.textContent = String(pop);
  if (pop < 0) val.style.color = '#ff6b6b';
  d.appendChild(document.createTextNode('Population '));
  d.appendChild(val);
  d.appendChild(document.createTextNode(next != null ? ` / ${next} to level up` : ''));
  if (pop < 0) d.appendChild(document.createTextNode(' — in debt'));
  return d;
}

function fmt(v) {
  if (typeof v !== 'number') return '?';
  return Number.isInteger(v) ? String(v) : v.toFixed(1);
}

function renderTray(app) {
  const s = app.state;
  const sel = app.sel;
  const tray = $('tray');
  tray.replaceChildren();

  const tile = s.tileAt(sel.tile);
  if (!tile) return;

  const opts = sel.options || [];
  if (!opts.length) {
    const why = trayEmptyReason(app);
    tray.appendChild(el('div', 'tray-note', why));
    return;
  }

  // Marker verbs first, as status chips — in the real game move and attack are RINGS ON THE
  // MAP, not buttons, so turning them into buttons would be a lie. They still appear, so a
  // spent unit visibly says why it can do nothing.
  const markers = opts.filter((o) => MARKER_VERBS.has(o.verb));
  const actions = opts.filter((o) => !MARKER_VERBS.has(o.verb));

  if (markers.length) {
    tray.appendChild(el('div', 'tray-head', 'movement'));
    for (const o of markers) {
      const on = o.status === 'available';
      const chip = el('div', `opt marker${on ? ' on' : ''}${o.verb === 'attack' ? ' atk' : ''}`);
      const row = el('span', 'orow');
      row.appendChild(el('span', 'oname', o.label));
      row.appendChild(el('span', 'otag', on
        ? (o.verb === 'move' ? 'click a cyan ring' : 'click a red ring')
        : `(${o.reason || 'unavailable'})`));
      chip.appendChild(row);
      chip.title = on ? '' : REASON_TEXT[o.reason] || '';
      tray.appendChild(chip);
    }
  }

  if (actions.length) {
    const head = actions[0].verb === 'choose_reward' ? 'level-up reward — choose one'
               : sel.layer === 'city' ? 'train'
               : 'actions';
    tray.appendChild(el('div', 'tray-head', head));
    for (const o of actions) tray.appendChild(optionButton(app, o));
  }
}

function trayEmptyReason(app) {
  const s = app.state;
  const sel = app.sel;
  const tile = s.tileAt(sel.tile);
  const unit = s.unitAt(sel.tile);
  const city = s.cityAt(sel.tile);
  if (!s.isMyTurn()) return 'It is not your turn — this tile is inspectable only right now.';
  if (sel.layer === 'unit' && unit && unit.owner !== s.me) {
    return 'This is the enemy! You can inspect an enemy unit but not command it.';
  }
  if (sel.layer === 'city' && city && city.owner !== s.me) {
    return 'An enemy city offers you no actions. Capture is an action on YOUR unit: move a unit ' +
           'onto the city, then select the unit and press Capture next turn.';
  }
  if (tile && tile.owner !== s.me) {
    return 'This tile is outside of your empire, so there is nothing to build or harvest here. ' +
           'Grow a city\'s borders or capture a village to claim it.';
  }
  return 'No actions are offered on this tile right now.';
}

/**
 * One option button. EVERY status gets a button — the click always does something, even if
 * that something is only an explanation. This is the whole point of v2.
 */
function optionButton(app, o) {
  const b = el('button', `opt ${o.status}`);
  const row = el('span', 'orow');
  const name = el('span', 'oname');
  name.textContent = o.label + (o.status === 'missing_tech' ? ' (locked)' : '');
  row.appendChild(name);
  // The price is ALWAYS shown, including when you cannot afford it.
  if (o.cost > 0) row.appendChild(el('span', 'oprice', starPrice(o.cost)));
  b.appendChild(row);

  if (o.status === 'blocked' && o.reason) {
    b.appendChild(el('span', 'otag', `(${o.reason})`));
  } else if (o.status === 'missing_tech' && o.needs_tech) {
    b.appendChild(el('span', 'otag', `needs ${o.needs_tech}`));
  } else if (o.status === 'cannot_afford') {
    b.appendChild(el('span', 'otag', 'not enough stars'));
  }

  b.addEventListener('click', () => openOption(app, o));
  return b;
}

// --- ② world-space floating action button ------------------------------------

function renderTileAction(app) {
  const holder = $('tileaction');
  const sel = app.sel;
  const primary = (sel.options || []).find((o) => o.status === 'available' && !MARKER_VERBS.has(o.verb));
  if (!primary) { holder.hidden = true; return; }
  holder.hidden = false;
  holder.dataset.tile = String(sel.tile);
  $('tileaction-glyph').textContent = VERB_GLYPH[primary.verb] || pretty(primary.verb);
  $('tileaction-name').textContent = primary.label;
  const price = $('tileaction-price');
  price.textContent = primary.cost > 0 ? starPrice(primary.cost) : '';
  price.hidden = !(primary.cost > 0);
  const btn = $('tileaction-btn');
  btn.onclick = () => openOption(app, primary);
  positionTileAction(app);
}

export function positionTileAction(app) {
  const holder = $('tileaction');
  if (holder.hidden || !app.state || !app.sel || app.sel.tile < 0) return;
  const { x, y } = app.state.tileXY(app.sel.tile);
  const pt = P.tileToScreen(x, y);
  holder.style.left = `${pt.x}px`;
  holder.style.top = `${pt.y - P.tilePixels() * 0.45}px`;
}

// --- combat preview ----------------------------------------------------------

export function showCombatPreview(preview, at, attackerName, defenderName, defenderHp) {
  const c = $('combat');
  if (!preview || !preview.valid) { c.hidden = true; return; }
  c.hidden = false;
  c.replaceChildren();
  c.appendChild(el('h4', '', 'Battle preview'));

  const r1 = el('div', 'row');
  r1.appendChild(el('span', 'you', `${attackerName} deals`));
  const b1 = el('b', 'you');
  b1.textContent = `${preview.attacker_damage} dmg`;
  r1.appendChild(b1);
  c.appendChild(r1);

  const r2 = el('div', 'row');
  r2.appendChild(el('span', 'them', `${defenderName} retaliates`));
  const b2 = el('b', 'them');
  b2.textContent = `${preview.defender_damage} dmg`;
  r2.appendChild(b2);
  c.appendChild(r2);

  // Death warnings for BOTH sides — the plan calls this out explicitly (C4).
  if (preview.defender_dies) {
    c.appendChild(el('div', 'death kill', `${defenderName} DIES — one-hit kill`));
  }
  if (preview.attacker_dies) {
    c.appendChild(el('div', 'death lose', `${attackerName} DIES to retaliation`));
  }
  if (!preview.defender_dies && !preview.attacker_dies && defenderHp != null) {
    c.appendChild(el('div', 'row', ''));
    c.lastChild.textContent = `Both survive (defender ${fmt(defenderHp)} HP now).`;
  }

  const pad = 16;
  const w = 224;
  c.style.left = `${Math.min(window.innerWidth - w - pad, at.x + 18)}px`;
  c.style.top = `${Math.max(pad, at.y - 40)}px`;
}

export function hideCombatPreview() { $('combat').hidden = true; }

// --- two-step confirm popup ---------------------------------------------------

let popupState = null;

export function popupOpen() { return !$('scrim').hidden; }

export function closePopup() {
  $('scrim').hidden = true;
  popupState = null;
}

/**
 * Info-then-commit. There is no undo in this game, so an action is explained before it can
 * be taken (ui-reference.md §2.1b). Each status gets a DIFFERENT button set:
 *   available     BACK (grey)  + DO IT (blue)
 *   cannot_afford BACK (grey)  + DO IT (DARK RED) — clicking it explains, it does not commit
 *   missing_tech  BACK (blue)  + TECH TREE        — no commit button at all
 *   blocked       BACK (blue)                     — no commit button
 */
export function openOption(app, o, ctx = {}) {
  const s = app.state;
  const sel = app.sel || {};
  const tile = sel.tile != null ? s.tileAt(sel.tile) : null;
  const city = sel.tile != null ? s.cityAt(sel.tile) : null;
  const context = {
    resource: tile ? tile.resource : null,
    cityName: city ? (city.capital ? 'your capital' : 'this city') : null,
    ...ctx,
  };

  popupState = { option: o, ctx: context };

  const titleEl = $('pop-title');
  titleEl.replaceChildren();
  titleEl.appendChild(document.createTextNode(o.label));
  if (o.status === 'missing_tech') titleEl.appendChild(el('span', 'locked', ' (locked)'));
  if (o.status === 'blocked' && o.reason) titleEl.appendChild(el('span', 'locked', ` (${o.reason})`));
  if (o.done) titleEl.appendChild(el('span', 'locked', ' (completed)'));

  // The price stays visible in every state, including the ones you cannot take.
  $('pop-price').textContent = o.cost > 0 ? starPrice(o.cost) : '';

  const body = $('pop-body');
  body.replaceChildren();
  body.appendChild(el('div', '', effectLine(o, context)));
  const req = requirementLine(o, context);
  if (req) body.appendChild(el('div', 'req', req));
  if (o.unlocks) body.appendChild(el('div', 'req', `This tech will enable: ${o.unlocks}`));

  const me = s.myPlayer();
  const note = $('pop-note');
  note.hidden = true;

  const back = $('pop-back');
  const commit = $('pop-commit');
  const tech = $('pop-tech');
  back.className = 'pop-btn back';
  back.textContent = 'BACK';
  commit.hidden = true;
  tech.hidden = true;
  commit.className = 'pop-btn commit';

  if (o.done) {
    body.appendChild(el('div', 'req', 'You have already researched this technology.'));
    back.className = 'pop-btn back blue';
  } else if (o.status === 'available') {
    commit.hidden = false;
    commit.textContent = o.verb === 'research' ? 'RESEARCH' : 'DO IT';
    commit.onclick = () => { closePopup(); app.commit(o); };
  } else if (o.status === 'cannot_afford') {
    const have = me && me.stars != null ? me.stars : '?';
    note.hidden = false;
    note.textContent =
      `You don't have enough stars to buy this. It costs ★${o.cost} and you have ★${have}. ` +
      `Press 'End Turn' to collect your income.`;
    commit.hidden = false;
    commit.className = 'pop-btn commit poor';
    commit.textContent = o.verb === 'research' ? 'RESEARCH' : 'DO IT';
    // Clickable, but it explains rather than commits — the real game's "unavailable callback".
    commit.onclick = () => {
      showBanner(`${o.label} costs ★${o.cost}; you have ★${have}.`, '');
      setTimeout(hideBanner, 2400);
    };
  } else if (o.status === 'missing_tech') {
    note.hidden = false;
    note.style.background = '#2b2410';
    note.style.borderColor = '#7a6320';
    note.style.color = '#ffe6a8';
    note.textContent =
      `You need to research ${o.needs_tech || 'a technology'} to do this. ` +
      `Tap the 'TECH TREE' button.`;
    tech.hidden = false;
    tech.onclick = () => { closePopup(); app.openTechTree(o.needs_tech); };
    back.className = 'pop-btn back blue';
  } else {
    body.appendChild(el('div', 'warn', REASON_TEXT[o.reason] || 'You cannot do this right now.'));
    back.className = 'pop-btn back blue';
  }

  if (o.status !== 'missing_tech') {
    note.style.background = '';
    note.style.borderColor = '';
    note.style.color = '';
  }

  back.onclick = closePopup;
  $('scrim').hidden = false;
}

// --- ruin / event result popup ------------------------------------------------

/**
 * What the ENGINE just gave you, said out loud. The reward is rolled server-side, so the log
 * line is the only place the answer exists — and a line in the log panel, under the bot
 * chatter, is not an answer the player will read. Raised over the map instead.
 */
export function resultOpen() { return !$('result-scrim').hidden; }
export function closeResult() { $('result-scrim').hidden = true; }

export function showResult(kicker, title, body, buttonText = 'TAKE IT') {
  $('result-kicker').textContent = kicker;
  $('result-title').textContent = title;
  $('result-body').textContent = body || '';
  $('result-ok').textContent = buttonText;
  $('result-scrim').hidden = false;
}

/**
 * Split an engine ruin line — "Ruin: Scrolls of Wisdom -- a free technology" — into its
 * headline and its explanation. Anything that does not match is shown whole rather than
 * dropped: an unrecognised reward must still reach the player.
 */
export function parseRuinLine(line) {
  const text = String(line).replace(/^Ruin:\s*/, '');
  const m = text.match(/^(.*?)\s*--\s*(.*)$/);
  return m ? { title: m[1], body: m[2] } : { title: text, body: '' };
}

// --- city level-up: a TRUE modal ----------------------------------------------

let levelUpFor = -1;

export function levelUpOpen() { return !$('levelup-scrim').hidden; }
export function levelUpCity() { return levelUpFor; }
export function closeLevelUp() {
  $('levelup-scrim').hidden = true;
  levelUpFor = -1;
}

/** What each reward actually does, so the choice is informed rather than a coin flip. */
const REWARD_TEXT = {
  workshop: 'This city produces one more star every turn, permanently.',
  explorer: 'Spawns an Explorer that reveals a large area of the map around it.',
  citywall: 'Walls this city: defenders here get a large defence bonus, and the city cannot be captured while any unit stands in it.',
  resources: 'Immediately gives you ★5.',
  populationgrowth: 'Adds 3 population to this city right now, pushing it towards the next level.',
  bordergrowth: "Expands this city's borders by one ring, claiming the tiles around it.",
  park: 'A Grand Park: a large, permanent score bonus.',
  superunit: 'Trains a Giant — the strongest unit in the game.',
};

/**
 * Raise the level-up choice. There is no dismiss path on purpose: the engine refuses every
 * other verb while a reward is outstanding, so a closable dialog would leave the player
 * clicking a board that silently does nothing — which reads as the game being frozen.
 */
export function openLevelUp(app, city, options) {
  levelUpFor = city.id;
  $('lvl-title').textContent = `${city.capital ? 'Your capital' : 'Your city'} reached level ${city.level}`;
  $('lvl-sub').textContent = 'Choose its reward.';

  const box = $('lvl-options');
  box.replaceChildren();
  if (!options.length) {
    box.appendChild(el('div', 'lvl-foot', 'The engine offered no choices for this level-up.'));
  }
  for (const o of options) {
    const b = el('button', `lvl-opt${o.status === 'available' ? '' : ' blocked'}`);
    b.appendChild(el('span', 'ln', o.label));
    b.appendChild(el('span', 'ld', REWARD_TEXT[key(o.label)] || ''));
    if (o.status !== 'available' && o.reason) b.appendChild(el('span', 'ld', `(${o.reason})`));
    b.addEventListener('click', () => app.chooseReward(city, o));
    box.appendChild(b);
  }
  $('levelup-scrim').hidden = false;
}

// --- tech tree overlay --------------------------------------------------------

export function techTreeOpen() { return !$('tech-overlay').hidden; }
export function closeTechTree() { $('tech-overlay').hidden = true; }

/**
 * Full-screen tech tree from `research_options`. Already-researched techs are absent from
 * that list, so they are recovered from your own `techs` array and rendered "(completed)".
 * A tech missing from BOTH is still drawn, greyed, so the 5x5 shape is never gappy.
 */
export function renderTechTree(app, optionList, highlight) {
  const s = app.state;
  const me = s.myPlayer();
  $('tech-stars').textContent = me && me.stars != null ? me.stars : '?';

  const byName = new Map();
  for (const o of optionList) byName.set(key(o.label), o);
  const known = new Set((me && me.techs ? me.techs : []).map(key));

  const grid = $('tech-grid');
  grid.replaceChildren();
  for (const branch of TECH_BRANCHES) {
    const col = el('div', 'branch');
    col.appendChild(el('h3', '', branch.title));
    for (const [name, tier] of branch.nodes) {
      const k = key(name);
      const o = byName.get(k);
      const done = known.has(k);
      const status = done ? 'done' : o ? o.status : 'blocked';
      const node = el('button', `tnode tier${tier} ${status}`);
      if (highlight && key(highlight) === k) node.style.outline = '2px solid #ffd05a';

      node.appendChild(el('div', 'tname', name + (status === 'missing_tech' ? ' (locked)' : '')));
      const meta = el('div', 'tmeta');
      const price = el('span', 'tprice', o ? starPrice(o.cost) : done ? 'researched' : '—');
      meta.appendChild(price);
      if (done) meta.appendChild(el('span', '', '✓ completed'));
      else if (o && o.status === 'missing_tech') meta.appendChild(el('span', '', `needs ${o.needs_tech}`));
      else if (o && o.status === 'cannot_afford') meta.appendChild(el('span', '', 'not enough stars'));
      node.appendChild(meta);
      node.appendChild(el('div', 'tmeta', TECH_UNLOCKS[k] || ''));

      node.addEventListener('click', () => {
        if (o) openOption(app, { ...o, unlocks: TECH_UNLOCKS[k] || '' });
        else openOption(app, {
          verb: 'research', label: name, status: 'blocked', cost: 0,
          reason: done ? '' : 'unavailable', done, unlocks: TECH_UNLOCKS[k] || '',
        });
      });
      col.appendChild(node);
    }
    grid.appendChild(col);
  }
  $('tech-overlay').hidden = false;
}

// --- wiring -------------------------------------------------------------------

export function initHud(app) {
  $('btn-endturn').addEventListener('click', () => app.endTurn());
  $('btn-tech').addEventListener('click', () => app.toggleTechTree());
  $('tech-close').addEventListener('click', closeTechTree);
  $('desc-close').addEventListener('click', () => app.deselect());
  $('log-toggle').addEventListener('click', () => {
    $('logpanel').classList.toggle('collapsed');
    $('log-toggle').textContent = $('logpanel').classList.contains('collapsed') ? '+' : '–';
  });
  $('scrim').addEventListener('click', (e) => { if (e.target.id === 'scrim') closePopup(); });

  $('result-ok').addEventListener('click', closeResult);

  const dlg = $('newgame-dialog');
  $('btn-menu').addEventListener('click', () => {
    if (levelUpOpen()) return;
    dlg.showModal();
    refreshMapInfo(app);
  });

  // The 12 tribes, for YOUR seat only. Opponents are always sent as "random": the engine
  // picks an unused tribe from a seeded generator and then HIDES its identity until you meet
  // them, which is the whole point — letting the player pick them would spoil it.
  const tribes = ['imperius', 'bardur', 'xin_xi', 'oumaji', 'kickoo', 'hoodrick',
                  'luxidoor', 'vengir', 'zebasi', 'ai_mo', 'quetzali', 'yadakk'];
  const mine = $('ng-tribe');
  mine.replaceChildren();
  for (const t of tribes) mine.appendChild(new Option(pretty(t), t));
  mine.value = 'imperius';

  for (const id of ['ng-width', 'ng-height']) {
    $(id).addEventListener('change', () => refreshMapInfo(app));
    $(id).addEventListener('input', () => refreshMapInfo(app));
  }
  $('ng-opponents').addEventListener('change', () => clampOpponents());

  $('newgame-form').addEventListener('submit', (e) => {
    if (e.submitter && e.submitter.value === 'cancel') return;
    const n = clampOpponents();
    const difficulty = $('ng-difficulty').value;
    const players = [{ tribe: mine.value, human: true, difficulty }];
    for (let i = 0; i < n; i++) players.push({ tribe: 'random', human: false, difficulty });
    // No turn_limit key at all: games end by domination, and Perfection sets its own.
    app.newGame({
      width: Number($('ng-width').value),
      height: Number($('ng-height').value),
      seed: Number($('ng-seed').value),
      mode: $('ng-mode').value,
      fog: $('ng-fog').checked,
      players,
    });
  });
}

/** Hold the opponent count inside what the map can actually seat. */
function clampOpponents() {
  const box = $('ng-opponents');
  const max = Number(box.max) || 1;
  let n = Math.round(Number(box.value) || 1);
  if (!Number.isFinite(n) || n < 1) n = 1;
  if (n > max) n = max;
  box.value = String(n);
  return n;
}

/**
 * Ask the ENGINE how many players this map seats. Player count is bounded by settlement
 * sites, and generation refuses a game with more players than villages — so the picker is
 * clamped from `map_info` rather than offering counts that will be rejected on Start.
 */
async function refreshMapInfo(app) {
  const note = $('ng-seats');
  const w = Number($('ng-width').value);
  const h = Number($('ng-height').value);
  if (!(w > 0 && h > 0)) { note.textContent = 'Enter a map size.'; return; }
  try {
    const r = await app.rpc('map_info', { width: w, height: h, land_ratio: 0.6 });
    const seats = Math.max(2, Number(r.max_players) || 2);
    $('ng-opponents').max = String(seats - 1);
    const n = clampOpponents();
    note.textContent =
      `This ${w}×${h} map seats up to ${seats} players — ${seats - 1} opponent` +
      `${seats === 2 ? '' : 's'} (about ${r.expected_villages} villages). You have ${n}.`;
  } catch (e) {
    note.textContent = `Could not read the map limit: ${e.message}`;
  }
}
