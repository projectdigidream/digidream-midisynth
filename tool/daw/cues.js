//
// Cues tab: test how gameplay pieces combine. Pieces are baked .mstk tracks
// (slices from the Cut & Loop tab, the whole project, imported files). A
// change can be queued to take over exactly when the current piece finishes
// its loop (or at the next bar), or crossfaded / hard-switched right now.
// Chain rules ("then → B after N loops") are executed here the way a game
// would: on the engine's piece-switch notification and loop wraps.
//
import { scoped, fmtTimeTenths as fmt, download } from '../lib/ui.js';

export function initCues(root, ctx) {
  const $ = scoped(root);
  const { store, client, log } = ctx;
  let current = null;      // name of the playing piece
  let pending = null;      // name queued
  let pendingMode = null;
  let lastSwitches = null;
  let lastTimeMs = 0;
  let wraps = 0;
  let chainQueued = false;
  let hold = false;        // A/B compare hold etc (unused)
  const infos = new Map();  // name -> {durationMs}

  const rules = () => store.p.cues.transitions;

  const cueLog = (msg, cls = '') => {
    const el = document.createElement('div');
    if (cls) el.className = cls;
    const st = ctx.playback.status();
    el.textContent = `${new Date().toLocaleTimeString()} ${st ? '[' + fmt(st.timeMs) + '] ' : ''}${msg}`;
    $('cue-log').appendChild(el);
    $('cue-log').scrollTop = $('cue-log').scrollHeight;
    while ($('cue-log').childNodes.length > 200) $('cue-log').removeChild($('cue-log').firstChild);
  };

  // ---------------- actions ----------------
  async function playNow(name) {
    const piece = ctx.pieces.get(name);
    if (!piece) return;
    ctx.enginePiecesMode();
    pending = null;
    try {
      const r = await client.loadTrack(piece.bytes);
      for (const w of r.warnings || []) log(`[cue] ${w}`, 'warn');
      const info = await client.trackInfo();
      infos.set(name, { durationMs: info.durationMs });
      client.setLoop(true);
      client.seekMs(0);
      client.play();
      setCurrent(name, 'play now');
    } catch (err) { log('[cue] ' + err.message, 'err'); }
    refresh();
  }
  async function queue(name, mode) {
    const piece = ctx.pieces.get(name);
    if (!piece) return;
    if (!current) { await playNow(name); return; }
    try {
      if (mode === 'crossfade') {
        await client.crossfadeTrack(piece.bytes, +$('cue-fade').value || 800);
        setCurrent(name, `crossfade ${$('cue-fade').value} ms from ${current}`);
      } else {
        await client.queueTrack(piece.bytes, mode === 'nextBar' ? 'nextBar' : 'loopEnd', 0);
        pending = name;
        pendingMode = mode;
        cueLog(`queued '${name}' (${mode === 'nextBar' ? 'next bar' : 'loop end'}) while '${current}' plays`);
      }
    } catch (err) { log('[cue] ' + err.message, 'err'); }
    refresh();
  }
  function cancel() {
    client.cancelQueued();
    if (pending) cueLog(`cancelled queued '${pending}'`);
    pending = null;
    chainQueued = false;
    refresh();
  }
  function setCurrent(name, how) {
    current = name;
    wraps = 0;
    chainQueued = false;
    lastTimeMs = 0;
    cueLog(`▶ '${name}' — ${how}`, 'sw');
    applyChain();
  }
  // Chain rule for the current piece: queue its successor when the required
  // number of loops has been entered.
  function applyChain() {
    if (!current || chainQueued) return;
    const rule = rules()[current];
    if (!rule || !rule.then || !ctx.pieces.has(rule.then)) return;
    const after = Math.max(1, rule.after | 0);
    if (wraps >= after - 1) {
      chainQueued = true;
      const mode = rule.mode === 'nextBar' ? 'nextBar' : 'loopEnd';
      client.queueTrack(ctx.pieces.get(rule.then).bytes, mode, 0)
        .then(() => { pending = rule.then; pendingMode = mode; cueLog(`chain: '${current}' → '${rule.then}' after ${after} loop(s), queued at ${mode === 'nextBar' ? 'next bar' : 'loop end'}`); refresh(); })
        .catch((err) => log('[cue] ' + err.message, 'err'));
    }
  }

  // ---------------- status ----------------
  function onStatus(s) {
    if (!ctx.inPiecesMode()) return;
    if (lastSwitches !== null && s.pieceSwitches !== lastSwitches && pending) {
      const from = current;
      current = pending;
      pending = null;
      wraps = 0;
      chainQueued = false;
      cueLog(`⇥ '${from}' → '${current}' at the boundary (${pendingMode === 'nextBar' ? 'next bar' : 'loop end'})`, 'sw');
      applyChain();
    }
    lastSwitches = s.pieceSwitches;
    if (s.playing && s.timeMs + 200 < lastTimeMs) { wraps++; applyChain(); }
    lastTimeMs = s.timeMs;
    // Cards.
    const cards = $('cue-cards').children;
    for (const card of cards) {
      const name = card.dataset.name;
      const isNow = name === current, isPending = name === pending;
      card.classList.toggle('now', isNow);
      card.classList.toggle('pending', isPending);
      const bar = card.querySelector('.bar div');
      const cnt = card.querySelector('.cnt');
      const badge = card.querySelector('.badge');
      if (isNow) {
        const dur = s.durationMs || infos.get(name)?.durationMs || 1;
        bar.style.width = Math.min(100, (s.timeMs / dur) * 100) + '%';
        cnt.textContent = s.queued ? `in ${(s.msToBoundary / 1000).toFixed(1)}s` : `loop ${wraps + 1} · ${fmt(s.timeMs)} / ${fmt(dur)}`;
        badge.textContent = s.playing ? 'playing' : 'paused';
        badge.className = 'badge on';
      } else if (isPending) {
        bar.style.width = '0%';
        cnt.textContent = `takes over in ${(s.msToBoundary / 1000).toFixed(1)}s`;
        badge.textContent = 'queued';
        badge.className = 'badge q';
      } else {
        bar.style.width = '0%';
        cnt.textContent = '';
        badge.textContent = '';
        badge.className = 'badge';
        badge.style.display = 'none';
        continue;
      }
      badge.style.display = '';
    }
    $('cue-cancel').disabled = !s.queued;
  }

  // ---------------- cards ----------------
  function refresh() {
    const box = $('cue-cards');
    box.innerHTML = '';
    const names = [...ctx.pieces.keys()];
    if (!names.length) {
      box.innerHTML = '<div class="hint">no pieces yet — cut slices in the Cut &amp; Loop tab, add the whole project, or import .mstk files</div>';
      return;
    }
    for (const name of names) {
      const piece = ctx.pieces.get(name);
      const rule = rules()[name] || {};
      const card = document.createElement('div');
      card.className = 'card' + (name === current ? ' now' : '') + (name === pending ? ' pending' : '');
      card.dataset.name = name;
      const thenOpts = ['<option value="">— none —</option>', ...names.filter((n) => n !== name).map((n) =>
        `<option ${rule.then === n ? 'selected' : ''}>${n}</option>`)].join('');
      card.innerHTML = `
        <h3><span>${name}</span><span class="badge" style="display:none"></span>
          <span class="hint" style="margin-left:auto">${piece.source}${piece.ms ? ' · ' + fmt(piece.ms) : ''}</span></h3>
        <div class="bar"><div></div></div>
        <div class="cnt">&nbsp;</div>
        <div class="row">
          <button class="small" data-a="now" title="load and play immediately">▶ now</button>
          <button class="small primary" data-a="loopEnd" title="switch when the current piece finishes its loop">⇥ at loop end</button>
          <button class="small" data-a="nextBar" title="switch at the next bar line">⇥ next bar</button>
          <button class="small" data-a="crossfade" title="crossfade now">⇄ fade</button>
        </div>
        <div class="row">
          <label class="hint">then →</label><select data-k="then" style="flex:1">${thenOpts}</select>
          <label class="hint">after</label><input type="number" data-k="after" class="narrow" min="1" max="64" value="${rule.after || 1}" style="width:46px">
          <label class="hint">loops</label>
          <select data-k="mode" title="boundary"><option value="loopEnd" ${rule.mode !== 'nextBar' ? 'selected' : ''}>loop end</option><option value="nextBar" ${rule.mode === 'nextBar' ? 'selected' : ''}>next bar</option></select>
        </div>
        <div class="row">
          <label class="hint"><input type="radio" name="cue-entry" ${store.p.cues.entry === name ? 'checked' : ''}> entry piece</label>
          <span style="flex:1"></span>
          <button class="small" data-a="export" title="download this piece">⬇</button>
          <button class="small" data-a="remove" title="remove from the board">×</button>
        </div>`;
      card.querySelector('[data-a=now]').onclick = () => playNow(name);
      card.querySelector('[data-a=loopEnd]').onclick = () => queue(name, 'loopEnd');
      card.querySelector('[data-a=nextBar]').onclick = () => queue(name, 'nextBar');
      card.querySelector('[data-a=crossfade]').onclick = () => queue(name, 'crossfade');
      card.querySelector('[data-a=export]').onclick = () => download(name + '.mstk', piece.bytes);
      card.querySelector('[data-a=remove]').onclick = () => { ctx.pieces.delete(name); if (piece.source === 'slice') ctx.removeSlice?.(name); refresh(); };
      const setRule = () => store.mutate('cue rule', (pp) => {
        const r = pp.cues.transitions[name] || {};
        r.then = card.querySelector('[data-k=then]').value || null;
        r.after = +card.querySelector('[data-k=after]').value || 1;
        r.mode = card.querySelector('[data-k=mode]').value;
        if (!r.then) delete pp.cues.transitions[name]; else pp.cues.transitions[name] = r;
      }, { audio: false });
      for (const k of ['then', 'after', 'mode']) card.querySelector(`[data-k=${k}]`).onchange = setRule;
      card.querySelector('input[type=radio]').onchange = () => store.mutate('cue entry', (pp) => { pp.cues.entry = name; }, { audio: false });
      box.appendChild(card);
    }
  }

  $('cue-stop').onclick = () => { client.stop(); cueLog('stopped'); };
  $('cue-cancel').onclick = cancel;

  function exportCuesJson() {
    const p = store.p;
    const names = [...ctx.pieces.keys()];
    const doc = {
      version: 1,
      entry: p.cues.entry && names.includes(p.cues.entry) ? p.cues.entry : names[0] || null,
      pieces: names.map((n) => ({ name: n, file: n + '.mstk', loop: true, lengthMs: Math.round(ctx.pieces.get(n).ms || infos.get(n)?.durationMs || 0) })),
      transitions: Object.fromEntries(Object.entries(p.cues.transitions).filter(([k, v]) => names.includes(k) && v.then && names.includes(v.then))
        .map(([k, v]) => [k, { then: v.then, after: v.after || 1, mode: v.mode || 'loopEnd' }])),
    };
    return JSON.stringify(doc, null, 2);
  }

  return { refresh, onStatus, exportCuesJson, playNow, queue, cancel, get current() { return current; } };
}
