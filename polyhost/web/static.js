// The public replay site (built by py/export_site.py): no game server, no game code. This file answers
// app.js's API calls from exported JSON files and shows the results panel.
//   data/gamedata.json        names, tribe colours, basic unit stats, the tech tree
//   data/games.json           the games on the site (with py/rank_games.py's lead per turn)
//   data/replays/<id>.json    one game: its first state, then what each move changed
//   data/results.json         every generation's evaluations
(() => {
  const cache = {};
  const getJSON = p => cache[p] || (cache[p] = fetch(p, { cache: 'no-cache' }).then(r => {
    if (!r.ok) throw new Error(`${p}: ${r.status}`);
    return r.json();
  }));
  const fileOf = path => `data/replays/${path.replace(/\//g, '__')}.json`;
  let cur = null;  // the game being watched: {path, g, states, k}

  function expand(g) {
    const states = [g.first];
    for (const d of g.deltas) {
      const prev = states[states.length - 1];
      const tiles = d.t ? prev.tiles.slice() : prev.tiles;
      for (const [i, t] of d.t || []) tiles[i] = t;
      states.push({ ...prev, tiles, players: d.p || prev.players, turn: d.turn ?? prev.turn,
        currentPlayer: d.cp ?? prev.currentPlayer, state: d.s ?? prev.state, log: d.log || [] });
    }
    return states;
  }
  function stateAt(k) {
    const n = cur.states.length;
    cur.k = Math.max(0, Math.min(n - 1, k));
    const g = cur.g;
    return { ...cur.states[cur.k], legal: [], human: cur.agent || 1,
      watch: { path: cur.path, at: g.base + cur.k, base: g.base, total: g.base + n - 1, viewer: 0, turnStarts: g.turnStarts } };
  }

  window.staticApi = async (path) => {
    const u = new URL(path, location.href), q = u.searchParams, ep = u.pathname.split('/').pop();
    if (ep === 'gamedata') return getJSON('data/gamedata.json');
    if (ep === 'games') { const d = await getJSON('data/games.json'); return { games: d.games, sent: d.featured }; }
    if (ep === 'watch') {
      const g = await getJSON(fileOf(q.get('path')));
      const list = await getJSON('data/games.json');
      const meta = [...list.featured, ...list.games].find(x => x.path === q.get('path')) || {};
      // the top bar shows the network's player
      cur = { path: q.get('path'), g, states: g.states || (g.states = expand(g)), k: 0, agent: meta.agent };
      return stateAt(0);
    }
    if (ep === 'wstep') {
      const g = cur.g, at = g.base + cur.k, last = g.base + cur.states.length - 1;
      if (q.has('d')) return stateAt(cur.k + Number(q.get('d')));
      if (q.has('to')) return stateAt(Number(q.get('to')) - g.base);
      if (q.has('turn')) {
        const ts = g.turnStarts;
        const target = Number(q.get('turn')) > 0 ? (ts.find(x => x > at) ?? last)
          : (ts.filter(x => x < at).pop() ?? g.base);
        return stateAt(target - g.base);
      }
    }
    if (ep === 'wview' || ep === 'state') return cur ? stateAt(cur.k) : null;
    throw new Error(`no ${path} on the static site`);
  };

  // ---------- the results panel ----------
  const pct = x => `${(100 * x).toFixed(1)}%`;
  function chart(gens) {
    const W = 560, H = 190, L = 40, B = 24, pts = k => gens.filter(g => g[k]).map(g => [g.gen, g[k].rate]);
    const maxGen = Math.max(1, ...gens.map(g => g.gen));
    const x = gen => L + (W - L - 10) * gen / maxGen, y = r => 8 + (H - B - 8) * (1 - r);
    const line = (k, colour) => {
      const p = pts(k);
      if (!p.length) return '';
      return `<polyline fill="none" stroke="${colour}" stroke-width="2" points="${p.map(([g, r]) => `${x(g)},${y(r)}`).join(' ')}"/>`
        + p.map(([g, r]) => `<circle cx="${x(g)}" cy="${y(r)}" r="3" fill="${colour}"/>`).join('');
    };
    const grid = [0, 0.25, 0.5, 0.75, 1].map(r => `<line x1="${L}" x2="${W - 10}" y1="${y(r)}" y2="${y(r)}" stroke="#333"/>`
      + `<text x="${L - 6}" y="${y(r) + 4}" fill="#999" font-size="11" text-anchor="end">${r * 100}%</text>`).join('');
    const ticks = gens.map(g => `<text x="${x(g.gen)}" y="${H - 6}" fill="#999" font-size="11" text-anchor="middle">${g.gen}</text>`).join('');
    return `<svg viewBox="0 0 ${W} ${H}" style="width:100%;max-width:${W}px">${grid}${ticks}${line('crazy', '#e8c45a')}${line('gen0', '#7fdc7f')}${line('prev', '#6fa8ff')}</svg>`
      + `<div class="muted" style="font-size:13px"><span style="color:#e8c45a">●</span> vs the game's hardest bot (Crazy) &nbsp; `
      + `<span style="color:#7fdc7f">●</span> vs generation 0 &nbsp; <span style="color:#6fa8ff">●</span> vs the previous generation &nbsp; (generation on the x axis)</div>`;
  }
  function cell(e) { return e ? `${e.wins}/${e.games} <span class="muted">(${pct(e.rate)}, ${pct(e.ci[0])}–${pct(e.ci[1])})</span>` : '<span class="muted">—</span>'; }
  async function showResults() {
    const r = await getJSON('data/results.json');
    const gens = r.generations;
    const rows = gens.slice().reverse().map(g => `<tr><td>${g.gen}${g.gen === r.current ? ' (current)' : ''}</td>`
      + `<td>${cell(g.crazy)}</td><td>${cell(g.prev)}</td><td>${cell(g.gen0)}</td></tr>`).join('');
    $('siteview').querySelector('.panel').innerHTML = `
      <h2>${r.title}</h2>
      <p>${r.summary}</p>
      <p class="muted">${r.model}</p>
      <div class="btnrow"><button class="btn" id="sitewatch" style="background:#5a3fa8">Watch games</button>
        ${r.featured ? `<button class="btn" id="sitebest">Watch the featured game</button>` : ''}</div>
      <h3>Generations</h3>${chart(gens)}
      <table class="res"><tr><th>Generation</th><th>vs Crazy</th><th>vs previous</th><th>vs generation 0</th></tr>${rows}</table>
      <p class="muted" style="font-size:13px">Each evaluation is 200 games: every map is played twice with the seats swapped; the
        network plays greedily, with no search, and sees only what its player would see (fog of war). Brackets: 95% Wilson
        interval. Updated ${r.updated}.</p>
      <p class="muted" style="font-size:12px">${r.disclaimer}</p>`;
    $('sitewatch').onclick = () => { $('siteview').style.display = 'none'; openWatchList(); };
    if (r.featured) $('sitebest').onclick = async () => {
      $('siteview').style.display = 'none';
      const d = await getJSON('data/games.json');
      await watchGame(d.featured[0]);
      if (!autoplay) toggleAutoplay();
    };
    $('siteview').style.display = 'flex';
  }
  window.siteMenu = () => { stopAutoplay(); render(); showResults(); };
  window.siteInit = async () => {
    const d = await getJSON('data/games.json');
    const h = new URLSearchParams(location.hash.slice(1));
    const path = h.get('watch') || d.featured[0]?.path || d.games[0]?.path;
    if (path) await watchGame([...d.featured, ...d.games].find(g => g.path === path) || { path });
    if (h.get('watch')) {
      if (h.get('speed')) $('wspeed').value = h.get('speed');
      toggleAutoplay();
    } else showResults();
  };
})();
