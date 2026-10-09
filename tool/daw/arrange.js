//
// Arrange tab: track list (name, channel, program, mute/solo/arm) and the
// lane overview — note blocks per track, bar ruler, loop region (drag on the
// ruler), playhead, live highlight of what the engine is triggering.
//
import { GM_NAMES } from '../lib/gm.js';
import { scoped, canvas2d, clamp } from '../lib/ui.js';
import { newTrack, barStarts, projectEndTick, barTicksAt, TRACK_COLORS } from './project.js';

const RULER_H = 24, ROW_H = 68;

export function initArrange(root, ctx) {
  const $ = scoped(root);
  const { store, client, log } = ctx;
  const canvas = $('lanes');
  let pxPerBeat = 30;
  let scrollTick = 0;
  let raf = 0;
  let drag = null;  // {kind:'loop', startTick} | {kind:'seek'}

  $('arr-zoom').oninput = (e) => { pxPerBeat = +e.target.value; };
  $('btn-add-track').onclick = () => {
    const p = store.p;
    const used = new Set(p.tracks.map((t) => t.channel));
    const free = [...Array(16).keys()].find((c) => !used.has(c) && c !== 9) ?? [...Array(16).keys()].find((c) => !used.has(c));
    if (free === undefined) { log('all 16 channels are in use', 'warn'); return; }
    store.mutate('add track', (pp) => pp.tracks.push(newTrack(free, { program: 0 })));
    ctx.ui.selectTrack(free);
  };

  // ---------------- track list ----------------
  function refreshTracks() {
    const p = store.p;
    const box = $('track-rows');
    box.innerHTML = '';
    for (const t of p.tracks) {
      const row = document.createElement('div');
      row.className = 'trow' + (t.channel === ctx.ui.selectedChannel ? ' sel' : '');
      row.dataset.ch = t.channel;
      const used = new Set(p.tracks.map((x) => x.channel));
      const chOpts = [...Array(16).keys()].map((c) =>
        `<option value="${c}" ${c === t.channel ? 'selected' : ''} ${used.has(c) && c !== t.channel ? 'disabled' : ''}>ch ${c + 1}${c === 9 ? ' (drums)' : ''}</option>`).join('');
      const progOpts = t.channel === 9 ? '<option>drum kit</option>'
        : GM_NAMES.map((n, i) => `<option value="${i}" ${i === t.program ? 'selected' : ''}>${i} ${n}</option>`).join('');
      row.innerHTML = `
        <div class="sw" style="background:${t.color}"></div>
        <div class="tbody">
          <div class="l1">
            <input type="text" value="${t.name.replace(/"/g, '&quot;')}" title="track name">
            <div class="btns">
              <button data-k="m" class="${t.mute ? 'on-m' : ''}" title="mute">M</button>
              <button data-k="s" class="${t.solo ? 'on-s' : ''}" title="solo">S</button>
              <button data-k="r" class="${t.arm ? 'on-r' : ''}" title="arm for recording">R</button>
              <button data-k="e" title="edit this instrument in the Synth tab">♪</button>
              <button data-k="x" title="delete track">×</button>
            </div>
          </div>
          <div class="l2">
            <select data-k="ch" title="MIDI channel">${chOpts}</select>
            <select data-k="prog" title="GM program" ${t.channel === 9 ? 'disabled' : ''}>${progOpts}</select>
          </div>
        </div>`;
      row.onclick = (e) => { if (!e.target.closest('input,select,button')) ctx.ui.selectTrack(t.channel); };
      row.ondblclick = (e) => { if (!e.target.closest('input,select,button')) { ctx.ui.selectTrack(t.channel); ctx.ui.showTab('track'); } };
      row.querySelector('input').onchange = (e) => store.mutate('rename track', (pp) => { pp.tracks.find((x) => x.channel === t.channel).name = e.target.value; }, { audio: false });
      row.querySelector('[data-k=ch]').onchange = (e) => {
        const nc = +e.target.value;
        store.mutate('track channel', (pp) => {
          const tr = pp.tracks.find((x) => x.channel === t.channel);
          tr.channel = nc;
          if (nc === 9) tr.program = 0;
          tr.color = TRACK_COLORS[nc % TRACK_COLORS.length];
        });
        ctx.ui.selectTrack(nc);
      };
      row.querySelector('[data-k=prog]').onchange = (e) => {
        const prog = +e.target.value;
        store.mutate('program', (pp) => { pp.tracks.find((x) => x.channel === t.channel).program = prog; });
        client.program(t.channel, prog);  // immediate, before the debounced reload
      };
      row.querySelector('[data-k=m]').onclick = () => store.mutate('mute', (pp) => { const tr = pp.tracks.find((x) => x.channel === t.channel); tr.mute = !tr.mute; });
      row.querySelector('[data-k=s]').onclick = () => store.mutate('solo', (pp) => { const tr = pp.tracks.find((x) => x.channel === t.channel); tr.solo = !tr.solo; });
      row.querySelector('[data-k=r]').onclick = () => store.mutate('arm', (pp) => {
        for (const tr of pp.tracks) tr.arm = tr.channel === t.channel ? !tr.arm : false;
      }, { audio: false });
      row.querySelector('[data-k=e]').onclick = () => { ctx.ui.selectTrack(t.channel); ctx.ui.editTrackInstrument(t.channel); };
      row.querySelector('[data-k=x]').onclick = () => {
        if (p.tracks.length <= 1) { log('keep at least one track', 'warn'); return; }
        store.mutate('delete track', (pp) => { pp.tracks = pp.tracks.filter((x) => x.channel !== t.channel); });
        if (ctx.ui.selectedChannel === t.channel) ctx.ui.selectTrack(store.p.tracks[0].channel);
      };
      box.appendChild(row);
    }
    const wrap = $('arr-canvas-wrap');
    wrap.style.height = (RULER_H + Math.max(4, p.tracks.length) * ROW_H) + 'px';
  }

  // ---------------- lanes ----------------
  const tickToX = (tick, w) => ((tick - scrollTick) / store.p.ppq) * pxPerBeat;
  const xToTick = (x) => scrollTick + (x / pxPerBeat) * store.p.ppq;

  function draw() {
    raf = requestAnimationFrame(draw);
    const { g, w, h, dpr } = canvas2d(canvas);
    const p = store.p;
    g.fillStyle = '#101318';
    g.fillRect(0, 0, w, h);
    const rulerH = RULER_H * dpr, rowH = ROW_H * dpr;
    const X = (tick) => tickToX(tick, w) * dpr;
    const end = projectEndTick(p);
    const viewEnd = xToTick(w / dpr);

    // Follow the playhead.
    const pos = ctx.playback.positionTick();
    if (ctx.playback.isPlaying() && (pos > viewEnd || pos < scrollTick)) scrollTick = Math.max(0, pos - p.ppq);

    // Bars.
    const bars = barStarts(p, Math.max(end, viewEnd) + barTicksAt(p, 0));
    g.font = `${10 * dpr}px ui-monospace`;
    bars.forEach((t, i) => {
      const x = X(t);
      if (x < -10 || x > w + 10) return;
      g.strokeStyle = 'rgba(255,255,255,0.10)';
      g.beginPath(); g.moveTo(x, rulerH); g.lineTo(x, h); g.stroke();
      g.fillStyle = 'rgba(255,255,255,0.45)';
      g.fillText(String(i + 1), x + 3 * dpr, 14 * dpr);
      // Beats.
      const bt = barTicksAt(p, t), beat = bt / Math.max(1, Math.round(bt / (p.ppq * 4 / p.timeSigs[0].den)) || 4);
      for (let b = t + beat; b < t + bt; b += beat) {
        const bx = X(b);
        g.strokeStyle = 'rgba(255,255,255,0.04)';
        g.beginPath(); g.moveTo(bx, rulerH); g.lineTo(bx, h); g.stroke();
      }
    });
    // Song end shade.
    const xe = X(end);
    if (xe < w) { g.fillStyle = 'rgba(0,0,0,0.35)'; g.fillRect(xe, rulerH, w - xe, h - rulerH); }
    // Loop region on the ruler.
    if (p.loop.enabled) {
      const x0 = X(p.loop.startTick), x1 = X(p.loop.endTick);
      g.fillStyle = 'rgba(100,181,246,0.25)';
      g.fillRect(x0, 0, x1 - x0, rulerH);
      g.fillStyle = 'rgba(100,181,246,0.06)';
      g.fillRect(x0, rulerH, x1 - x0, h - rulerH);
      g.strokeStyle = '#64b5f6';
      g.lineWidth = 1.5 * dpr;
      g.beginPath(); g.moveTo(x0, 0); g.lineTo(x0, h); g.stroke();
      g.beginPath(); g.moveTo(x1, 0); g.lineTo(x1, h); g.stroke();
    }
    g.strokeStyle = 'rgba(255,255,255,0.15)';
    g.lineWidth = 1;
    g.beginPath(); g.moveTo(0, rulerH); g.lineTo(w, rulerH); g.stroke();

    // Track lanes.
    const st = ctx.playback.status();
    const active = st?.activeNotes || [];
    p.tracks.forEach((t, i) => {
      const y0 = rulerH + i * rowH;
      g.fillStyle = t.channel === ctx.ui.selectedChannel ? 'rgba(255,255,255,0.04)' : 'transparent';
      g.fillRect(0, y0, w, rowH);
      g.strokeStyle = 'rgba(255,255,255,0.08)';
      g.beginPath(); g.moveTo(0, y0 + rowH); g.lineTo(w, y0 + rowH); g.stroke();
      const lit = new Set(String(active[t.channel] || '').split(',').filter(Boolean).map(Number));
      const audible = !t.mute && (!p.tracks.some((x) => x.solo) || t.solo);
      let lo = 127, hi = 0;
      for (const n of t.notes) { lo = Math.min(lo, n.note); hi = Math.max(hi, n.note); }
      const span = Math.max(12, hi - lo + 1);
      for (const n of t.notes) {
        const x = X(n.tick), xw = Math.max(2 * dpr, (n.dur / p.ppq) * pxPerBeat * dpr);
        if (x + xw < 0 || x > w) continue;
        const ny = y0 + rowH - 4 * dpr - ((n.note - lo + 0.5) / span) * (rowH - 8 * dpr);
        g.fillStyle = lit.has(n.note) && ctx.playback.isPlaying() && pos >= n.tick && pos < n.tick + n.dur ? '#ffffff'
                    : (audible ? t.color : 'rgba(255,255,255,0.18)');
        g.fillRect(x, ny - 1.5 * dpr, xw, 3 * dpr);
      }
      if (lit.size && audible) {
        g.fillStyle = 'rgba(123,198,123,0.9)';
        g.beginPath(); g.arc(8 * dpr, y0 + 8 * dpr, 3 * dpr, 0, Math.PI * 2); g.fill();
      }
    });

    // Playhead.
    const px = X(pos);
    g.strokeStyle = '#7bc67b';
    g.lineWidth = 1.5 * dpr;
    g.beginPath(); g.moveTo(px, 0); g.lineTo(px, h); g.stroke();
  }

  canvas.addEventListener('pointerdown', (e) => {
    const rect = canvas.getBoundingClientRect();
    const x = e.clientX - rect.left, y = e.clientY - rect.top;
    const tick = Math.max(0, xToTick(x));
    const snap = ctx.ui.snapTicks() || barTicksAt(store.p, tick);
    if (y < RULER_H) {
      const barSnap = barTicksAt(store.p, tick);
      drag = { kind: 'loop', startTick: Math.round(tick / barSnap) * barSnap };
      canvas.setPointerCapture(e.pointerId);
    } else {
      const row = Math.floor((y - RULER_H) / ROW_H);
      const t = store.p.tracks[row];
      if (t) ctx.ui.selectTrack(t.channel);
      ctx.playback.seekTick(Math.round(tick / snap) * snap);
      drag = { kind: 'seek' };
    }
  });
  canvas.addEventListener('pointermove', (e) => {
    if (!drag || drag.kind !== 'loop') return;
    const rect = canvas.getBoundingClientRect();
    const tick = Math.max(0, xToTick(e.clientX - rect.left));
    const barSnap = barTicksAt(store.p, tick);
    const t = Math.round(tick / barSnap) * barSnap;
    if (t !== drag.startTick) {
      const s = Math.min(t, drag.startTick), en = Math.max(t, drag.startTick);
      drag.preview = { s, en };
      store.p.loop.startTick = s;  // live preview; committed on pointerup
      store.p.loop.endTick = en;
    }
  });
  canvas.addEventListener('pointerup', () => {
    if (drag?.kind === 'loop' && drag.preview) {
      const { s, en } = drag.preview;
      store.mutate('loop region', (pp) => { pp.loop.startTick = s; pp.loop.endTick = en; pp.loop.enabled = true; });
      $('chk-loop') && ($('chk-loop').checked = true);
    }
    drag = null;
  });
  canvas.addEventListener('dblclick', (e) => {
    const rect = canvas.getBoundingClientRect();
    const y = e.clientY - rect.top;
    if (y < RULER_H) return;
    const t = store.p.tracks[Math.floor((y - RULER_H) / ROW_H)];
    if (t) { ctx.ui.selectTrack(t.channel); ctx.ui.showTab('track'); }
  });
  canvas.addEventListener('wheel', (e) => {
    e.preventDefault();
    if (e.ctrlKey || e.metaKey) {
      pxPerBeat = clamp(pxPerBeat * (e.deltaY < 0 ? 1.1 : 0.9), 1, 100);
      $('arr-zoom').value = pxPerBeat;
    } else {
      scrollTick = Math.max(0, scrollTick + ((e.deltaX || e.deltaY) / pxPerBeat) * store.p.ppq);
    }
  }, { passive: false });

  refreshTracks();
  return {
    refreshTracks,
    start() { if (!raf) draw(); },
    stop() { cancelAnimationFrame(raf); raf = 0; },
  };
}
