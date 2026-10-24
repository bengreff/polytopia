// Browser client for polyhost serve. It draws the state reported by the game's own code
// and sends back the index of one of the moves the game itself generated.

const $ = id => document.getElementById(id);
const canvas = $('map'), ctx = canvas.getContext('2d');
const TW = 96, TH = 48, DEPTH = 11;

let data = null, names = {}, state = null;
let sel = null;                       // {x, y, layer: 'unit' | 'tile'}
const view = { s: 1, ox: 0, oy: 0 };
let centeredFor = null, busy = false;

const TERRAIN = { field: ['#7fcf4b', '#5e9f35', '#4d8a2a'], forest: ['#6cbf3f', '#4f9a2c', '#3f8224'],
  mountain: ['#8fbf55', '#6b9a3c', '#577f2f'], water: ['#53c3f0'], ocean: ['#2a8fd6'], ice: ['#eef6fb', '#c6d9e6', '#aec6d6'],
  wetland: ['#76b35a', '#5a8f45', '#4a7a38'], mangrove: ['#4f9a55', '#3b7a40', '#306634'] };
const RESOURCE = { game: '🦌', crop: '🌾', fish: '🐟', whale: '🐋', metal: '🪨', fruit: '🍒', spores: '🍄', starfish: '⭐', aquacrop: '🌿' };
const BUILT = { farm: '🌾', windmill: '🌬️', fishing: '🎣', port: '⚓', lumberhut: '🛖', sawmill: '🪚', mine: '⛏️', forge: '⚒️',
  temple: '⛩️', foresttemple: '⛩️', watertemple: '⛩️', mountaintemple: '⛩️', icetemple: '⛩️', market: '💰', lighthouse: '🗼',
  outpost: '🏕️', bridge: '🌉', sanctuary: '🐾', icebank: '🧊', ruin: '🏛️', road: '' };
const ACTION_ICON = { harvestfruit: '🍒', hunting: '🏹', fishing: '🎣', farm: '🌾', mine: '⛏️', lumberhut: '🪓', clearforest: '🪓',
  burnforest: '🔥', growforest: '🌳', windmill: '🌬️', sawmill: '🪚', forge: '⚒️', port: '⚓', road: '🛤️', market: '💰',
  bridge: '🌉', outpost: '🏕️', starfishing: '⭐', whaling: '🐋', aquafarm: '🌿' };
const UNIT_ICON = { warrior: '🗡️', archer: '🏹', rider: '🐎', knight: '♞', defender: '🛡️', swordsman: '⚔️', catapult: '☄️',
  giant: '🗿', mindbender: '🧠', scout: '🔭', cloak: '🥷', dagger: '🔪', pirate: '🏴‍☠️', scoutship: '⛵', transportship: '🚢',
  bombership: '💣', rammership: '🚤', juggernaut: '🛳️', polytaur: '🐂', amphibian: '🐸', tridention: '🔱', crab: '🦀',
  shaman: '🔮', babydragon: '🐲', firedragon: '🐉', dragonegg: '🥚', mooni: '🌙', battlesled: '🛷', icefortress: '🏰',
  icearcher: '🏹', jelly: '🪼', shark: '🦈', siren: '🧜', bunny: '🐰', gaami: '🦍', hexapod: '🐜', doomux: '🪲',
  phychi: '🦟', kiton: '🐞', exida: '🦗', centipede: '🐛', raychi: '🦋', mantis: '🦗', moth: '🦋', larva: '🐛' };
const UNIT_ACTION = { Recover: ['➕', 'Recover'], Disband: ['✨', 'Disband'], Capture: ['🚩', 'Capture'], Promote: ['⭐', 'Promote'],
  Upgrade: ['⬆️', 'Upgrade'], ExamineRuins: ['🔍', 'Examine ruins'], HealOthers: ['💗', 'Heal others'], Explode: ['💥', 'Explode'],
  FreezeArea: ['❄️', 'Freeze area'], BreakIce: ['🧊', 'Break ice'], Boost: ['💪', 'Boost'], Hide: ['🫥', 'Hide'],
  Disembark: ['⚓', 'Disembark'], Decompose: ['♻️', 'Decompose'], Destroy: ['💣', 'Destroy'], Flood: ['🌊', 'Flood'],
  Swarm: ['🐝', 'Swarm'], Stay: ['⏸️', 'Stay'], EstablishEmbassy: ['🏛️', 'Embassy'], DestroyEmbassy: ['🏚️', 'Destroy embassy'],
  ClearTileEffect: ['🧹', 'Clear'], BreakPeace: ['⚔️', 'Break peace'], PeaceTreaty: ['🕊️', 'Peace treaty'] };
const REWARD = { 1: ['🧱', 'City wall', 'Defence bonus in this city'], 2: ['🌳', 'Park', '+1★ and score'],
  3: ['⚙️', 'Workshop', '+1★ production'], 4: ['🧭', 'Explorer', 'Reveals the surrounding land'],
  5: ['🗺️', 'Border growth', 'Expands the city borders'], 6: ['🗿', 'Super unit', 'A super unit appears'],
  7: ['💰', 'Resources', 'Instant stars'], 8: ['👪', 'Population growth', '+3 population'],
  11: ['🔥', 'Rebellion', 'Rebels appear'], 13: ['☠️', 'Poison', 'Poison'] };
const PRETTY = { harvestfruit: 'Harvest fruit', clearforest: 'Clear forest', burnforest: 'Burn forest', growforest: 'Grow forest',
  lumberhut: 'Lumber hut', foresttemple: 'Forest temple', watertemple: 'Water temple', mountaintemple: 'Mountain temple',
  icetemple: 'Ice temple', starfishing: 'Starfishing', aquafarm: 'Aqua farm', icebank: 'Ice bank', riding2: 'Riding',
  freespirit: 'Free spirit', polarwarfare: 'Polar warfare', forestmagic: 'Forest magic', watermagic: 'Water magic' };

const title = s => s ? (PRETTY[s] || s.charAt(0).toUpperCase() + s.slice(1)) : '?';
const nameOf = (table, idx) => names[table]?.[idx] ?? `#${idx}`;
const same = (a, b) => a && b && a[0] === b[0] && a[1] === b[1];
const me = () => state.players.find(p => p.id === state.human);
const tileAt = (x, y) => (x >= 0 && y >= 0 && x < state.width && y < state.height) ? state.tiles.find(t => t.x === x && t.y === y) : null;
const myTurn = () => state.state !== 4 && state.currentPlayer === state.human;

function tribeColor(tribe) {
  const t = data.tribeData[nameOf('tribeData', tribe)];
  return t && t.color ? '#' + t.color.toString(16).padStart(6, '0') : '#999';
}
const playerOf = id => state.players.find(p => p.id === id);
// In a mirror match (92% of the human championship games) both players have the tribe's colour:
// player 2 gets the opposite hue so the sides stay apart.
function oppositeHue(hex) {
  const [r, g, b] = [1, 3, 5].map(i => parseInt(hex.slice(i, i + 2), 16) / 255);
  const max = Math.max(r, g, b), min = Math.min(r, g, b), l = (max + min) / 2, d = max - min;
  let h = 0;
  if (d) h = max === r ? ((g - b) / d) % 6 : max === g ? (b - r) / d + 2 : (r - g) / d + 4;
  return `hsl(${(h * 60 + 540) % 360}, 75%, ${Math.round(Math.min(0.6, Math.max(0.42, l)) * 100)}%)`;
}
const playerColor = id => {
  if (id === 255) return '#cfcfcf';
  const p = playerOf(id);
  if (!p) return '#999';
  const c = tribeColor(p.tribe), other = state.players.find(q => q.id !== id && q.id !== 255);
  return id === 2 && other && other.tribe === p.tribe ? oppositeHue(c) : c;
};
const tribeName = id => playerOf(id) ? title(nameOf('tribeData', playerOf(id).tribe)) : '';

function legalWith(pred) { return state.legal.map((m, i) => [m, i]).filter(([m]) => pred(m)); }
const origin = m => m.args.From || m.args.Origin;
const target = m => m.args.To || m.args.Target;
const where = m => m.args.Coordinates || m.args.coordinates || target(m);

// ---------- view ----------

function resize() {
  const dpr = devicePixelRatio || 1;
  canvas.width = innerWidth * dpr; canvas.height = innerHeight * dpr;
  draw();
}
function worldOf(x, y) { return [(x - y) * TW / 2, (x + y) * TH / 2]; }
function screenToTile(sx, sy) {
  const wx = (sx - view.ox) / view.s, wy = (sy - view.oy) / view.s;
  const a = wx / (TW / 2), b = wy / (TH / 2);
  const x = Math.round((a + b) / 2), y = Math.round((b - a) / 2);
  return x >= 0 && y >= 0 && x < state.width && y < state.height ? [x, y] : null;
}
function centerOn(x, y) {
  view.s = Math.max(0.55, Math.min(1.5, innerWidth / (TW * 8.5)));
  const [wx, wy] = worldOf(x, y);
  view.ox = innerWidth / 2 - wx * view.s;
  view.oy = innerHeight * 0.45 - wy * view.s;
}

// The whole board on screen, between the top bar and the watch bar (key F).
function fitBoard() {
  const w = state.width, h = state.height;
  const left = worldOf(0, h - 1)[0] - TW / 2, right = worldOf(w - 1, 0)[0] + TW / 2;
  const top = -TH, bottom = worldOf(w - 1, h - 1)[1] + TH / 2 + DEPTH;
  const availW = innerWidth * 0.96, availH = innerHeight - 230;
  view.s = Math.max(0.2, Math.min(availW / (right - left), availH / (bottom - top)));
  view.ox = innerWidth / 2 - (left + right) / 2 * view.s;
  view.oy = 130 + availH / 2 - (top + bottom) / 2 * view.s;
}

// ---------- drawing ----------

const stars = Array.from({ length: 160 }, () => [Math.random(), Math.random(), Math.random() * 1.4 + 0.3]);

function diamondPath(cx, cy, w = TW, h = TH) {
  ctx.beginPath();
  ctx.moveTo(cx, cy - h / 2); ctx.lineTo(cx + w / 2, cy); ctx.lineTo(cx, cy + h / 2); ctx.lineTo(cx - w / 2, cy); ctx.closePath();
}
function block(cx, cy, colors) {
  const [top, left, right] = colors;
  if (left) {
    ctx.fillStyle = left; ctx.beginPath();
    ctx.moveTo(cx - TW / 2, cy); ctx.lineTo(cx, cy + TH / 2); ctx.lineTo(cx, cy + TH / 2 + DEPTH); ctx.lineTo(cx - TW / 2, cy + DEPTH); ctx.fill();
    ctx.fillStyle = right; ctx.beginPath();
    ctx.moveTo(cx + TW / 2, cy); ctx.lineTo(cx, cy + TH / 2); ctx.lineTo(cx, cy + TH / 2 + DEPTH); ctx.lineTo(cx + TW / 2, cy + DEPTH); ctx.fill();
  }
  diamondPath(cx, cy); ctx.fillStyle = top; ctx.fill();
}
function mountain(cx, cy) {
  for (const [dx, dy, w, h] of [[-14, 4, 22, 30], [10, 2, 24, 36]]) {
    const bx = cx + dx, by = cy + dy;
    ctx.fillStyle = '#9a9a9a'; ctx.beginPath(); ctx.moveTo(bx - w / 2, by); ctx.lineTo(bx, by - h); ctx.lineTo(bx, by + 6); ctx.fill();
    ctx.fillStyle = '#6f6f6f'; ctx.beginPath(); ctx.moveTo(bx + w / 2, by); ctx.lineTo(bx, by - h); ctx.lineTo(bx, by + 6); ctx.fill();
    ctx.fillStyle = '#ffffff'; ctx.beginPath(); ctx.moveTo(bx - w / 6, by - h * 0.66); ctx.lineTo(bx, by - h); ctx.lineTo(bx + w / 6, by - h * 0.66); ctx.lineTo(bx, by - h * 0.6); ctx.fill();
  }
}
function trees(cx, cy) {
  for (const [dx, dy] of [[-22, 0], [-6, -8], [10, 2], [24, -4], [-4, 10], [14, 12]]) {
    const bx = cx + dx * 0.9, by = cy + dy * 0.8;
    ctx.fillStyle = '#1f7a2a'; ctx.beginPath(); ctx.moveTo(bx - 5, by + 4); ctx.lineTo(bx, by - 16); ctx.lineTo(bx + 5, by + 4); ctx.fill();
    ctx.fillStyle = '#2fa83a'; ctx.beginPath(); ctx.moveTo(bx - 5, by + 4); ctx.lineTo(bx, by - 16); ctx.lineTo(bx, by + 4); ctx.fill();
  }
}
function emoji(text, x, y, size) { ctx.font = `${size}px "Apple Color Emoji","Segoe UI Emoji",sans-serif`; ctx.fillText(text, x, y); }
function textOutlined(text, x, y, size, color = '#fff', weight = 600) {
  ctx.font = `italic ${weight} ${size}px "Avenir Next", Avenir, system-ui`; ctx.lineWidth = 3.5; ctx.strokeStyle = 'rgba(0,0,0,.75)';
  ctx.strokeText(text, x, y); ctx.fillStyle = color; ctx.fillText(text, x, y);
}
const KILL_COLOUR = '#ffd400';  // an attack that kills its target in one hit
function rings(cx, cy, color) {
  ctx.lineWidth = 3;
  for (const k of [1, 0.66, 0.33]) { diamondPath(cx, cy, TW * 0.78 * k, TH * 0.78 * k); ctx.strokeStyle = color; ctx.stroke(); }
}
function shield(x, y, text) {
  ctx.fillStyle = '#fff'; ctx.strokeStyle = '#222'; ctx.lineWidth = 1.5; ctx.beginPath();
  ctx.moveTo(x - 9, y - 10); ctx.lineTo(x + 9, y - 10); ctx.lineTo(x + 9, y + 2); ctx.lineTo(x, y + 10); ctx.lineTo(x - 9, y + 2); ctx.closePath();
  ctx.fill(); ctx.stroke();
  ctx.fillStyle = '#111'; ctx.font = 'bold 11px system-ui'; ctx.fillText(text, x, y - 1);
}

// Units of yours with something legal left to do this turn.
function activeUnits() {
  const ids = new Set();
  if (!myTurn()) return ids;
  for (const m of state.legal) if (m.ok && m.args.UnitId !== undefined) ids.add(m.args.UnitId);
  return ids;
}

function draw() {
  if (!state) return;
  const dpr = devicePixelRatio || 1;
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.fillStyle = '#000'; ctx.fillRect(0, 0, innerWidth, innerHeight);
  ctx.fillStyle = '#fff'; ctx.globalAlpha = 0.5;
  for (const [x, y, r] of stars) ctx.fillRect(x * innerWidth, y * innerHeight, r, r);
  ctx.globalAlpha = 1;
  ctx.setTransform(dpr * view.s, 0, 0, dpr * view.s, dpr * view.ox, dpr * view.oy);
  ctx.textAlign = 'center'; ctx.textBaseline = 'middle';

  const selUnit = sel?.layer === 'unit' ? tileAt(sel.x, sel.y)?.unit : null;
  const moveMarks = new Set(), attackMarks = new Map();  // attack target -> kills in one hit
  if (selUnit && selUnit.owner === state.human) for (const m of state.legal) {
    if (!same(origin(m), [sel.x, sel.y])) continue;
    if (m.cmd === 'Attack') attackMarks.set(target(m) + '', !!m.kills); else moveMarks.add(target(m) + '');
  }
  const active = activeUnits();
  const tiles = [...state.tiles].sort((a, b) => (a.x + a.y) - (b.x + b.y) || a.x - b.x);
  const labels = [];

  for (const t of tiles) {
    let [cx, cy] = worldOf(t.x, t.y);
    if (t.fog) {
      const shade = (t.x * 7 + t.y * 13) % 3;
      block(cx, cy, [['#e8eef8', '#dce5f3', '#cfdaee'][shade], '#b7c4dc', '#a6b5d1']);
      netOverlayTile(t, cx, cy);
      continue;
    }
    const terr = nameOf('terrainData', t.terrain);
    const isWater = terr === 'water' || terr === 'ocean';
    if (isWater) cy += DEPTH;
    block(cx, cy, TERRAIN[terr] || ['#888', '#666', '#555']);
    if (isWater) { diamondPath(cx, cy); ctx.strokeStyle = 'rgba(255,255,255,.12)'; ctx.lineWidth = 1; ctx.stroke(); }

    // Territory: a thick dashed border in the owner's colour along edges facing other owners.
    if (t.owner && t.owner !== 255) {
      const edges = [[[0, -1], [cx, cy - TH / 2], [cx + TW / 2, cy]], [[1, 0], [cx + TW / 2, cy], [cx, cy + TH / 2]],
                     [[0, 1], [cx, cy + TH / 2], [cx - TW / 2, cy]], [[-1, 0], [cx - TW / 2, cy], [cx, cy - TH / 2]]];
      ctx.strokeStyle = playerColor(t.owner); ctx.lineWidth = 5; ctx.setLineDash([7, 5]);
      for (const [[dx, dy], a, b] of edges) {
        const n = tileAt(t.x + dx, t.y + dy);
        if (n && !n.fog && n.owner === t.owner) continue;
        ctx.beginPath(); ctx.moveTo(a[0] + (b[0] - a[0]) * 0.04, a[1] + (b[1] - a[1]) * 0.04);
        ctx.lineTo(a[0] + (b[0] - a[0]) * 0.96, a[1] + (b[1] - a[1]) * 0.96); ctx.stroke();
      }
      ctx.setLineDash([]);
    }
    netOverlayTile(t, cx, cy);
    if (terr === 'mountain') mountain(cx, cy);
    if (terr === 'forest' || terr === 'mangrove') trees(cx, cy);
    if (t.road) { ctx.fillStyle = '#c9a46a'; diamondPath(cx, cy, 26, 13); ctx.fill(); }
    const imp = t.improvement && nameOf('improvementData', t.improvement.type);
    if (t.resource && !(imp && imp !== 'road')) emoji(RESOURCE[nameOf('resourceData', t.resource)] || '•', cx + (terr === 'forest' ? 18 : 0), cy - 4, 22);
    if (imp === 'city') {
      const village = !t.owner, lvl = t.improvement.level;
      emoji(village ? '🛖' : (t.capitalOf ? '🏯' : '🏘️'), cx, cy - 8, village ? 30 : 30 + Math.min(lvl, 6) * 3);
      if (!village) labels.push([t, cx, cy]);
    } else if (imp && BUILT[imp]) {
      emoji(BUILT[imp], cx, cy - 6, 26);
    } else if (imp && imp.startsWith('monument')) emoji('🗿', cx, cy - 6, 26);

    const key = [t.x, t.y] + '';
    if (moveMarks.has(key)) rings(cx, cy, '#3aa0ff');
    if (attackMarks.has(key)) rings(cx, cy, attackMarks.get(key) ? KILL_COLOUR : '#ff4b3a');
    if (sel && sel.x === t.x && sel.y === t.y) {
      diamondPath(cx, cy); ctx.lineWidth = 3; ctx.strokeStyle = '#fff'; ctx.shadowColor = '#fff'; ctx.shadowBlur = 12; ctx.stroke(); ctx.shadowBlur = 0;
    }
    if (t.unit) {
      const u = t.unit, uy = cy - 20;
      const dim = u.owner === state.human && myTurn() && !active.has(u.id);
      ctx.globalAlpha = dim ? 0.55 : 1;
      ctx.beginPath(); ctx.ellipse(cx, cy + 2, 18, 8, 0, 0, Math.PI * 2); ctx.fillStyle = 'rgba(0,0,0,.25)'; ctx.fill();
      ctx.beginPath(); ctx.arc(cx, uy, 17, 0, Math.PI * 2); ctx.fillStyle = playerColor(u.owner); ctx.fill();
      ctx.lineWidth = 3; ctx.strokeStyle = '#fff'; ctx.stroke();
      emoji(UNIT_ICON[nameOf('unitData', u.type)] || '❓', cx, uy + 1, 19);
      shield(cx - 22, uy - 14, Math.round(u.hp / 10));
      if (u.passenger) emoji('⛵', cx + 20, uy + 12, 14);
      ctx.globalAlpha = 1;
      if (!dim && u.owner === state.human && myTurn()) {  // can still act
        ctx.beginPath(); ctx.arc(cx + 17, uy - 14, 5, 0, Math.PI * 2); ctx.fillStyle = '#ffcf3f'; ctx.fill();
      }
    }
  }
  // City labels on top: name, star output, population towards the next level.
  for (const [t, cx, cy] of labels) {
    const imp = t.improvement;
    const text = `${t.capitalOf ? '👑 ' : ''}${imp.name}  ★${imp.stars}`;
    ctx.font = 'italic 600 15px "Avenir Next", Avenir, system-ui';
    const w = ctx.measureText(text).width + 18, ly = cy + 16;
    ctx.fillStyle = playerColor(t.owner); ctx.globalAlpha = 0.85; ctx.fillRect(cx - w / 2, ly - 11, w, 22); ctx.globalAlpha = 1;
    textOutlined(text, cx, ly, 15);
    // Progress towards the next level is ImprovementState.xp: the city levels up once xp > level
    // (ImprovementDataExtensions.ShouldLevelUp), then xp -= the new level. `population` is the city's
    // total size and never resets.
    const need = imp.level + 1, segW = Math.min(16, 64 / need), bw = segW * need;
    for (let i = 0; i < need; i++) {
      ctx.fillStyle = i < imp.xp ? '#3aa0ff' : 'rgba(255,255,255,.85)';
      ctx.fillRect(cx - bw / 2 + i * segW + 1, ly + 14, segW - 2, 7);
    }
    if (imp.xp < 0) { ctx.fillStyle = '#ff4b3a'; ctx.fillRect(cx - bw / 2, ly + 14, segW * Math.min(-imp.xp, need), 7); }
  }
}

// ---------- panels ----------

function actButton(icon, label, idx, { cost, ok = true, cls = '' } = {}) {
  const d = document.createElement('div');
  d.className = `act ${cls} ${ok ? '' : 'off'}`;
  d.innerHTML = `${cost !== undefined ? `<div class="cost">${cost}</div>` : ''}<div class="disc">${icon}</div><span>${label}</span>`;
  if (ok) d.onclick = () => play(idx);
  return d;
}

function showCard(titleText, text, buttons) {
  $('cardtitle').textContent = titleText; $('cardtext').textContent = text;
  $('actions').replaceChildren(...buttons);
  $('card').style.display = 'block';
}

function renderCard() {
  if (!sel) { $('card').style.display = 'none'; return; }
  const t = tileAt(sel.x, sel.y);
  if (!t || t.fog) { showCard('Unexplored', 'Move a unit next to it to explore.', []); return; }
  const terr = nameOf('terrainData', t.terrain);
  if (sel.layer === 'unit' && t.unit) {
    const u = t.unit, uname = nameOf('unitData', u.type), ud = data.unitData[uname] || {};
    const mine = u.owner === state.human;
    const moves = legalWith(m => m.cmd === 'Move' && same(origin(m), [t.x, t.y])).length;
    const attacks = legalWith(m => m.cmd === 'Attack' && same(origin(m), [t.x, t.y])).length;
    let text = mine
      ? (moves && attacks ? 'Select a blue mark to move or a red mark to attack.' : moves ? 'Select a blue mark to move.'
        : attacks ? 'Select a red mark to attack.' : myTurn() ? 'This unit has no moves left this turn.' : 'Waiting for your turn.')
      : `${tribeName(u.owner)} unit.`;
    if (mine && legalWith(m => m.cmd === 'Attack' && m.kills && same(origin(m), [t.x, t.y])).length)
      text += ' Yellow marks: the attack kills in one hit.';
    text += `   ❤️ ${u.hp / 10}/${u.maxHp / 10}  ⚔️ ${ud.attack / 10}  🛡️ ${ud.defence / 10}  👣 ${ud.movement}  🎯 ${ud.range}`;
    const buttons = mine ? legalWith(m => m.args.UnitId === u.id && m.cmd !== 'Move' && m.cmd !== 'Attack').map(([m, i]) => {
      const [icon, label] = UNIT_ACTION[m.cmd] || ['•', m.cmd.replace(/([a-z])([A-Z])/g, '$1 $2')];
      return actButton(icon, m.cmd === 'Disband' ? `Disband ${title(uname)}` : label, i,
        { cls: m.cmd === 'Disband' ? 'danger' : 'unit', ok: m.ok });
    }) : [];
    // upgrades (a boat into a Scout, Rammer or Bomber) name the tile and the new type, not the unit
    if (mine) for (const [m, i] of legalWith(m => m.cmd === 'Upgrade' && same(where(m), [t.x, t.y]))) {
      const n = nameOf('unitData', m.args.Type);
      buttons.push(actButton(UNIT_ICON[n] || '⬆️', `Upgrade to ${title(n)}`, i, { cost: m.cost, ok: m.ok, cls: 'unit' }));
    }
    showCard(`${tribeName(u.owner)} ${title(uname)}`, text, buttons);
    return;
  }
  const imp = t.improvement && nameOf('improvementData', t.improvement.type);
  const res = t.resource && nameOf('resourceData', t.resource);
  const here = legalWith(m => !['CityReward', 'Move', 'Attack', 'Research', 'EndTurn', 'PeaceRequestResponse', 'Upgrade'].includes(m.cmd)
    && m.args.UnitId === undefined && same(where(m), [t.x, t.y]));
  const buttons = here.map(([m, i]) => {
    if (m.cmd === 'Train') { const n = nameOf('unitData', m.args.Type); return actButton(UNIT_ICON[n] || '❓', title(n), i, { cost: m.cost, ok: m.ok, cls: 'unit' }); }
    if (m.cmd === 'Build') { const n = nameOf('improvementData', m.args.Type); return actButton(ACTION_ICON[n] || BUILT[n] || '🔨', title(n), i, { cost: m.cost, ok: m.ok }); }
    return actButton('•', m.cmd.replace(/([a-z])([A-Z])/g, '$1 $2'), i, { ok: m.ok });
  });
  let head = `${title(terr)}${res ? ', ' + title(res) : ''}`, text = '';
  if (imp === 'city' && t.owner) {
    const c = t.improvement;
    head = `${c.name}${t.capitalOf ? ' (capital)' : ''}`;
    text = `${tribeName(t.owner)} city · level ${c.level} · ${c.xp}/${c.level + 1} towards the next level · population ${c.population} · ★${c.stars} per turn`;
    if (t.owner === state.human) text += here.some(([m]) => m.cmd === 'Train') ? '. Train units here.' : t.unit ? '. Move the unit away to train here.' : '';
  } else if (imp === 'city') {
    head = 'Village'; text = 'Move a unit here and capture it to grow your empire.';
  } else if (imp === 'ruin') {
    head = 'Ruins'; text = 'Move a unit here and examine the ruins for a reward.';
  } else if (imp && imp !== 'road') {
    head = `${title(imp)}${res ? ', ' + title(res) : ''}`;
  } else if (here.some(([m]) => m.cmd === 'Build')) {
    text = res ? 'Extract this resource to upgrade your city.' : 'You can build here.';
  } else if (res && t.owner !== state.human) {
    text = 'Bring it inside your borders to use it.';
  } else if (res) {
    text = 'Research the right technology to use this resource.';
  }
  showCard(head, text, buttons);
}

function renderHud() {
  const p = me();
  $('score').textContent = p ? p.score.toLocaleString() : '0';
  $('stars').textContent = p ? p.stars : 0;
  $('income').textContent = p ? `+${p.income}` : '+0';
  $('turn').textContent = state.turn;
  const active = activeUnits().size;
  const affordable = legalWith(m => m.ok && ['Build', 'Train', 'Research'].includes(m.cmd)).length;
  const canEnd = state.legal.some(m => m.cmd === 'EndTurn');
  const choosing = state.legal.some(m => m.cmd === 'CityReward' || m.cmd === 'PeaceRequestResponse');
  $('endbtn').classList.toggle('off', !canEnd);
  $('endbtn').classList.toggle('pulse', canEnd && active === 0);
  let banner = '';
  if (state.watch) banner = watchBanner();
  else if (state.state === 4) banner = state.winner === state.human ? '🏆 Victory!' : 'Defeat. Open the menu to start a new game.';
  else if (!myTurn()) banner = 'Waiting…';
  else if (choosing) banner = 'Choose your reward';
  else if (active) banner = `Your turn · ${active} unit${active > 1 ? 's' : ''} can still act · ${affordable} things you can afford`;
  else banner = affordable ? `Your turn · ${affordable} things you can afford, or end your turn` : 'Your turn · nothing left to do, end your turn';
  $('banner').textContent = banner;
  if (state.error) toast(`Not allowed: ${state.error}`);
}

function renderChoices() {
  const rewards = legalWith(m => m.cmd === 'CityReward');
  const peace = legalWith(m => m.cmd === 'PeaceRequestResponse');
  if (rewards.length) {
    const at = rewards[0][0].args.Coordinates, t = tileAt(at[0], at[1]), c = t?.improvement;
    $('choicetitle').textContent = `${c?.name || 'City'} level up!`;
    const p = document.createElement('p');
    p.className = 'muted'; p.style.fontSize = '16px';
    p.textContent = `${c?.name || 'Your city'} upgraded to level ${c?.level ?? ''} and its production has increased. You also get to pick an additional reward:`;
    const row = document.createElement('div'); row.className = 'choice';
    for (const [m, i] of rewards) {
      const [icon, name, desc] = REWARD[m.args.Reward] || ['•', `Reward ${m.args.Reward}`, ''];
      const b = document.createElement('button');
      b.innerHTML = `<div style="font-size:40px">${icon}</div>${name}<div class="muted">${desc}</div>`;
      b.onclick = () => play(i); row.append(b);
    }
    $('choices').replaceChildren(p, row);
    $('choiceview').style.display = 'flex';
  } else if (peace.length) {
    $('choicetitle').textContent = 'Peace offered';
    const row = document.createElement('div'); row.className = 'choice';
    for (const [m, i] of peace) {
      const b = document.createElement('button'); b.textContent = m.args.Accepted ? '🕊️ Accept peace' : '⚔️ Decline';
      b.onclick = () => play(i); row.append(b);
    }
    $('choices').replaceChildren(row);
    $('choiceview').style.display = 'flex';
  } else {
    $('choiceview').style.display = 'none';
  }
}

function renderTechTree() {
  const p = me(), have = new Set(p.techs);
  const research = new Map(legalWith(m => m.cmd === 'Research').map(([m, i]) => [m.args.Type, i]));
  const node = (name, tier) => {
    const td = data.techData[name]; if (!td) return null;
    const idx = td.idx, d = document.createElement('div');
    const can = research.has(idx);
    d.className = `tech tier${tier} ${have.has(idx) ? 'have' : can ? 'can' : ''}`;
    d.innerHTML = `${title(name)}${have.has(idx) ? '' : `<span class="price">${state.techPrices[idx] ?? '?'}</span>`}`;
    if (can) d.onclick = () => { $('techview').style.display = 'none'; play(research.get(idx)); };
    return d;
  };
  const shown = new Set();
  const branches = (data.techData.basic.techUnlocks || []).map(t1 => {
    const col = document.createElement('div'); col.className = 'branch';
    const add = (n, tier) => { const e = node(n, tier); if (e) { col.append(e); shown.add(data.techData[n].idx); } };
    add(t1, 1);
    for (const t2 of data.techData[t1]?.techUnlocks || []) {
      add(t2, 2);
      for (const t3 of data.techData[t2]?.techUnlocks || []) add(t3, 3);
    }
    return col;
  });
  const others = [...research.keys()].filter(idx => !shown.has(idx));
  if (others.length) {
    const col = document.createElement('div'); col.className = 'branch';
    for (const idx of others) { const e = node(nameOf('techData', idx), 1); if (e) col.append(e); }
    branches.push(col);
  }
  $('tree').style.gridTemplateColumns = `repeat(${branches.length}, 150px)`;
  $('tree').replaceChildren(...branches);
}

function summarizeBot() {
  const counts = {};
  for (const l of state.log) {
    if (l.p === 255 || l.p === state.human) continue;
    let k = l.c.cmd;
    if (k === 'Train') k = `trained ${title(nameOf('unitData', l.c.args.Type))}`;
    else if (k === 'Build') k = `built ${title(nameOf('improvementData', l.c.args.Type))}`;
    else if (k === 'Research') k = `researched ${title(nameOf('techData', l.c.args.Type))}`;
    else if (['EndTurn', 'CityReward', 'Stay'].includes(k)) continue;
    else k = { Move: 'moved', Attack: 'attacked', Capture: 'captured' }[k] || k.toLowerCase();
    counts[k] = (counts[k] || 0) + 1;
  }
  const parts = Object.entries(counts).map(([k, n]) => n > 1 ? `${k} ×${n}` : k);
  const bot = state.players.find(p => p.id !== state.human && p.id !== 255);
  return parts.length ? `${bot ? tribeName(bot.id) : 'Bot'}: ${parts.join(', ')}` : '';
}

let toastTimer = null;
function toast(text) {
  if (!text) return;
  $('toast').textContent = text; $('toast').style.opacity = 1;
  clearTimeout(toastTimer); toastTimer = setTimeout(() => $('toast').style.opacity = 0, 5000);
}

function render() {
  draw(); renderHud(); renderCard(); renderChoices(); renderWatch(); renderNetTab();
  if ($('techview').style.display === 'flex') renderTechTree();
}

// ---------- the network panel: what the network is and what it is thinking (py/agent_server.py) ----------

const net = { info: null, stats: null, open: false, overlay: '', timer: null, error: '' };
const netUrl = what => `http://127.0.0.1:${state.agentStats}/${what}`;
const pct = x => `${(100 * x).toFixed(1)}%`;
async function netFetch() {
  if (!state?.agentStats) return;
  try {
    if (!net.info) net.info = await (await fetch(netUrl('info'))).json();
    net.stats = await (await fetch(netUrl('stats'))).json();
    net.error = '';
  } catch (e) { net.error = `The network's stats server (port ${state.agentStats}) isn't answering.`; }
  if (net.open) renderNetBody();
  if (net.overlay) draw();
  renderNetTab();
}
function netPoll() {
  clearInterval(net.timer); net.timer = null;
  if (state?.agentStats && (net.open || net.overlay)) { netFetch(); net.timer = setInterval(netFetch, 1500); }
}
function renderNetTab() {
  const on = !!state?.agentStats && !state.watch;
  $('netdrawer').style.display = on ? 'block' : 'none';
  if (!on) return;
  if (!net.info && !net.asked) { net.asked = true; netFetch(); }  // the tab names the checkpoint from the start
  const i = net.info, v = net.stats?.last?.value;
  const who = i ? (i.generation != null ? `generation ${i.generation} of run ${i.run.toUpperCase()}` : i.file) : state.opponent || 'our network';
  $('nettab').innerHTML = `🧠 Playing against <b>${who}</b>${i ? ` · ${i.file} · ${i.sha1}` : ''}`
    + `${v != null ? ` · it gives itself <b>${pct(v)}</b>` : ''} ${net.open ? '▼' : '▲'}`;
}
const CITY_REWARD = { 1: 'City wall', 2: 'Park', 3: 'Workshop', 4: 'Explorer', 5: 'Border growth', 6: 'Super unit', 7: 'Resources',
  8: 'Population growth', 11: 'Rebellion', 13: 'Poison' };  // the game's CityReward values
// A legal-list row from the agent (cmd, at, to, arg) as text.
function rowText(r) {
  const xy = c => c ? `(${c[0]},${c[1]})` : '';
  const named = { Train: 'unitData', Upgrade: 'unitData', Build: 'improvementData', Research: 'techData' }[r.cmd];
  const what = named ? `${r.cmd} ${title(nameOf(named, r.arg))}` : r.cmd === 'CityReward' ? `City reward: ${CITY_REWARD[r.arg] || r.arg}` : r.cmd;
  return `${what} ${xy(r.at)}${r.to ? ' → ' + xy(r.to) : ''}`.trim();
}
function netSpark(values, w = 300, h = 60, lo = 0, hi = 1, colour = '#7fdc7f') {
  if (values.length < 2) return '<span class="muted">(after a few decisions)</span>';
  const x = i => (w * i / (values.length - 1)).toFixed(1), y = v => (h - 2 - (h - 4) * (v - lo) / (hi - lo || 1)).toFixed(1);
  const mid = lo < 0.5 && hi > 0.5 ? `<line x1="0" x2="${w}" y1="${y(0.5)}" y2="${y(0.5)}" stroke="#39435a" stroke-dasharray="4 3"/>` : '';
  return `<svg viewBox="0 0 ${w} ${h}" style="width:100%;height:${h}px">${mid}<polyline fill="none" stroke="${colour}" stroke-width="1.6" `
    + `points="${values.map((v, i) => `${x(i)},${y(v)}`).join(' ')}"/></svg>`;
}
function renderNetBody() {
  const body = $('netbody'), i = net.info, s = net.stats, d = s?.choice;
  if (net.error) { body.innerHTML = `<div class="ncard">${net.error}</div>`; return; }
  if (!i) { body.innerHTML = '<div class="ncard">Loading…</div>'; return; }
  const cards = [];
  const evals = (i.evaluations || []).map(e => `<tr><td>vs ${e.vs === 'crazy' ? 'Crazy bot' : e.vs.replace('gen', 'generation ').replace(/ 0+(\d)/, ' $1')}</td>`
    + `<td>${e.wins}/${e.games} (${pct(e.rate)}, ${pct(e.ci[0])}–${pct(e.ci[1])})</td><td>${e.mean_turns.toFixed(1)} turns</td></tr>`).join('');
  const L = i.learning;
  cards.push(`<div class="ncard"><h4>WHICH NETWORK</h4><table>
    <tr><th>Checkpoint</th><td>${i.path} (sha1 ${i.sha1}${i.trained ? `, trained ${i.trained}` : ''})</td></tr>
    ${i.generation != null ? `<tr><th>Generation</th><td>${i.generation} of self-play run ${i.run.toUpperCase()}${i.rl_from ? `, trained from ${i.rl_from}` : ''}</td></tr>` : ''}
    <tr><th>Network</th><td>${i.parameters.toLocaleString()} parameters: transformer, width ${i.config.d}, ${i.config.layers} layers, ${i.config.heads} heads</td></tr>
    <tr><th>Plays</th><td>${i.temperature > 0 ? `sampled at temperature ${i.temperature}` : 'greedily (its top move)'}, on ${i.device}, seeing only its own fog of war</td></tr>
    ${i.selfcheck_max_logit_diff != null ? `<tr><th>Check</th><td>layer-by-layer pass matches the network within ${i.selfcheck_max_logit_diff.toExponential(1)}</td></tr>` : ''}
    </table>${evals ? `<h4 style="margin-top:8px">ITS EVALUATIONS (200 GAMES EACH, GREEDY)</h4><table>${evals}</table>` : ''}
    ${L ? `<p class="muted">Its learning step: ${L.step} of ${L.of} steps, KL ${L.kl}, entropy ${L.ent}, clip fraction ${L.clipfrac}, value loss ${L.value}.</p>` : ''}
    ${i.pool ? `<p class="muted">Its training games' opponents: ${i.pool.split(',').map(x => x.replace(/:(\d.*)$/, (m, v) => ` ${pct(+v)}`)).join(', ')}.</p>` : ''}
    ${i.run_settings ? `<p class="muted">Settings then: ${Object.entries(i.run_settings).map(([k, v]) => `${k} ${v}`).join(', ')}.</p>` : ''}</div>`);
  if (!d) cards.push('<div class="ncard"><h4>ITS LAST DECISION</h4>No decisions yet in this session: end your turn.</div>');
  else {
    const top = d.top.map(r => `<tr><td>${rowText(r)}</td><td style="width:42%"><span class="bar" style="width:${Math.max(2, 100 * r.p)}%"></span> ${pct(r.p)}</td></tr>`).join('');
    cards.push(`<div class="ncard"><h4>ITS LAST REAL CHOICE (TURN ${d.turn}, DECISION ${d.n + 1} THIS GAME)</h4>
      <div><span class="big">${pct(d.value)}</span> <span class="muted">its own estimate that it wins (value head)</span></div>
      <p>Chose <b>${rowText(d.chosen)}</b> (${pct(d.chosen.p)}) out of ${d.legal} legal moves · policy entropy ${d.entropy} nats · ${d.ms} ms</p>
      <table>${top}</table>
      <p class="muted">Lone End Turns and other forced moves are skipped here.</p></div>`);
    const log = (s.turn_log || []).map(r => `<tr><td>${r.n + 1}</td><td>${rowText(r.move)}</td><td>${pct(r.move.p)}</td>`
      + `<td>${r.legal}</td><td>${pct(r.value)}</td></tr>`).join('');
    cards.push(`<div class="ncard"><h4>ITS LAST TURN, MOVE BY MOVE (TURN ${s.turn_log?.[0] ? s.last.turn : ''})</h4>
      <table><tr><th>#</th><th>Move</th><th>Its probability</th><th>Legal</th><th>Win estimate</th></tr>${log}</table></div>`);
    const h = s.history;
    cards.push(`<div class="ncard"><h4>THIS GAME, DECISION BY DECISION</h4>
      <div class="muted">Win estimate (dashed line: 50%)</div>${netSpark(h.map(x => x.value))}
      <div class="muted">Probability of the move it played</div>${netSpark(h.map(x => x.p), 300, 40, 0, 1, '#6fa8ff')}
      <div class="muted">Policy entropy (nats; high = unsure)</div>${netSpark(h.map(x => x.entropy), 300, 40, 0, Math.max(1, ...h.map(x => x.entropy)), '#e8c45a')}
      <p class="muted">${h.length} decisions, ${h.length ? (h.reduce((a, x) => a + x.ms, 0) / h.length).toFixed(0) : 0} ms each on average.</p></div>`);
    const me = playerColor(state.players.find(p => p.id !== state.human && p.id !== 255)?.id), you = playerColor(state.human);
    cards.push(`<div class="ncard"><h4>WHAT IT EXPECTS THE MAP TO LOOK LIKE AT THE END</h4>
      <div class="seg"><div style="width:${100 * d.own.mine}%;background:${me}"></div><div style="width:${100 * d.own.none}%;background:#3a4252"></div>
      <div style="width:${100 * d.own.theirs}%;background:${you}"></div></div>
      <p class="muted">Ownership head, averaged over the 196 tiles: itself ${pct(d.own.mine)}, nobody ${pct(d.own.none)}, you ${pct(d.own.theirs)}.
      Caution: self-play trains only the policy and value heads, so this head was last trained at generation 0 and drifts as the trunk changes.</p>
      <h4>BOARD OVERLAY</h4>
      ${[['', 'none'], ['owner', 'predicted final owner of each tile'], ['attn', 'where it looks (global token\'s attention)']]
        .map(([k, t]) => `<label><input type="radio" name="netov" value="${k}" ${net.overlay === k ? 'checked' : ''}> ${t}</label>`).join('<br>')}
      <p class="muted">Key N opens and closes this panel.</p></div>`);
    const rows = d.layers.map((l, k) => `<tr><td>${k + 1}</td><td>${l.resid}</td><td>${pct(l.attn)}</td><td>${pct(l.ffn)}</td>`
      + `<td><span class="bar" style="width:${50 * l.active}px;background:#7fdc7f"></span> ${pct(l.active)}</td>`
      + `<td><span class="bar" style="width:${50 * l.focus}px;background:#e8c45a"></span> ${pct(l.focus)}</td></tr>`).join('');
    cards.push(`<div class="ncard"><h4>ACTIVATIONS, LAYER BY LAYER (THIS DECISION)</h4>
      <table><tr><th>Layer</th><th>Stream size</th><th>Attention adds</th><th>FFN adds</th><th>FFN units on</th><th>Attention focus</th></tr>${rows}</table>
      <p class="muted">Stream size: mean length of the 197 token vectors after the layer. "Adds": how big the attention and
      feed-forward updates are, relative to the stream. FFN units on: share of the 1,024 hidden units per token with a positive
      input to the GELU. Attention focus: 0% = attention spread evenly over all tokens, 100% = on one token.</p></div>`);
  }
  const groups = (i.parameter_groups || []).map(g => `<tr><td>${g.name}</td><td>${g.count.toLocaleString()}</td><td>${g.rms}</td></tr>`).join('');
  cards.push(`<div class="ncard"><h4>PARAMETERS</h4><table><tr><th>Part</th><th>Count</th><th>RMS value</th></tr>${groups}</table></div>`);
  body.innerHTML = cards.join('');
  for (const r of body.querySelectorAll('input[name=netov]')) r.onchange = () => { net.overlay = r.value; netPoll(); draw(); };
}
// Draws the chosen overlay on one tile (tiles are y * width + x in the network's maps).
function netOverlayTile(t, cx, cy) {
  const d = net.overlay && net.stats?.last;
  if (!d) return;
  const k = t.y * state.width + t.x;
  if (net.overlay === 'owner') {
    const v = d.ownMap[k];
    if (Math.abs(v) < 0.05) return;
    ctx.globalAlpha = 0.6 * Math.abs(v);
    ctx.fillStyle = playerColor(v > 0 ? state.players.find(p => p.id !== state.human && p.id !== 255)?.id : state.human);
  } else {
    const v = d.attnMap[k];
    if (v < 0.04) return;
    ctx.globalAlpha = 0.7 * v; ctx.fillStyle = '#ffe14a';
  }
  diamondPath(cx, cy); ctx.fill(); ctx.globalAlpha = 1;
}
$('nettab').onclick = () => {
  net.open = !net.open;
  $('netdrawer').classList.toggle('open', net.open);
  if (net.open) renderNetBody();
  renderNetTab(); netPoll();
};

// ---------- watch mode: replaying a recorded game ----------

let games = null, autoplay = null, watchMeta = null;
const cmdText = l => {
  const a = l.c.args, c = l.c.cmd;
  const what = c === 'Train' ? `trains ${title(nameOf('unitData', a.Type))}` : c === 'Build' ? `builds ${title(nameOf('improvementData', a.Type))}`
    : c === 'Research' ? `researches ${title(nameOf('techData', a.Type))}` : c === 'EndTurn' ? 'ends the turn' : c.toLowerCase();
  return `${tribeName(l.p) || 'Player ' + l.p} ${what}${a.To ? ' → ' + a.To : a.Target ? ' → ' + a.Target : a.Coordinates ? ' @ ' + a.Coordinates : ''}`;
};
function watchBanner() {
  const ps = state.players.filter(p => p.id === 1 || p.id === 2);
  const who = id => watchMeta?.players?.[id] ? ` (${watchMeta.players[id]})` : '';
  const side = p => `${tribeName(p.id)}${who(p.id)} ★${p.stars} +${p.income}`;
  if (state.state === 4) return `🏆 ${tribeName(state.winner)} won · ${ps.map(side).join('  vs  ')}`;
  return ps.map(side).join('  vs  ');
}
function renderWatch() {
  const on = !!state.watch;
  $('watchbar').style.display = on ? 'flex' : 'none';
  $('rightbar').style.display = on ? 'none' : 'flex';
  $('logbtn').style.display = on ? 'none' : '';
  if (!on) return;
  const w = state.watch, last = state.log.length ? state.log[state.log.length - 1] : null;
  const turnNo = w.turnStarts.filter(t => t <= w.at).length;
  $('winfo').textContent = `${w.path} · move ${w.at - w.base}/${w.total - w.base} · player turn ${turnNo}/${w.turnStarts.length} · game turn ${state.turn}`
    + (last ? ` · last: ${cmdText(last)}` : '');
  $('wview').value = String(w.viewer);
  $('wplay').classList.toggle('on', !!autoplay);
}
async function wstep(q) {
  state = await api(`/wstep?${q}`, 'POST');
  if (state.watch && state.watch.at >= state.watch.total) stopAutoplay();
  render();
}
function stopAutoplay() { clearInterval(autoplay); autoplay = null; }
function toggleAutoplay() {
  if (autoplay) { stopAutoplay(); render(); return; }
  const v = $('wspeed').value, byTurn = v === 'turn';
  autoplay = setInterval(() => { if (!busy) { busy = true; wstep(byTurn ? 'turn=1' : 'd=1').finally(() => busy = false); } },
    byTurn ? 1500 : 1000 / Number(v));
  render();
}
let gamesData = null;
// The lead per turn (network minus opponent, share of territory; py/rank_games.py) as a sparkline.
function spark(leads) {
  if (!leads || leads.length < 2) return '<svg class="spark"></svg>';
  const w = 90, h = 22, n = leads.length, x = i => (i / (n - 1) * w).toFixed(1), y = v => (h / 2 - v * h / 2).toFixed(1);
  const pts = leads.map((v, i) => `${x(i)},${y(v)}`).join(' ');
  return `<svg class="spark" viewBox="0 0 ${w} ${h}"><line x1="0" y1="${h / 2}" x2="${w}" y2="${h / 2}" stroke="#555" stroke-width="0.5"/>`
    + `<polyline points="${pts}" fill="none" stroke="#e8c45a" stroke-width="1.2"/></svg>`;
}
function renderGameList() {
  const d = gamesData;
  const row = (g, note) => {
    const r = document.createElement('div'); r.className = 'grow';
    const t = g.tribes || [];
    const name = i => title(nameOf('tribeData', t[i - 1]));
    const res = g.result || '';
    const sw = g.swing !== undefined ? `${g.lead_changes} lead changes, behind ${Math.round(100 * g.comeback)}%` : '';
    r.innerHTML = `<span class="res ${res}">${res}</span><span class="when">${g.time || ''}</span>${spark(g.leads)}<span class="swing">${sw}</span>`
      + `<span>${name(1)} (${g.players?.[1] || 'player 1'}) vs ${name(2)} (${g.players?.[2] || 'player 2'}), ${g.turns ?? '?'} turns`
      + ` <span class="muted">${g.path}</span>${note ? ` <span class="note">${note}</span>` : ''}</span>`;
    r.onclick = () => watchGame(g);
    return r;
  };
  const key = $('gsort').value, filt = $('gfilter').value, tag = $('gtag').value;
  const cmp = key === 'time' ? (a, b) => (b.time || '').localeCompare(a.time || '')
    : (a, b) => (b[key] ?? -1) - (a[key] ?? -1);
  const keep = g => (filt === 'all' || g.result === filt) && (!tag || g.tag === tag);
  const kids = [];
  if (d.sent.length) {
    const h = document.createElement('div'); h.className = 'ghead'; h.textContent = 'Sent by Claude'; kids.push(h);
    for (const g of d.sent) kids.push(row(g, g.note));
    const h2 = document.createElement('div'); h2.className = 'ghead'; h2.textContent = 'All recorded games'; kids.push(h2);
  }
  for (const g of d.games.filter(keep).sort(cmp).slice(0, 400)) kids.push(row(g));
  if (!kids.length) kids.push(Object.assign(document.createElement('p'), { textContent: 'No recorded games yet.' }));
  $('gamelist').replaceChildren(...kids);
}
async function openWatchList() {
  gamesData = await api('/games');
  const tags = [...new Set(gamesData.games.map(g => g.tag).filter(Boolean))].sort();
  const cur = $('gtag').value;
  $('gtag').replaceChildren(new Option('Every set', ''), ...tags.map(t => new Option(t, t)));
  $('gtag').value = tags.includes(cur) ? cur : '';
  renderGameList();
  $('menuview').style.display = 'none';
  $('watchview').style.display = 'flex';
}
for (const id of ['gsort', 'gfilter', 'gtag']) $(id).onchange = renderGameList;
async function watchGame(g) {
  stopAutoplay(); sel = null; centeredFor = null; watchMeta = g;
  state = await api(`/watch?path=${encodeURIComponent(g.path)}`, 'POST');
  if (g.jump) await jumpTo(g.jump.turn, g.jump.player);
  $('watchview').style.display = 'none';
  fitBoard();
  resize(); render();
}
// Seeks a watched game to the start of `player`'s turn `turn` (the turn starts the server found).
async function jumpTo(turn, player) {
  for (const ts of state.watch.turnStarts) {
    const s = await api(`/wstep?to=${ts}`, 'POST');
    if (s.turn > turn || (s.turn === turn && (!player || s.currentPlayer === player))) { state = s; return; }
    state = s;
  }
}
$('watchgames').onclick = openWatchList;
$('wlist').onclick = openWatchList;
$('wfirst').onclick = () => wstep('to=0');
$('wlast').onclick = () => wstep('to=1000000');
$('wprev').onclick = () => wstep('d=-1');
$('wnext').onclick = () => wstep('d=1');
$('wprevturn').onclick = () => wstep('turn=-1');
$('wnextturn').onclick = () => wstep('turn=1');
$('wplay').onclick = toggleAutoplay;
$('wspeed').onchange = () => { if (autoplay) { stopAutoplay(); toggleAutoplay(); } };
$('wview').onchange = async () => { state = await api(`/wview?v=${$('wview').value}`, 'POST'); render(); };
$('wexit').onclick = () => { stopAutoplay(); if (window.siteMenu) window.siteMenu(); else $('menuview').style.display = 'flex'; };
addEventListener('keydown', e => {
  if (!state?.watch || e.target.tagName === 'SELECT') return;
  const k = { ArrowRight: 'd=1', ArrowLeft: 'd=-1', ArrowDown: 'turn=1', ArrowUp: 'turn=-1', Home: 'to=0', End: 'to=1000000' }[e.key];
  if (k) { e.preventDefault(); wstep(k); }
  if (e.key === ' ') { e.preventDefault(); toggleAutoplay(); }
  if (e.key === 'f' || e.key === 'F') { fitBoard(); draw(); }
});

// ---------- actions ----------

// The public replay site (py/export_site.py) has no server: static.js answers these from JSON files.
async function api(path, method = 'GET') {
  if (window.staticApi) return window.staticApi(path, method);
  return (await fetch(path, { method })).json();
}

async function play(i) {
  if (busy) return;
  busy = true;
  const m = state.legal[i];
  if (m?.cmd === 'EndTurn') $('banner').textContent = 'The bot is playing…';
  state = await api(`/do?i=${i}`, 'POST');
  busy = false;
  if (m?.cmd === 'Move' && sel) sel = { x: m.args.To[0], y: m.args.To[1], layer: 'unit' };  // follow the unit
  if (m?.cmd === 'EndTurn') { sel = null; toast(summarizeBot()); }
  const t = sel && tileAt(sel.x, sel.y);
  if (sel?.layer === 'unit' && !t?.unit) sel = { ...sel, layer: 'tile' };
  render();
}

function onTileClick(x, y) {
  const t = tileAt(x, y);
  if (sel?.layer === 'unit' && !(sel.x === x && sel.y === y)) {
    const k = state.legal.findIndex(m => (m.cmd === 'Move' || m.cmd === 'Attack') && same(origin(m), [sel.x, sel.y]) && same(target(m), [x, y]));
    if (k >= 0) { play(k); return; }
  }
  if (t && !t.fog && t.unit) {
    sel = (sel && sel.x === x && sel.y === y && sel.layer === 'unit') ? { x, y, layer: 'tile' } : { x, y, layer: 'unit' };
  } else {
    sel = (sel && sel.x === x && sel.y === y) ? null : { x, y, layer: 'tile' };
  }
  render();
}

// Pan with drag, zoom with wheel or pinch; a press without movement is a click.
const pointers = new Map();
let dragStart = null, pinchStart = null;
canvas.addEventListener('pointerdown', e => {
  canvas.setPointerCapture(e.pointerId);
  pointers.set(e.pointerId, [e.clientX, e.clientY]);
  if (pointers.size === 1) dragStart = { x: e.clientX, y: e.clientY, ox: view.ox, oy: view.oy, moved: false };
  if (pointers.size === 2) {
    const [a, b] = [...pointers.values()];
    pinchStart = { d: Math.hypot(a[0] - b[0], a[1] - b[1]), s: view.s, ox: view.ox, oy: view.oy, cx: (a[0] + b[0]) / 2, cy: (a[1] + b[1]) / 2 };
  }
});
canvas.addEventListener('pointermove', e => {
  if (!pointers.has(e.pointerId)) return;
  pointers.set(e.pointerId, [e.clientX, e.clientY]);
  if (pointers.size === 2 && pinchStart) {
    const [a, b] = [...pointers.values()];
    zoomAt(pinchStart.cx, pinchStart.cy, pinchStart.s * Math.hypot(a[0] - b[0], a[1] - b[1]) / pinchStart.d, pinchStart);
    if (dragStart) dragStart.moved = true;
    return;
  }
  if (!dragStart) return;
  const dx = e.clientX - dragStart.x, dy = e.clientY - dragStart.y;
  if (Math.hypot(dx, dy) > 6) dragStart.moved = true;
  if (dragStart.moved) { canvas.classList.add('dragging'); view.ox = dragStart.ox + dx; view.oy = dragStart.oy + dy; draw(); }
});
canvas.addEventListener('pointerup', e => {
  pointers.delete(e.pointerId);
  canvas.classList.remove('dragging');
  if (pointers.size < 2) pinchStart = null;
  if (dragStart && !dragStart.moved && pointers.size === 0) {
    const hit = screenToTile(e.clientX, e.clientY);
    if (hit) onTileClick(hit[0], hit[1]);
    else { sel = null; render(); }
  }
  if (pointers.size === 0) dragStart = null;
});
function zoomAt(px, py, s, from = view) {
  s = Math.max(0.35, Math.min(2.5, s));
  const fs = from.s, fox = from.ox, foy = from.oy;
  view.ox = px - (px - fox) * s / fs; view.oy = py - (py - foy) * s / fs; view.s = s;
  draw();
}
canvas.addEventListener('wheel', e => { e.preventDefault(); zoomAt(e.clientX, e.clientY, view.s * Math.exp(-e.deltaY * 0.0015)); }, { passive: false });
addEventListener('resize', resize);
addEventListener('keydown', e => {
  if (e.key === 'Escape') { sel = null; for (const o of ['techview', 'logview', 'menuview', 'watchview']) $(o).style.display = 'none'; render(); }
  if (e.key === 'n' && state?.agentStats && !e.metaKey && !e.ctrlKey) $('nettab').click();
  if ((e.key === 'e' || e.key === 'Enter') && myTurn() && $('choiceview').style.display !== 'flex') {
    const k = state.legal.findIndex(m => m.cmd === 'EndTurn'); if (k >= 0) play(k);
  }
});

$('closecard').onclick = () => { sel = null; render(); };
$('endbtn').onclick = () => { const k = state.legal.findIndex(m => m.cmd === 'EndTurn'); if (k >= 0) play(k); };
$('techbtn').onclick = () => { renderTechTree(); $('techview').style.display = 'flex'; };
$('menubtn').onclick = () => {
  if (window.siteMenu) return window.siteMenu();
  renderMenuStats();
  $('menuview').style.display = 'flex';
};
// This game's players in the menu: score and cities, as the game's own player list shows them.
function renderMenuStats() {
  const box = $('menustats');
  const ps = state?.players?.filter(p => p.id !== 255) || [];
  if (!ps.length || state.watch) { box.innerHTML = ''; return; }
  const who = p => p.id === state.human ? 'You' : state.opponent ? title(state.opponent) : 'Opponent';
  box.innerHTML = `<h2>This game</h2><table class="stats"><tr><th></th><th>Tribe</th><th>Score</th><th>Cities</th><th>Kills</th></tr>`
    + ps.map(p => `<tr><td>${who(p)}</td><td>${tribeName(p.id)}</td><td>${p.score.toLocaleString()}</td><td>${p.cities}</td><td>${p.kills}</td></tr>`).join('')
    + `</table><p class="muted">Turn ${state.turn}${state.state === 4 ? ' · game over' : ''}</p>`;
}
$('logbtn').onclick = () => {
  $('log').replaceChildren(...state.log.filter(l => l.p !== 255 && l.p !== state.human).map(l => {
    const d = document.createElement('div'); const a = l.c.args;
    const what = l.c.cmd === 'Train' ? `Train ${title(nameOf('unitData', a.Type))}` : l.c.cmd === 'Build' ? `Build ${title(nameOf('improvementData', a.Type))}`
      : l.c.cmd === 'Research' ? `Research ${title(nameOf('techData', a.Type))}` : l.c.cmd;
    d.textContent = `${what}${a.To ? ' → ' + a.To : a.Target ? ' → ' + a.Target : a.Coordinates ? ' @ ' + a.Coordinates : ''}`;
    return d;
  }));
  $('logview').style.display = 'flex';
};
for (const o of ['techview', 'logview', 'menuview', 'watchview']) $(o).addEventListener('click', e => { if (e.target === $(o)) $(o).style.display = 'none'; });
$('newgame').onclick = async () => {
  stopAutoplay();
  // (the map-type select isn't #map: that id is the board's canvas)
  const q = [['tribe', 'tribe'], ['opponent', 'opponent'], ['mode', 'mode'], ['difficulty', 'difficulty'], ['size', 'size'], ['map', 'mapType']]
    .map(([k, id]) => `${k}=${$(id).value}`).join('&');
  $('menuview').style.display = 'none'; sel = null; centeredFor = null;
  state = await api(`/new?${q}`, 'POST');
  state = await api(`/reveal?on=${$('reveal').value}`, 'POST');
  start();
};

function start() {
  const cap = state.tiles.find(t => t.capitalOf === state.human && !t.fog);
  const key = state.players.map(p => p.tribe).join(',') + state.width;
  if (centeredFor !== key && cap) { centerOn(cap.x, cap.y); centeredFor = key; }
  resize(); render();
}

(async () => {
  data = await api('/gamedata');
  for (const table of ['terrainData', 'resourceData', 'improvementData', 'unitData', 'techData', 'tribeData']) {
    names[table] = {};
    for (const [n, v] of Object.entries(data[table])) names[table][v.idx] = n;
  }
  const playable = Object.entries(data.tribeData).filter(([, v]) => v.idx >= 2).sort((a, b) => a[1].idx - b[1].idx);
  for (const id of ['tribe', 'opponent']) $(id).replaceChildren(...playable.map(([n, v]) => new Option(title(n), v.idx)));
  $('tribe').value = 7; $('opponent').value = 13;
  if (window.siteInit) { await window.siteInit(); return; }
  state = await api('/state');
  start();
  // #watch=<path>&speed=2.5&view=0 opens a recorded game and starts autoplay (speed in moves/s or "turn");
  // &turn=22&player=1 opens it at that player's turn (paused unless a speed is given)
  const h = new URLSearchParams(location.hash.slice(1));
  if (h.get('watch')) {
    const d = await api('/games');
    const g = [...d.sent, ...d.games].find(x => x.path === h.get('watch')) || { path: h.get('watch') };
    if (h.get('turn')) g.jump = { turn: +h.get('turn'), player: +h.get('player') || 0 };  // &turn=22&player=1
    if (h.get('speed')) $('wspeed').value = h.get('speed');
    await watchGame(g);
    if (h.get('view')) { $('wview').value = h.get('view'); state = await api(`/wview?v=${h.get('view')}`, 'POST'); render(); }
    $('menuview').style.display = 'none';
    if (!h.get('turn') || h.get('speed')) toggleAutoplay();
  }
})();
