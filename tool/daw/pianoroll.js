//
// Track tab: piano roll for one track (= one channel). Draw / select / move /
// resize / erase notes on a snap grid, velocity lane, CC & pitch-bend lanes,
// keyboard audition on the track's channel. Every edit goes through the
// project store (undo) and the app hot-swaps the playing song keeping the
// position.
//
import { GM_NAMES, DRUM_NAMES } from '../lib/gm.js';
import { scoped, canvas2d, clamp } from '../lib/ui.js';
import { barStarts, barTicksAt, beatTicksAt, gridTicks, projectEndTick, sortEvents } from './project.js';

const KEY_W = 52, RULER_H = 20, LANE_H = 70;
// A scroll bar a mouse can hit is not one a fingertip can. Keyed to how
// coarse the pointer is rather than to screen size, because that is what
// decides how big the target has to be -- a touchscreen laptop included.
const SB_H = matchMedia('(pointer: coarse)').matches ? 18 : 11;
const BLACK = new Set([1, 3, 6, 8, 10]);

export function initPianoRoll(root, ctx) {
  const $ = scoped(root);
  const { store, client, log } = ctx;
  const canvas = $('roll');
  let channel = 0;
  let tool = 'draw';
  let pxPerBeat = 60;
  let scrollTick = 0;
  let topNote = 84;           // note at the top edge
  let noteH = 12;
  let pitchFitted = false;
  let selection = new Set();  // indices into track.notes
  let drag = null;
  let raf = 0;
  let active = false;
  let hoverX = -1, hoverY = -1;
  let heldKey = null;

  const track = () => store.p.tracks.find((t) => t.channel === channel) || store.p.tracks[0];
  const laneMode = () => $('roll-lane').value;

  // ---------------- toolbar ----------------
  function refreshTrackList() {
    const sel = $('roll-track');
    sel.innerHTML = '';
    for (const t of store.p.tracks) {
      const o = document.createElement('option');
      o.value = t.channel;
      o.textContent = `${t.name} · ch ${t.channel + 1} · ${t.channel === 9 ? 'drums' : GM_NAMES[t.program]}`;
      sel.appendChild(o);
    }
    if (!store.p.tracks.some((t) => t.channel === channel)) channel = store.p.tracks[0]?.channel ?? 0;
    sel.value = channel;
  }
  $('roll-track').onchange = (e) => { setTrack(+e.target.value); ctx.ui.selectTrack(channel); };
  const setTool = (t) => {
    tool = t;
    for (const k of ['draw', 'select', 'erase']) $('tool-' + k).classList.toggle('active', k === t);
  };
  $('tool-draw').onclick = () => setTool('draw');
  $('tool-select').onclick = () => setTool('select');
  $('tool-erase').onclick = () => setTool('erase');

  function setTrack(ch) {
    channel = ch;
    selection.clear();
    $('roll-track').value = ch;
    // Frame the content.
    const t = track();
    if (t?.notes.length) {
      let lo = 127, hi = 0;
      for (const n of t.notes) { lo = Math.min(lo, n.note); hi = Math.max(hi, n.note); }
      topNote = clamp(hi + 4, 24, 127);
    } else topNote = ch === 9 ? 60 : 84;
  }

  // ---------------- geometry ----------------
  const geo = (w, h, dpr) => ({
    keyW: KEY_W * dpr, rulerH: RULER_H * dpr, laneH: LANE_H * dpr, sbH: SB_H * dpr,
    gridTop: RULER_H * dpr, laneTop: h - LANE_H * dpr,
    gridBottom: h - (LANE_H + SB_H) * dpr, nh: noteH * dpr,
  });

  // Horizontal scroll bar, in the strip between the grid and the lane. The
  // span runs to the end of the project plus a bar, or to the right edge of
  // the view if you have scrolled past that, so the thumb never pins itself.
  const hbar = (w, h, dpr) => {
    const G = geo(w, h, dpr);
    const visible = Math.max(1, ((w - G.keyW) / (pxPerBeat * dpr)) * store.p.ppq);
    const total = Math.max(projectEndTick(store.p) + store.p.ppq * 4, scrollTick + visible);
    const trackW = w - G.keyW;
    const thumbW = Math.max(30 * dpr, Math.min(trackW, (visible / total) * trackW));
    const span = Math.max(1, total - visible);
    return { x: G.keyW, y: G.gridBottom, h: G.sbH, trackW, thumbW, span,
             thumbX: G.keyW + (scrollTick / span) * (trackW - thumbW) };
  };
  const scrollTo = (x, grab, sb) =>
    { scrollTick = Math.max(0, Math.min(sb.span,
        ((x - grab - sb.x) / Math.max(1, sb.trackW - sb.thumbW)) * sb.span)); };
  const xOfTick = (tick, dpr) => KEY_W * dpr + ((tick - scrollTick) / store.p.ppq) * pxPerBeat * dpr;
  const tickOfX = (x, dpr) => scrollTick + ((x - KEY_W * dpr) / (pxPerBeat * dpr)) * store.p.ppq;
  const yOfNote = (note, dpr) => RULER_H * dpr + (topNote - note) * noteH * dpr;
  const noteOfY = (y, dpr) => topNote - Math.floor((y - RULER_H * dpr) / (noteH * dpr));
  const snapT = (tick) => { const s = ctx.ui.snapTicks(); return s ? Math.round(tick / s) * s : Math.round(tick); };
  const floorSnap = (tick) => { const s = ctx.ui.snapTicks(); return s ? Math.floor(tick / s) * s : Math.round(tick); };

  function noteAt(x, y, dpr) {
    const t = track();
    const tick = tickOfX(x, dpr), note = noteOfY(y, dpr);
    for (let i = t.notes.length - 1; i >= 0; i--) {
      const n = t.notes[i];
      if (n.note === note && tick >= n.tick && tick <= n.tick + n.dur) return i;
    }
    return -1;
  }

  // ---------------- drawing ----------------
  function draw() {
    raf = requestAnimationFrame(draw);
    if (!active) return;
    const { g, w, h, dpr } = canvas2d(canvas);
    const p = store.p;
    const t = track();
    if (!pitchFitted && h > 40 * dpr) {
      // At the desktop note height a phone opens on eight rows, which is most
      // of why this view felt like nothing but scrolling. Fit about twenty
      // rows once, at the size the canvas actually came up, then leave it to
      // the user's own zoom.
      noteH = clamp((h - (RULER_H + LANE_H + SB_H) * dpr) / dpr / 20, 7, 12);
      pitchFitted = true;
    }
    const G = geo(w, h, dpr);
    g.fillStyle = '#101318';
    g.fillRect(0, 0, w, h);
    if (!t) return;
    const pos = ctx.playback.positionTick();
    const viewEnd = tickOfX(w, dpr);
    if (ctx.playback.isPlaying() && (pos > viewEnd || pos < scrollTick)) scrollTick = Math.max(0, pos - p.ppq);

    // Row stripes + key names.
    const bottomNote = noteOfY(G.gridBottom, dpr);
    const st = ctx.playback.status();
    const lit = new Set(String((st?.activeNotes || [])[channel] || '').split(',').filter(Boolean).map(Number));
    for (let n = topNote; n >= Math.max(0, bottomNote); n--) {
      const y = yOfNote(n, dpr);
      if (y > G.gridBottom) break;
      const black = BLACK.has(n % 12);
      g.fillStyle = black ? 'rgba(255,255,255,0.025)' : 'rgba(255,255,255,0.055)';
      g.fillRect(G.keyW, y, w - G.keyW, G.nh);
      // key
      g.fillStyle = lit.has(n) ? '#64b5f6' : (heldKey === n ? '#4a90d9' : (black ? '#1a1a1e' : '#e8e6e1'));
      g.fillRect(0, y, G.keyW - 2 * dpr, G.nh - 1 * dpr);
      if (n % 12 === 0 || channel === 9) {
        g.fillStyle = black ? '#ccc' : '#333';
        g.font = `${9 * dpr}px ui-monospace`;
        const label = channel === 9 ? (DRUM_NAMES[n] || '').slice(0, 9) : `C${Math.floor(n / 12) - 1}`;
        g.fillText(label, 3 * dpr, y + G.nh - 3 * dpr);
      }
    }
    // Grid: bars / beats / snap.
    const end = projectEndTick(p);
    const bars = barStarts(p, Math.max(end, viewEnd) + barTicksAt(p, 0));
    const snap = ctx.ui.snapTicks();
    g.font = `${10 * dpr}px ui-monospace`;
    for (let i = 0; i < bars.length; i++) {
      const bt = bars[i];
      const x = xOfTick(bt, dpr);
      if (x > w) break;
      if (snap && (snap / p.ppq) * pxPerBeat * dpr > 6 * dpr) {
        for (let s = bt + snap; s < bt + barTicksAt(p, bt); s += snap) {
          const sx = xOfTick(s, dpr);
          if (sx < G.keyW || sx > w) continue;
          g.strokeStyle = 'rgba(255,255,255,0.05)';
          g.beginPath(); g.moveTo(sx, G.gridTop); g.lineTo(sx, G.gridBottom); g.stroke();
        }
      }
      const beat = beatTicksAt(p, bt);
      for (let b = bt + beat; b < bt + barTicksAt(p, bt); b += beat) {
        const bx = xOfTick(b, dpr);
        if (bx < G.keyW) continue;
        g.strokeStyle = 'rgba(255,255,255,0.10)';
        g.beginPath(); g.moveTo(bx, G.gridTop); g.lineTo(bx, h); g.stroke();
      }
      if (x >= G.keyW) {
        g.strokeStyle = 'rgba(255,255,255,0.22)';
        g.beginPath(); g.moveTo(x, 0); g.lineTo(x, h); g.stroke();
        g.fillStyle = 'rgba(255,255,255,0.5)';
        g.fillText(String(i + 1), x + 3 * dpr, 13 * dpr);
      }
    }
    // Loop region.
    if (p.loop.enabled) {
      const x0 = xOfTick(p.loop.startTick, dpr), x1 = xOfTick(p.loop.endTick, dpr);
      g.fillStyle = 'rgba(100,181,246,0.25)';
      g.fillRect(Math.max(G.keyW, x0), 0, Math.max(0, x1 - Math.max(G.keyW, x0)), G.rulerH);
    }
    const xe = xOfTick(end, dpr);
    if (xe < w) { g.fillStyle = 'rgba(0,0,0,0.3)'; g.fillRect(Math.max(G.keyW, xe), G.gridTop, w - xe, G.gridBottom - G.gridTop); }

    // Notes.
    const others = p.tracks.filter((o) => o !== t);
    g.globalAlpha = 0.25;
    for (const o of others) for (const n of o.notes) {
      const x = xOfTick(n.tick, dpr), y = yOfNote(n.note, dpr);
      if (x > w || y < G.gridTop || y > G.gridBottom) continue;
      g.fillStyle = o.color;
      g.fillRect(Math.max(G.keyW, x), y + G.nh * 0.3, (n.dur / p.ppq) * pxPerBeat * dpr, G.nh * 0.4);
    }
    g.globalAlpha = 1;
    t.notes.forEach((n, i) => {
      const x = xOfTick(n.tick, dpr), y = yOfNote(n.note, dpr);
      const nw = Math.max(3 * dpr, (n.dur / p.ppq) * pxPerBeat * dpr);
      if (x + nw < G.keyW || x > w || y + G.nh < G.gridTop || y > G.gridBottom) return;
      const playing = ctx.playback.isPlaying() && pos >= n.tick && pos < n.tick + n.dur && lit.has(n.note);
      g.fillStyle = playing ? '#fff' : t.color;
      g.globalAlpha = 0.35 + 0.65 * (n.vel / 127);
      g.fillRect(Math.max(G.keyW, x), y + 1 * dpr, nw - (x < G.keyW ? G.keyW - x : 0), G.nh - 2 * dpr);
      g.globalAlpha = 1;
      if (selection.has(i)) {
        g.strokeStyle = '#fff';
        g.lineWidth = 1.5 * dpr;
        g.strokeRect(Math.max(G.keyW, x) + 0.5, y + 1.5 * dpr, nw - 1, G.nh - 3 * dpr);
      }
    });
    // Drag preview (new note).
    if (drag?.kind === 'new' && drag.dur > 0) {
      g.fillStyle = 'rgba(255,255,255,0.5)';
      g.fillRect(xOfTick(drag.tick, dpr), yOfNote(drag.note, dpr) + dpr, (drag.dur / p.ppq) * pxPerBeat * dpr, G.nh - 2 * dpr);
    }
    if (drag?.kind === 'box') {
      g.strokeStyle = 'rgba(255,255,255,0.6)';
      g.setLineDash([4 * dpr, 3 * dpr]);
      g.strokeRect(Math.min(drag.x0, drag.x1), Math.min(drag.y0, drag.y1), Math.abs(drag.x1 - drag.x0), Math.abs(drag.y1 - drag.y0));
      g.setLineDash([]);
    }

    // Lane.
    const laneTop = G.laneTop;
    g.fillStyle = '#0c0e12';
    g.fillRect(0, laneTop, w, G.laneH);
    g.strokeStyle = 'rgba(255,255,255,0.2)';
    g.beginPath(); g.moveTo(0, laneTop); g.lineTo(w, laneTop); g.stroke();
    const mode = laneMode();
    g.fillStyle = 'rgba(255,255,255,0.35)';
    g.font = `${10 * dpr}px ui-monospace`;
    g.fillText(mode === 'vel' ? 'velocity' : mode === 'bend' ? 'pitch bend' : 'CC ' + mode, 4 * dpr, laneTop + 12 * dpr);
    if (mode === 'vel') {
      t.notes.forEach((n, i) => {
        const x = xOfTick(n.tick, dpr);
        if (x < G.keyW || x > w) return;
        const bh = (n.vel / 127) * (G.laneH - 6 * dpr);
        g.fillStyle = selection.has(i) ? '#fff' : t.color;
        g.fillRect(x, laneTop + G.laneH - bh - 2 * dpr, Math.max(2 * dpr, 3 * dpr), bh);
      });
    } else if (mode === 'bend') {
      g.strokeStyle = '#e0b060';
      g.lineWidth = 1.5 * dpr;
      g.beginPath();
      let first = true, last = 0;
      const yb = (v) => laneTop + G.laneH / 2 - (v / 8192) * (G.laneH / 2 - 4 * dpr);
      for (const b of t.bends) {
        const x = xOfTick(b.tick, dpr);
        if (first) { g.moveTo(Math.max(G.keyW, x), yb(last)); first = false; }
        g.lineTo(x, yb(last)); g.lineTo(x, yb(b.value)); last = b.value;
      }
      g.lineTo(w, yb(last));
      g.stroke();
    } else {
      const cc = +mode;
      g.strokeStyle = '#e0b060';
      g.lineWidth = 1.5 * dpr;
      g.beginPath();
      let first = true, last = 0;
      const yc = (v) => laneTop + G.laneH - 3 * dpr - (v / 127) * (G.laneH - 8 * dpr);
      for (const c of t.ccs) {
        if (c.cc !== cc) continue;
        const x = xOfTick(c.tick, dpr);
        if (first) { g.moveTo(Math.max(G.keyW, x), yc(last)); first = false; }
        g.lineTo(x, yc(last)); g.lineTo(x, yc(c.val)); last = c.val;
        g.fillStyle = '#e0b060';
        g.fillRect(x - 1.5 * dpr, yc(c.val) - 1.5 * dpr, 3 * dpr, 3 * dpr);
      }
      if (!first) { g.lineTo(w, yc(last)); g.stroke(); }
    }

    // Scroll bar: the one-finger way to move along the song, and the only
    // on-screen sign of how long the song is and where in it you are.
    const sb = hbar(w, h, dpr);
    g.fillStyle = '#0a0c10';
    g.fillRect(0, sb.y, w, sb.h);
    g.strokeStyle = 'rgba(255,255,255,0.12)';
    g.lineWidth = dpr;
    g.beginPath(); g.moveTo(0, sb.y); g.lineTo(w, sb.y); g.stroke();
    g.fillStyle = drag?.kind === 'hscroll' ? '#7bc67b' : 'rgba(255,255,255,0.3)';
    g.fillRect(sb.thumbX, sb.y + 2 * dpr, sb.thumbW, sb.h - 4 * dpr);

    // Playhead.
    const px = xOfTick(pos, dpr);
    if (px >= G.keyW) {
      g.strokeStyle = '#7bc67b';
      g.lineWidth = 1.5 * dpr;
      g.beginPath(); g.moveTo(px, 0); g.lineTo(px, h); g.stroke();
    }
    // Hover cell.
    if (hoverX >= G.keyW && hoverY >= G.gridTop && hoverY < G.gridBottom && !drag) {
      const n = noteOfY(hoverY, dpr);
      g.fillStyle = 'rgba(255,255,255,0.06)';
      g.fillRect(G.keyW, yOfNote(n, dpr), w - G.keyW, G.nh);
    }
  }

  // ---------------- editing ----------------
  const commit = (label, fn, coalesce = false) => {
    store.mutate(label, (pp) => {
      const t = pp.tracks.find((x) => x.channel === channel);
      if (t) { fn(t); sortEvents(t); }
    }, { coalesce });
  };
  const defaultVel = () => +$('roll-vel').value || 100;

  canvas.addEventListener('contextmenu', (e) => e.preventDefault());
  // Live touch points, for the two-finger gestures below.
  const touches = new Map();
  let gesture = null, holdTimer = null;

  const gestureState = () => {
    const [a, b] = [...touches.values()];
    return {
      cx: (a.x + b.x) / 2, cy: (a.y + b.y) / 2,
      sx: Math.abs(a.x - b.x), sy: Math.abs(a.y - b.y),
    };
  };
  // Two fingers never hold an exactly constant distance, so a bare spread
  // ratio zooms a little on every pan -- and because the pan is measured in
  // the zoomed units, the grid slides out from under the fingers as it goes.
  // Ignore the first fifth of the change, and ignore an axis the fingers are
  // not meaningfully spread along.
  const zoomFactor = (now, start) => {
    if (start < 30 || now < 30) return 1;
    const r = now / start, dz = 1.2;
    return r > dz ? r / dz : r < 1 / dz ? r * dz : 1;
  };

  canvas.addEventListener('pointerdown', (e) => {
    const rect = canvas.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1;
    const x = (e.clientX - rect.left) * dpr, y = (e.clientY - rect.top) * dpr;
    const G = geo(canvas.width, canvas.height, dpr);
    if (e.pointerType === 'touch') {
      touches.set(e.pointerId, { x, y });
      if (touches.size === 2) {
        // Second finger down: abandon whatever the first one started and pan
        // instead. Nothing is committed until pointerup, so no edit is lost --
        // but a note the first finger set sounding has no pointerup coming,
        // and used to hang until something else happened to stop it.
        if (drag?.kind === 'new') client.noteOff(channel, drag.note);
        if (heldKey !== null) { client.noteOff(channel, heldKey); heldKey = null; }
        clearTimeout(holdTimer); holdTimer = null;
        drag = null;
        gesture = { ...gestureState(), scrollTick, topNote, pxPerBeat, noteH };
        return;
      }
      if (touches.size > 2) return;
    }
    canvas.setPointerCapture(e.pointerId);
    const t = track();
    if (!t) return;
    if (x < G.keyW && y >= G.gridTop && y < G.gridBottom) {   // keys: tap auditions, drag scrolls
      heldKey = noteOfY(y, dpr);
      client.noteOn(channel, heldKey, defaultVel());
      drag = { kind: 'key', y0: y, top0: topNote };
      return;
    }
    if (y >= G.gridBottom && y < G.laneTop) {   // scroll bar
      const sb = hbar(canvas.width, canvas.height, dpr);
      const onThumb = x >= sb.thumbX && x <= sb.thumbX + sb.thumbW;
      const grab = onThumb ? x - sb.thumbX : sb.thumbW / 2;   // tapping the track centres the thumb
      drag = { kind: 'hscroll', grab, sb };
      scrollTo(x, grab, sb);
      return;
    }
    if (y < G.rulerH) {  // seek
      ctx.playback.seekTick(snapT(Math.max(0, tickOfX(x, dpr))));
      drag = { kind: 'seek' };
      return;
    }
    if (y >= G.laneTop) {  // lane edit
      if (tool === 'erase' || e.button === 2) {   // remove points, don't paint
        drag = { kind: 'lane-erase' };
        laneErase(x, dpr);
        return;
      }
      if (e.shiftKey) {   // anchor a straight ramp; drawn on release
        drag = { kind: 'lane-ramp', from: { x, y } };
        return;
      }
      drag = { kind: 'lane', lastTick: null };
      laneEdit(x, y, dpr, G);
      return;
    }
    const tick = Math.max(0, tickOfX(x, dpr));
    const idx = noteAt(x, y, dpr);
    const erase = tool === 'erase' || e.button === 2;
    if (erase) {
      if (idx >= 0) { commit('erase note', (tr) => tr.notes.splice(idx, 1)); selection.clear(); }
      drag = { kind: 'erase' };
      return;
    }
    if (idx >= 0) {
      const n = t.notes[idx];
      if (e.pointerType === 'touch') {
        clearTimeout(holdTimer);
        holdTimer = setTimeout(() => {
          if (!drag || drag.moved) return;          // a drag became a move
          commit('erase note', (tr) => tr.notes.splice(idx, 1));
          selection.clear();
          client.noteOff(channel, n.note);
          drag = null;
          holdTimer = null;
        }, 450);
      }
      const xEnd = xOfTick(n.tick + n.dur, dpr);
      if (!e.shiftKey && !selection.has(idx)) selection = new Set([idx]);
      else if (e.shiftKey) selection.add(idx);
      const resizing = xEnd - x < 8 * dpr;
      drag = { kind: resizing ? 'resize' : 'move', startTick: tick, startNote: noteOfY(y, dpr),
               orig: [...selection].map((i) => ({ i, ...t.notes[i] })), moved: false };
      client.noteOn(channel, n.note, n.vel);
      setTimeout(() => client.noteOff(channel, n.note), 120);
      return;
    }
    if (tool === 'select') {
      if (!e.shiftKey) selection.clear();
      drag = { kind: 'box', x0: x, y0: y, x1: x, y1: y, dpr };
      return;
    }
    // Draw a new note.
    const start = floorSnap(tick);
    drag = { kind: 'new', tick: start, note: clamp(noteOfY(y, dpr), 0, 127), dur: ctx.ui.snapTicks() || store.p.ppq / 4 };
    client.noteOn(channel, drag.note, defaultVel());
  });
  canvas.addEventListener('pointermove', (e) => {
    const rect = canvas.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1;
    const x = (e.clientX - rect.left) * dpr, y = (e.clientY - rect.top) * dpr;
    hoverX = x; hoverY = y;
    if (e.pointerType === 'touch' && touches.has(e.pointerId)) {
      touches.set(e.pointerId, { x, y });
      if (gesture && touches.size === 2) {
        const now = gestureState();
        // The grid follows the fingers on BOTH axes: drag down and the notes
        // come down with you. The vertical half used to be the other way
        // round -- the view moved down, so the notes went up -- which made
        // the one gesture fight itself, since time already followed the hand.
        // Each axis zooms from its own spread, so a pinch can open the pitch
        // range out without stretching the bars.
        pxPerBeat = clamp(gesture.pxPerBeat * zoomFactor(now.sx, gesture.sx), 4, 400);
        noteH = clamp(gesture.noteH * zoomFactor(now.sy, gesture.sy), 5, 36);
        scrollTick = Math.max(0, gesture.scrollTick -
          ((now.cx - gesture.cx) / (pxPerBeat * dpr)) * store.p.ppq);
        const G = geo(canvas.width, canvas.height, dpr);
        topNote = clamp(Math.round(gesture.topNote + (now.cy - gesture.cy) / G.nh), 12, 127);
        return;
      }
    }
    if (!drag) return;
    if (holdTimer && (drag.moved || drag.kind === 'new')) { clearTimeout(holdTimer); holdTimer = null; }
    const G = geo(canvas.width, canvas.height, dpr);
    const tick = Math.max(0, tickOfX(x, dpr));
    switch (drag.kind) {
      case 'key': {
        // Sliding along the keys is the one-finger way to move through the
        // pitch range; a short slide still plays the keys it crosses.
        if (drag.panning || Math.abs(y - drag.y0) > 7 * dpr) {
          if (!drag.panning) {
            drag.panning = true;
            if (heldKey !== null) client.noteOff(channel, heldKey);
            heldKey = null;
          }
          topNote = clamp(Math.round(drag.top0 + (y - drag.y0) / G.nh), 12, 127);
          break;
        }
        const n = clamp(noteOfY(y, dpr), 0, 127);
        if (n !== heldKey) { client.noteOff(channel, heldKey); heldKey = n; client.noteOn(channel, n, defaultVel()); }
        break;
      }
      case 'hscroll': scrollTo(x, drag.grab, drag.sb); break;
      case 'seek': ctx.playback.seekTick(snapT(tick)); break;
      case 'lane': laneEdit(x, y, dpr, G); break;
      case 'lane-erase': laneErase(x, dpr); break;
      case 'lane-ramp': drag.to = { x, y }; break;
      case 'new': {
        const end = snapT(tick);
        drag.dur = Math.max(ctx.ui.snapTicks() || 1, end - drag.tick);
        break;
      }
      case 'erase': {
        const idx = noteAt(x, y, dpr);
        if (idx >= 0) commit('erase note', (tr) => tr.notes.splice(idx, 1), true);
        break;
      }
      case 'move': {
        const dt = snapT(tick) - snapT(drag.startTick);
        const dn = clamp(noteOfY(y, dpr), 0, 127) - drag.startNote;
        if (!dt && !dn && !drag.moved) return;
        drag.moved = true;
        applyDrag(drag.orig.map((o) => ({ i: o.i, tick: Math.max(0, o.tick + dt), note: clamp(o.note + dn, 0, 127), dur: o.dur, vel: o.vel })), 'move notes');
        break;
      }
      case 'resize': {
        const first = drag.orig[0];
        const newDur = Math.max(ctx.ui.snapTicks() || 1, snapT(tick) - first.tick);
        const scale = newDur / first.dur;
        drag.moved = true;
        applyDrag(drag.orig.map((o) => ({ i: o.i, tick: o.tick, note: o.note, vel: o.vel, dur: Math.max(1, Math.round(o.dur * scale)) })), 'resize notes');
        break;
      }
      case 'box': { drag.x1 = x; drag.y1 = y; break; }
      default: break;
    }
  });
  const endTouch = (e) => {
    if (e.pointerType !== 'touch') return;
    touches.delete(e.pointerId);
    if (touches.size < 2) gesture = null;
  };
  canvas.addEventListener('pointercancel', (e) => { endTouch(e); drag = null; });

  canvas.addEventListener('pointerup', (e) => {
    endTouch(e);
    if (holdTimer) { clearTimeout(holdTimer); holdTimer = null; }
    if (!drag) return;
    const d = drag;
    drag = null;
    switch (d.kind) {
      case 'key': if (heldKey !== null) client.noteOff(channel, heldKey); heldKey = null; break;
      case 'lane-ramp': {
        const dpr = window.devicePixelRatio || 1;
        if (d.to) laneRamp(d.from, d.to, dpr, geo(canvas.width, canvas.height, dpr));
        break;
      }
      case 'new': {
        client.noteOff(channel, d.note);
        commit('add note', (tr) => tr.notes.push({ tick: d.tick, dur: d.dur, note: d.note, vel: defaultVel() }));
        // Select the new note (after sort, find it).
        const t = track();
        selection = new Set([t.notes.findIndex((n) => n.tick === d.tick && n.note === d.note && n.dur === d.dur)]);
        break;
      }
      case 'box': {
        const dpr = d.dpr;
        const t0 = tickOfX(Math.min(d.x0, d.x1), dpr), t1 = tickOfX(Math.max(d.x0, d.x1), dpr);
        const nHi = noteOfY(Math.min(d.y0, d.y1), dpr), nLo = noteOfY(Math.max(d.y0, d.y1), dpr);
        track().notes.forEach((n, i) => {
          if (n.tick + n.dur >= t0 && n.tick <= t1 && n.note <= nHi && n.note >= nLo) selection.add(i);
        });
        break;
      }
      default: break;
    }
    store.endCoalesce?.();
  });
  // Live-drag application: coalesced into one undo entry per gesture.
  function applyDrag(updates, label) {
    commit(label, (tr) => {
      // Indices refer to the sorted order at gesture start; re-sorting happens
      // after, so map by identity through a temp array.
      const target = updates.map((u) => tr.notes[u.i]).filter(Boolean);
      updates.forEach((u, k) => { const n = target[k]; if (n) { n.tick = u.tick; n.note = u.note; n.dur = u.dur; n.vel = u.vel; } });
      // Rebuild selection indices after sort.
      const set = new Set(target);
      sortEvents(tr);
      selection = new Set(tr.notes.map((n, i) => (set.has(n) ? i : -1)).filter((i) => i >= 0));
    }, true);
  }
  // Delete the lane point nearest the cursor. Until this existed a drawn CC
  // could only be overwritten, never removed.
  function laneErase(x, dpr) {
    const mode = laneMode();
    if (mode === 'vel') return;            // velocity belongs to a note
    const near = (tick) => Math.abs(xOfTick(tick, dpr) - x) < 6 * dpr;
    if (mode === 'bend') {
      commit('erase bend', (tr) => { tr.bends = tr.bends.filter((b) => !near(b.tick)); }, true);
    } else {
      const cc = +mode;
      commit('erase cc', (tr) => { tr.ccs = tr.ccs.filter((c) => !(c.cc === cc && near(c.tick))); }, true);
    }
  }

  // A straight line between two points, written at grid resolution: the long
  // filter sweep, without clicking in every step of it.
  function laneRamp(a, b, dpr, G) {
    const mode = laneMode();
    if (mode === 'vel') return;
    const valAt = (y) => clamp(1 - (y - G.laneTop - 3 * dpr) / (G.laneH - 8 * dpr), 0, 1);
    let t0 = snapT(Math.max(0, tickOfX(a.x, dpr))), t1 = snapT(Math.max(0, tickOfX(b.x, dpr)));
    let f0 = valAt(a.y), f1 = valAt(b.y);
    if (t1 < t0) { [t0, t1] = [t1, t0]; [f0, f1] = [f1, f0]; }
    const step = ctx.ui.snapTicks() || gridTicks(store.p, '1/16');
    if (t1 <= t0 || !step) return;
    const pts = [];
    for (let tk = t0; tk <= t1; tk += step) {
      const f = f0 + (f1 - f0) * ((tk - t0) / (t1 - t0));
      pts.push({ tick: tk, f });
    }
    if (mode === 'bend') {
      commit('bend ramp', (tr) => {
        tr.bends = tr.bends.filter((bd) => bd.tick < t0 || bd.tick > t1);
        for (const pt of pts) tr.bends.push({ tick: pt.tick, value: clamp(Math.round((pt.f - 0.5) * 2 * 8192), -8192, 8191) });
      }, true);
    } else {
      const cc = +mode;
      commit('cc ramp', (tr) => {
        tr.ccs = tr.ccs.filter((c) => !(c.cc === cc && c.tick >= t0 && c.tick <= t1));
        for (const pt of pts) tr.ccs.push({ tick: pt.tick, cc, val: clamp(Math.round(pt.f * 127), 0, 127) });
      }, true);
    }
  }

  function laneEdit(x, y, dpr, G) {
    const mode = laneMode();
    const tick = Math.max(0, tickOfX(x, dpr));
    const frac = clamp(1 - (y - G.laneTop - 3 * dpr) / (G.laneH - 8 * dpr), 0, 1);
    if (mode === 'vel') {
      const vel = clamp(Math.round(frac * 127), 1, 127);
      const t = track();
      // Nearest note-on within a few pixels; selected notes move together.
      let best = -1, bd = 6 * dpr;
      t.notes.forEach((n, i) => { const d = Math.abs(xOfTick(n.tick, dpr) - x); if (d < bd) { bd = d; best = i; } });
      if (best < 0) return;
      const targets = selection.has(best) ? [...selection] : [best];
      commit('velocity', (tr) => { for (const i of targets) if (tr.notes[i]) tr.notes[i].vel = vel; }, true);
    } else if (mode === 'bend') {
      const st = snapT(tick);
      const value = clamp(Math.round((frac - 0.5) * 2 * 8192), -8192, 8191);
      commit('bend', (tr) => { tr.bends = tr.bends.filter((b) => b.tick !== st); tr.bends.push({ tick: st, value }); }, true);
    } else {
      const cc = +mode;
      const st = snapT(tick);
      const val = clamp(Math.round(frac * 127), 0, 127);
      commit('cc', (tr) => { tr.ccs = tr.ccs.filter((c) => !(c.cc === cc && c.tick === st)); tr.ccs.push({ tick: st, cc, val }); }, true);
    }
  }
  canvas.addEventListener('wheel', (e) => {
    e.preventDefault();
    if (e.ctrlKey || e.metaKey) {
      pxPerBeat = clamp(pxPerBeat * (e.deltaY < 0 ? 1.1 : 0.9), 4, 400);
    } else if (e.shiftKey) {
      scrollTick = Math.max(0, scrollTick + (e.deltaY / pxPerBeat) * store.p.ppq);
    } else {
      if (Math.abs(e.deltaX) > Math.abs(e.deltaY)) scrollTick = Math.max(0, scrollTick + (e.deltaX / pxPerBeat) * store.p.ppq);
      else topNote = clamp(topNote + Math.sign(e.deltaY) * -2, 12, 127);
    }
  }, { passive: false });

  const onKey = (e) => {
    if (['TEXTAREA', 'INPUT', 'SELECT'].includes(e.target.tagName)) return;
    const k = e.key.toLowerCase();
    if ((e.metaKey || e.ctrlKey) && k === 'd') {
      e.preventDefault();
      if (!selection.size) return;
      const t = track();
      const sel = [...selection].map((i) => t.notes[i]).filter(Boolean);
      const span = Math.max(...sel.map((n) => n.tick + n.dur)) - Math.min(...sel.map((n) => n.tick));
      const shift = snapT(span) || span;
      commit('duplicate', (tr) => { for (const n of sel) tr.notes.push({ ...n, tick: n.tick + shift }); });
      return;
    }
    if (e.metaKey || e.ctrlKey) return;
    if (k === 'backspace' || k === 'delete') {
      if (!selection.size) return;
      e.preventDefault();
      const idx = new Set(selection);
      commit('delete notes', (tr) => { tr.notes = tr.notes.filter((_, i) => !idx.has(i)); });
      selection.clear();
    } else if (k === 'd') setTool('draw');
    else if (k === 's') setTool('select');
    else if (k === 'e') setTool('erase');
    else if (k === 'a' && e.shiftKey) { selection = new Set(track().notes.map((_, i) => i)); }
  };

  refreshTrackList();
  return {
    setTrack,
    refreshTrackList,
    get channel() { return channel; },
    // What the view is currently showing. Scrolling is otherwise only
    // visible as pixels, and the smoke test needs to assert a direction.
    view: () => {
      const dpr = window.devicePixelRatio || 1;
      const G = geo(canvas.width, canvas.height, dpr);
      return { scrollTick, topNote, noteH, pxPerBeat,
               sbCss: { y: G.gridBottom / dpr, h: G.sbH / dpr } };
    },
    activate() { active = true; document.addEventListener('keydown', onKey); if (!raf) draw(); },
    deactivate() { active = false; document.removeEventListener('keydown', onKey); cancelAnimationFrame(raf); raf = 0; },
  };
}
