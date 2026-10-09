//
// Master tab: the engine's deterministic mastering chain (EQ → compressor →
// width → gain → lookahead limiter) with live meters, plus the song-level
// send effects. Parameters live in project.master / project.fx and are baked
// into the .mstk; every knob is pushed to the engine immediately.
//
import { scoped, canvas2d, clamp } from '../lib/ui.js';
import { defaultMaster } from './project.js';

// MasterParamId order (src/synth/master_chain.h).
export const MASTER_IDS = {
  enabled: 0, 'eq.low.hz': 1, 'eq.low.db': 2, 'eq.mid.hz': 3, 'eq.mid.db': 4, 'eq.mid.q': 5,
  'eq.high.hz': 6, 'eq.high.db': 7, 'comp.thresholdDb': 8, 'comp.ratio': 9, 'comp.attackMs': 10,
  'comp.releaseMs': 11, 'comp.makeupDb': 12, 'comp.kneeDb': 13, 'limiter.ceilingDb': 14,
  'limiter.lookaheadBlocks': 15, 'limiter.releaseMs': 16, width: 17, gainDb: 18, satDrive: 19,
};
const getPath = (o, path) => path.split('.').reduce((a, k) => (a == null ? a : a[k]), o);
const setPath = (o, path, v) => { const ks = path.split('.'); let a = o; for (const k of ks.slice(0, -1)) a = a[k] ??= {}; a[ks[ks.length - 1]] = v; };
const dbfs = (lin) => (lin <= 0 ? -Infinity : 20 * Math.log10(lin));

export function initMaster(root, ctx) {
  const $ = scoped(root);
  const { store, client, log } = ctx;
  const sliders = [...root.querySelectorAll('[data-mp]')];
  let abHold = false;
  const loudHist = [];  // 3 s of mean squares

  const fmtVal = (path, v) => {
    if (path.endsWith('.hz')) return v >= 1000 ? (v / 1000).toFixed(2) + ' kHz' : v + ' Hz';
    if (path.endsWith('Db') || path.endsWith('.db')) return (v > 0 ? '+' : '') + (+v).toFixed(1) + ' dB';
    if (path.endsWith('Ms')) return (+v).toFixed(v < 10 ? 1 : 0) + ' ms';
    if (path === 'comp.ratio') return (+v).toFixed(1) + ':1';
    if (path === 'limiter.lookaheadBlocks') return v + ' blk (' + (v * 64 / 48).toFixed(1) + ' ms)';
    return (+v).toFixed(2);
  };
  function push(path, v) {
    const id = MASTER_IDS[path];
    if (id === undefined) return;
    client.masterParam(id, path === 'limiter.lookaheadBlocks' || path === 'enabled' ? v : v * 65536);
  }
  function setParam(path, v, { commit = true } = {}) {
    setPath(store.p.master, path, v);
    push(path, v);
    if (commit) store.mutate('master ' + path, () => {}, { audio: false, coalesce: true });
    syncUI();
  }
  for (const s of sliders) {
    const path = s.dataset.mp;
    s.oninput = () => setParam(path, +s.value);
  }
  $('mst-enabled').onchange = (e) => setParam('enabled', e.target.checked);
  $('mst-reset').onclick = () => { store.mutate('master reset', (pp) => { pp.master = { ...defaultMaster(), enabled: pp.master.enabled }; }, { audio: false }); pushAll(); syncUI(); };
  const ab = $('mst-ab');
  const abDown = () => { abHold = true; push('enabled', 0); ab.classList.add('active'); };
  const abUp = () => { if (!abHold) return; abHold = false; push('enabled', store.p.master.enabled ? 1 : 0); ab.classList.remove('active'); };
  ab.onpointerdown = abDown;
  ab.onpointerup = abUp;
  ab.onpointerleave = abUp;
  $('chk-dither').onchange = (e) => { client.dither(e.target.checked); store.p.view.dither = e.target.checked; };

  function syncUI() {
    const m = store.p.master;
    for (const s of sliders) {
      const v = getPath(m, s.dataset.mp);
      if (v === undefined) continue;
      if (document.activeElement !== s) s.value = v;
      s.parentElement.querySelector('.val').textContent = fmtVal(s.dataset.mp, v);
    }
    $('mst-enabled').checked = !!m.enabled;
    $('mst-latency').textContent = m.enabled ? `+${m.limiter.lookaheadBlocks * 64} samples latency (${(m.limiter.lookaheadBlocks * 64 / 48).toFixed(1)} ms)` : 'bypassed — output identical to an unmastered engine';
    syncFx();
  }
  function pushAll() {
    client.loadMaster(JSON.stringify({ version: 1, master: store.p.master })).catch((err) => log('[master] ' + err.message, 'err'));
    client.loadSongConfig(JSON.stringify({ ...store.p.fx, master: store.p.master })).catch((err) => log('[fx] ' + err.message, 'err'));
    client.dither(!!store.p.view.dither);
    $('chk-dither').checked = !!store.p.view.dither;
  }

  // ---------------- send FX ----------------
  const FX_PATH = { 'reverb:0': 'reverb.roomSize', 'reverb:1': 'reverb.damping', 'reverb:2': 'reverb.width', 'reverb:3': 'reverb.wetGain',
                    'chorus:0': 'chorus.rateHz', 'chorus:1': 'chorus.depthMs', 'chorus:2': 'chorus.wetGain',
                    'delay:0': 'delay.timeMs', 'delay:1': 'delay.feedback', 'delay:2': 'delay.wetGain' };
  const pushFxDoc = () => client.loadSongConfig(JSON.stringify({ ...store.p.fx, master: store.p.master })).catch(() => {});
  for (const el of root.querySelectorAll('[data-fx]')) {
    el.oninput = () => {
      const [fx, id] = el.dataset.fx.split(':');
      const v = +el.value / 1000;  // delay:0 slider is milli-ms (320000 = 320 ms)
      if (fx === 'reverb') client.reverbParam(+id, +el.value);
      else if (fx === 'chorus') client.chorusParam(+id, +el.value);
      setPath(store.p.fx, FX_PATH[el.dataset.fx], v);
      if (fx === 'delay') pushFxDoc();  // no dedicated live message: reload the fx config
      store.mutate('fx', () => {}, { audio: false, coalesce: true });
    };
  }
  const pp = root.querySelector('#fx-pingpong');
  if (pp) pp.onchange = () => {
    store.p.fx.delay.pingPong = pp.checked;
    pushFxDoc();
    store.mutate('fx', () => {}, { audio: false });
  };
  function syncFx() {
    for (const el of root.querySelectorAll('[data-fx]')) {
      const v = getPath(store.p.fx, FX_PATH[el.dataset.fx]);
      if (v !== undefined && document.activeElement !== el) el.value = Math.round(v * 1000);
    }
    const pp2 = root.querySelector('#fx-pingpong');
    if (pp2) pp2.checked = !!store.p.fx.delay?.pingPong;
  }

  // ---------------- per-channel EQ ----------------
  const ceqSel = root.querySelector('#ceq-ch');
  const ceqSliders = [...root.querySelectorAll('[data-ceq]')];
  function ceqDoc() {
    store.p.fx.channelEq ??= {};
    return store.p.fx.channelEq;
  }
  function refreshCeqChannels() {
    if (!ceqSel) return;
    const cur = ceqSel.value;
    ceqSel.innerHTML = '';
    for (const t of store.p.tracks) {
      const o = document.createElement('option');
      o.value = t.channel + 1;  // JSON keys are MIDI channels 1..16
      o.textContent = `${t.name} (ch ${t.channel + 1})`;
      ceqSel.appendChild(o);
    }
    if ([...ceqSel.options].some((o) => o.value === cur)) ceqSel.value = cur;
    syncCeq();
  }
  function syncCeq() {
    if (!ceqSel) return;
    const e = ceqDoc()[ceqSel.value] || {};
    for (const s of ceqSliders) {
      const def = s.dataset.ceq === 'midHz' ? 1000 : 0;
      const v = e[s.dataset.ceq] ?? def;
      if (document.activeElement !== s) s.value = v;
      s.parentElement.querySelector('.val').textContent =
          s.dataset.ceq === 'midHz' ? (v >= 1000 ? (v / 1000).toFixed(2) + ' kHz' : v + ' Hz')
                                    : (v > 0 ? '+' : '') + (+v).toFixed(1) + ' dB';
    }
  }
  if (ceqSel) {
    ceqSel.onchange = syncCeq;
    for (const s of ceqSliders) {
      s.oninput = () => {
        const doc = ceqDoc();
        const key = ceqSel.value;
        const e = (doc[key] ??= {});
        const v = +s.value;
        const def = s.dataset.ceq === 'midHz' ? 1000 : 0;
        if (v === def) delete e[s.dataset.ceq]; else e[s.dataset.ceq] = v;
        if (!Object.keys(e).length) delete doc[key];
        pushFxDoc();
        store.mutate('channel eq', () => {}, { audio: false, coalesce: true });
        syncCeq();
      };
    }
  }

  // ---------------- EQ canvas ----------------
  const eq = $('eq-canvas');
  const fMin = 20, fMax = 20000, dbTop = 24, dbBot = -24;
  const xOf = (hz, w) => (Math.log(hz / fMin) / Math.log(fMax / fMin)) * w;
  const hzOf = (x, w) => fMin * Math.pow(fMax / fMin, x / w);
  const yOf = (db, h) => ((dbTop - db) / (dbTop - dbBot)) * h;
  const dbOf = (y, h) => dbTop - (y / h) * (dbTop - dbBot);
  // Approximate magnitude response (dB) of the parallel-form chain.
  function response(hz) {
    const m = store.p.master;
    const fc = (f) => Math.min(f, 12000);  // engine cutoff cap
    let g = 1;
    { const w = hz / fc(m.eq.low.hz); g *= 1 + (Math.pow(10, m.eq.low.db / 20) - 1) / Math.sqrt(1 + w ** 4); }
    { const w = hz / fc(m.eq.high.hz); const hp = (w * w) / Math.sqrt(1 + w ** 4); g *= 1 + (Math.pow(10, m.eq.high.db / 20) - 1) * hp; }
    { const w = hz / fc(m.eq.mid.hz); const Q = m.eq.mid.q; const bp = (w / Q) / Math.sqrt((1 - w * w) ** 2 + (w / Q) ** 2); g *= 1 + (Math.pow(10, m.eq.mid.db / 20) - 1) * bp; }
    return 20 * Math.log10(Math.max(1e-6, g));
  }
  let eqDrag = null;
  function drawEq() {
    const { g, w, h, dpr } = canvas2d(eq);
    g.fillStyle = '#101318';
    g.fillRect(0, 0, w, h);
    g.strokeStyle = 'rgba(255,255,255,0.07)';
    g.font = `${9 * dpr}px ui-monospace`;
    for (const hz of [50, 100, 200, 500, 1000, 2000, 5000, 10000]) {
      const x = xOf(hz, w);
      g.beginPath(); g.moveTo(x, 0); g.lineTo(x, h); g.stroke();
      g.fillStyle = 'rgba(255,255,255,0.3)';
      g.fillText(hz >= 1000 ? hz / 1000 + 'k' : String(hz), x + 2 * dpr, h - 3 * dpr);
    }
    for (const db of [-18, -12, -6, 0, 6, 12, 18]) {
      const y = yOf(db, h);
      g.strokeStyle = db === 0 ? 'rgba(255,255,255,0.25)' : 'rgba(255,255,255,0.06)';
      g.beginPath(); g.moveTo(0, y); g.lineTo(w, y); g.stroke();
    }
    g.fillStyle = 'rgba(224,112,112,0.08)';
    g.fillRect(xOf(12000, w), 0, w - xOf(12000, w), h);
    const m = store.p.master;
    g.strokeStyle = m.enabled ? '#64b5f6' : 'rgba(100,181,246,0.4)';
    g.lineWidth = 2 * dpr;
    g.beginPath();
    for (let px = 0; px <= w; px += 2) {
      const y = yOf(clamp(response(hzOf(px, w)), dbBot, dbTop), h);
      if (px === 0) g.moveTo(px, y); else g.lineTo(px, y);
    }
    g.stroke();
    const handles = [
      { path: 'eq.low', hz: m.eq.low.hz, db: m.eq.low.db, c: '#e0b060' },
      { path: 'eq.mid', hz: m.eq.mid.hz, db: m.eq.mid.db, c: '#7bc67b' },
      { path: 'eq.high', hz: m.eq.high.hz, db: m.eq.high.db, c: '#d678d6' },
    ];
    for (const hd of handles) {
      g.fillStyle = hd.c;
      g.beginPath(); g.arc(xOf(hd.hz, w), yOf(hd.db, h), 6 * dpr, 0, Math.PI * 2); g.fill();
    }
  }
  const handleAt = (x, y, w, h, dpr) => {
    const m = store.p.master;
    const hs = [['eq.low', m.eq.low], ['eq.mid', m.eq.mid], ['eq.high', m.eq.high]];
    let best = null, bd = 16 * dpr;
    for (const [path, b] of hs) {
      const d = Math.hypot(xOf(b.hz, w) - x, yOf(b.db, h) - y);
      if (d < bd) { bd = d; best = path; }
    }
    return best;
  };
  eq.addEventListener('pointerdown', (e) => {
    const { w, h, dpr } = canvas2d(eq);
    const r = eq.getBoundingClientRect();
    const x = (e.clientX - r.left) * dpr, y = (e.clientY - r.top) * dpr;
    const hd = handleAt(x, y, w, h, dpr);
    if (hd) { eqDrag = { path: hd, y0: y, q0: store.p.master.eq.mid.q }; eq.setPointerCapture(e.pointerId); }
  });
  eq.addEventListener('pointermove', (e) => {
    if (!eqDrag) return;
    const { w, h, dpr } = canvas2d(eq);
    const r = eq.getBoundingClientRect();
    const x = (e.clientX - r.left) * dpr, y = (e.clientY - r.top) * dpr;
    if (e.shiftKey && eqDrag.path === 'eq.mid') {
      setParam('eq.mid.q', clamp(+(eqDrag.q0 * Math.pow(2, (eqDrag.y0 - y) / (40 * dpr))).toFixed(2), 0.5, 10), { commit: false });
      return;
    }
    const lim = { 'eq.low': [20, 500], 'eq.mid': [100, 12000], 'eq.high': [1000, 16000] }[eqDrag.path];
    setParam(eqDrag.path + '.hz', Math.round(clamp(hzOf(x, w), lim[0], lim[1])), { commit: false });
    setParam(eqDrag.path + '.db', +clamp(dbOf(y, h), -24, 24).toFixed(1), { commit: false });
  });
  eq.addEventListener('pointerup', () => { if (eqDrag) { eqDrag = null; store.mutate('master eq', () => {}, { audio: false }); } });

  // ---------------- meters ----------------
  function onStatus(s) {
    const m = s.meters;
    if (!m) return;
    const peak = (v) => dbfs(v / 16777216);
    const rms = (ms) => dbfs(Math.sqrt(ms / 4294967296));
    const bar = (id, db, lo = -60) => {
      const el = $(id), v = $(id + '-v');
      if (!el) return;
      el.style.width = clamp(((db - lo) / -lo) * 100, 0, 100) + '%';
      v.textContent = db === -Infinity ? '-∞' : db.toFixed(1);
    };
    bar('mt-pl', peak(m.peakL)); bar('mt-pr', peak(m.peakR));
    bar('mt-rl', rms(m.msL)); bar('mt-rr', rms(m.msR));
    const grDb = dbfs(m.compGr / 65536), limDb = dbfs(m.limGain / 65536);
    $('mt-gr').style.width = clamp((-grDb / 24) * 100, 0, 100) + '%';
    $('mt-gr-v').textContent = grDb.toFixed(1) + ' dB';
    $('mt-lim').style.width = clamp((-limDb / 12) * 100, 0, 100) + '%';
    $('mt-lim-v').textContent = limDb.toFixed(1) + ' dB';
    // Short-term loudness: RMS over a 3 s window, both channels (≈ LUFS-ish,
    // no K-weighting yet).
    loudHist.push((m.msL + m.msR) / 2);
    while (loudHist.length > 30) loudHist.shift();
    const avg = loudHist.reduce((a, b) => a + b, 0) / loudHist.length;
    const l = rms(avg);
    $('mt-loud').textContent = l === -Infinity ? '–' : `${l.toFixed(1)} dBFS RMS (3 s)`;
    $('mt-clips').textContent = m.clips ? ` · ${m.clips} clipped` : '';
    drawEq();
  }

  store.addEventListener('change', (e) => { if (e.detail?.structural || e.detail?.loaded || e.detail?.label === 'add track' || e.detail?.label === 'track channel' || e.detail?.label === 'delete track') refreshCeqChannels(); });
  refreshCeqChannels();
  syncUI();
  drawEq();
  return { syncUI, pushAll, onStatus, drawEq, refreshCeqChannels };
}
