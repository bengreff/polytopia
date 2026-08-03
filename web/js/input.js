/*
 * input.js — pointer and keyboard. Turns raw events into TILE INDICES and hands them to the
 * app; it never decides whether something is legal, because that decision is exactly what
 * made v1 unplayable. Every discovered tile is clickable, full stop.
 *
 * All screen->tile conversion goes through projection.js.
 */

import * as P from './projection.js';

const DRAG_SLOP = 5; // px of movement before a press becomes a pan rather than a click

export function initInput(app, canvas) {
  let dragging = false;
  let moved = false;
  let lastX = 0;
  let lastY = 0;

  function localPoint(e) {
    const r = canvas.getBoundingClientRect();
    return { x: e.clientX - r.left, y: e.clientY - r.top };
  }

  function tileUnder(e) {
    const p = localPoint(e);
    const t = P.screenToTile(p.x, p.y);
    if (!P.inBounds(t.x, t.y)) return null;
    return { x: t.x, y: t.y, screen: { x: e.clientX, y: e.clientY } };
  }

  canvas.addEventListener('pointerdown', (e) => {
    if (e.button !== 0 && e.button !== 1) return;
    dragging = true;
    moved = false;
    lastX = e.clientX;
    lastY = e.clientY;
    canvas.setPointerCapture(e.pointerId);
  });

  canvas.addEventListener('pointermove', (e) => {
    if (dragging) {
      const dx = e.clientX - lastX;
      const dy = e.clientY - lastY;
      if (!moved && Math.abs(dx) + Math.abs(dy) > DRAG_SLOP) moved = true;
      if (moved) {
        P.panBy(dx, dy);
        lastX = e.clientX;
        lastY = e.clientY;
        app.draw();
        app.repositionFloaters();
      }
      return;
    }
    app.onHover(tileUnder(e));
  });

  function endDrag(e) {
    if (!dragging) return;
    dragging = false;
    try { canvas.releasePointerCapture(e.pointerId); } catch (_) { /* pointer already gone */ }
    if (moved) return;
    const t = tileUnder(e);
    if (t && app.state) app.onTileClick(app.state.tileIndex(t.x, t.y));
    else app.deselect();
  }

  canvas.addEventListener('pointerup', endDrag);
  canvas.addEventListener('pointercancel', () => { dragging = false; });
  canvas.addEventListener('pointerleave', () => app.onHover(null));
  canvas.addEventListener('contextmenu', (e) => { e.preventDefault(); app.deselect(); });

  canvas.addEventListener(
    'wheel',
    (e) => {
      e.preventDefault();
      const p = localPoint(e);
      P.zoomAt(p.x, p.y, e.deltaY < 0 ? 1.12 : 1 / 1.12);
      app.draw();
      app.repositionFloaters();
    },
    { passive: false }
  );

  window.addEventListener('keydown', (e) => {
    if (e.target && /^(INPUT|SELECT|TEXTAREA)$/.test(e.target.tagName)) return;
    switch (e.key) {
      case 'Escape':
        e.preventDefault();
        app.onEscape();
        break;
      case 'Enter':
        if (!app.popupOpen()) { e.preventDefault(); app.endTurn(); }
        break;
      case 't': case 'T':
        e.preventDefault();
        app.toggleTechTree();
        break;
      case 'f': case 'F':
        e.preventDefault();
        app.fit();
        break;
      case ' ':
        e.preventDefault();
        app.selectNextIdleUnit();
        break;
      default: break;
    }
  });
}
