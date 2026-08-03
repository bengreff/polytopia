/*
 * input.js — mouse and keyboard.
 *
 * The one rule that matters: a click is turned into a TILE INDEX, that index is
 * looked up in the engine's `legal` array, and the entry found there is sent
 * verbatim. If the lookup misses, nothing happens. No index is ever built by
 * arithmetic, and no click "means" anything the engine did not already offer.
 */

import * as P from './projection.js';

const DRAG_THRESHOLD = 4; // px of movement before a press counts as a pan

export function initInput(app, canvas) {
  let down = null;
  let dragging = false;

  function localPos(e) {
    const r = canvas.getBoundingClientRect();
    return { x: e.clientX - r.left, y: e.clientY - r.top };
  }

  function tileUnder(e) {
    const p = localPos(e);
    const t = P.screenToTile(p.x, p.y);
    return P.inBounds(t.x, t.y) ? t : null;
  }

  canvas.addEventListener('mousedown', (e) => {
    if (e.button === 2) return;
    down = { ...localPos(e), t: Date.now() };
    dragging = false;
  });

  window.addEventListener('mousemove', (e) => {
    const p = localPos(e);
    if (down) {
      const dx = p.x - down.x;
      const dy = p.y - down.y;
      if (!dragging && Math.hypot(dx, dy) > DRAG_THRESHOLD) dragging = true;
      if (dragging) {
        P.panBy(p.x - (down.lastX ?? down.x), p.y - (down.lastY ?? down.y));
        down.lastX = p.x;
        down.lastY = p.y;
        app.draw();
      }
      return;
    }
    const t = P.screenToTile(p.x, p.y);
    const hover = P.inBounds(t.x, t.y) ? t : null;
    const prev = app.ui.hover;
    const changed = (!!prev !== !!hover) || (hover && prev && (hover.x !== prev.x || hover.y !== prev.y));
    if (changed) {
      app.ui.hover = hover;
      app.draw();
    }
  });

  window.addEventListener('mouseup', () => {
    down = null;
  });

  canvas.addEventListener('mouseleave', () => {
    if (app.ui.hover) {
      app.ui.hover = null;
      app.draw();
    }
  });

  canvas.addEventListener('click', (e) => {
    if (dragging) {
      dragging = false;
      return;
    }
    const t = tileUnder(e);
    if (!t || !app.state) return;
    const i = app.state.tileIndex(t.x, t.y);

    // Membership test against the engine's legal set — never index construction.
    const entry = app.state.legalForTile(i);
    if (!entry) {
      app.flashIllegal(i);
      return;
    }
    app.applyAction(entry);
  });

  canvas.addEventListener('contextmenu', (e) => {
    e.preventDefault();
    app.deselect();
  });

  canvas.addEventListener(
    'wheel',
    (e) => {
      e.preventDefault();
      const p = localPos(e);
      P.zoomAt(p.x, p.y, e.deltaY < 0 ? 1.12 : 1 / 1.12);
      app.draw();
    },
    { passive: false }
  );

  window.addEventListener('keydown', (e) => {
    if (e.target instanceof HTMLInputElement || e.target instanceof HTMLSelectElement) return;
    switch (e.key) {
      case 'Escape':
        app.deselect();
        break;
      case 'Enter':
        app.endTurn();
        break;
      case 'f':
      case 'F':
        app.fit();
        break;
      case 'i':
      case 'I': {
        const box = document.getElementById('chk-ids');
        box.checked = !box.checked;
        app.ui.showIds = box.checked;
        app.draw();
        break;
      }
      default:
        break;
    }
  });
}
