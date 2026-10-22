//
// Track cutter panel: slices baked .mstk tracks into smaller gameplay-ready
// pieces. Cutting and baking happen in the WASM engine (state chase, note
// closing, canonical tempo re-anchoring), so what you export is exactly what
// you auditioned. Hosted inside `root` (page or DAW tab).
//
//   const cutter = initCutterPanel(root, client, { log, onSlicesChanged });
//   cutter.loadTrackBytes(arrayBuffer, 'name');
//
import { scoped, clamp, fmtTimeTenths as fmtTime, download, canvas2d } from './ui.js';
import { saveAllBytes } from './desktop.js';

export const CUTTER_HTML = `
<section>
  <h2>Source track</h2>
  <div class="row">
    <input type="file" id="track-file" accept=".mstk">
    <span id="track-desc" class="hint">load a baked .mstk</span>
    <label style="margin-left:auto">volume</label>
    <input type="range" id="volume" min="0" max="300" value="125">
  </div>
</section>
<section>
  <h2>Timeline — drag to select, click a bar to move the playhead</h2>
  <canvas id="timeline"></canvas>
  <div class="row">
    <span id="region-label">no region</span>
    <label><input type="checkbox" id="chk-snap" checked> snap to bars</label>
    <span style="flex:1"></span>
    <button id="btn-preview" class="primary" disabled>▶ Loop region</button>
    <button id="btn-play-all" disabled>▶ Whole track</button>
    <button id="btn-stop">■</button>
  </div>
  <div class="row">
    <label>crossfade ms</label>
    <input type="number" id="fade-ms" min="20" max="8000" value="800">
    <button id="btn-xfade-region" disabled title="cut the region in memory and crossfade the current playback into it looping">⇄ Crossfade into region</button>
    <span style="flex:1"></span>
    <input type="text" id="slice-name" placeholder="slice name" style="width:160px">
    <button id="btn-add-slice" disabled>＋ Add slice</button>
  </div>
</section>
<section>
  <h2>Slices — gameplay-ready pieces</h2>
  <table id="slice-table" style="display:none">
    <thead><tr><th>name</th><th>range</th><th>length</th><th>size</th><th></th></tr></thead>
    <tbody id="slice-rows"></tbody>
  </table>
  <div class="row" id="slice-empty"><span class="hint">no slices yet — select a region and press “＋ Add slice”</span></div>
  <div class="row">
    <button id="btn-export-all" disabled>⬇ Export all slices</button>
    <span class="hint">each slice is a self-contained .mstk: state-chased at its start, cross-boundary notes closed,
      loops whole, shares the parent’s instruments (crossfade-compatible family)</span>
  </div>
</section>`;

export function initCutterPanel(root, client, opts = {}) {
  const $ = scoped(root);
  const log = opts.log || ((m) => console.log(m));
  const has = (id) => !!$(id);

  let parentBytes = null;
  let parentName = 'track';
  let info = null;
  let region = null;
  let status = { timeMs: 0, durationMs: 0 };
  const slices = [];
  let raf = 0;

  const changed = () => opts.onSlicesChanged?.(slices);

  client.addEventListener('status', (e) => {
    status = e.detail;
    const set = (id, v) => { const el = $(id); if (el) el.textContent = v; };
    set('m-bar', status.bar + 1);
    set('m-beat', status.beat + 1);
    set('m-voices', status.activeVoices);
    set('t-cur', fmtTime(status.timeMs));
  });

  // ---------------- track loading ----------------
  async function loadTrackBytes(buf, name) {
    const u8 = new Uint8Array(buf);
    if (!(u8[0] === 0x4D && u8[1] === 0x53 && u8[2] === 0x54 && u8[3] === 0x4B)) {
      log((name || 'file') + ' is not a .mstk track', 'err');
      return false;
    }
    parentBytes = buf.slice(0);
    parentName = (name || 'track').replace(/\.mstk$/i, '');
    slices.length = 0;
    renderSlices();
    try {
      const r = await client.loadTrack(parentBytes);
      for (const w of r.warnings || []) log(`[track] ${w}`, 'warn');
      info = await client.trackInfo();
      if (has('track-desc'))
        $('track-desc').textContent = `${parentName} · ${fmtTime(info.durationMs)} · ${info.barTicks.length} bars`;
      if (has('btn-play-all')) $('btn-play-all').disabled = false;
      setRegion(null);
      log('track loaded', 'ok');
      return true;
    } catch (err) {
      log('[track] ' + err.message, 'err');
      return false;
    }
  }
  if (has('track-file')) $('track-file').onchange = async (e) => {
    const f = e.target.files[0];
    if (f) loadTrackBytes(await f.arrayBuffer(), f.name);
  };
  if (has('volume')) $('volume').oninput = (e) => client.masterVolume(+e.target.value);

  // ---------------- tick/sample/pixel mapping ----------------
  const endSample = () => (info ? info.durationMs * 48 : 1);
  function sampleToTick(sample) {
    const bt = info.barTicks, bs = info.barSamples;
    let i = 0;
    while (i + 1 < bs.length && bs[i + 1] <= sample) i++;
    const s0 = bs[i], t0 = bt[i];
    const s1 = i + 1 < bs.length ? bs[i + 1] : endSample();
    const t1 = i + 1 < bt.length ? bt[i + 1] : info.endTick;
    if (s1 <= s0) return t0;
    return Math.round(t0 + ((sample - s0) * (t1 - t0)) / (s1 - s0));
  }
  function tickToSample(tick) {
    const bt = info.barTicks, bs = info.barSamples;
    let i = 0;
    while (i + 1 < bt.length && bt[i + 1] <= tick) i++;
    const t0 = bt[i], s0 = bs[i];
    const t1 = i + 1 < bt.length ? bt[i + 1] : info.endTick;
    const s1 = i + 1 < bs.length ? bs[i + 1] : endSample();
    if (t1 <= t0) return s0;
    return s0 + ((tick - t0) * (s1 - s0)) / (t1 - t0);
  }
  function snapTick(tick) {
    if (has('chk-snap') && !$('chk-snap').checked) return clamp(tick, 0, info.endTick);
    let best = info.barTicks[0], bd = Infinity;
    for (const t of info.barTicks) { const d = Math.abs(t - tick); if (d < bd) { bd = d; best = t; } }
    const dEnd = Math.abs(info.endTick - tick);
    return dEnd < bd ? info.endTick : best;
  }
  function barIndexOf(tick) {
    let i = 0;
    while (i + 1 < info.barTicks.length && info.barTicks[i + 1] <= tick) i++;
    return i;
  }

  // ---------------- timeline ----------------
  let dragStartTick = null;
  const tl = $('timeline');
  function pxToTick(clientX) {
    const rect = tl.getBoundingClientRect();
    const frac = clamp((clientX - rect.left) / rect.width, 0, 1);
    return sampleToTick(frac * endSample());
  }
  tl.addEventListener('pointerdown', (e) => {
    if (!info) return;
    dragStartTick = snapTick(pxToTick(e.clientX));
    tl.setPointerCapture(e.pointerId);
  });
  tl.addEventListener('pointermove', (e) => {
    if (dragStartTick === null || !info) return;
    const t = snapTick(pxToTick(e.clientX));
    if (t !== dragStartTick) setRegion({ startTick: Math.min(dragStartTick, t), endTick: Math.max(dragStartTick, t) });
  });
  tl.addEventListener('pointerup', (e) => {
    if (!info) return;
    const t = snapTick(pxToTick(e.clientX));
    if (dragStartTick !== null && t === dragStartTick) {
      client.seekMs(tickToSample(t) / 48);
      client.play();
    }
    dragStartTick = null;
  });

  function setRegion(r) {
    region = r;
    const hasR = !!r;
    for (const id of ['btn-preview', 'btn-xfade-region', 'btn-add-slice']) if (has(id)) $(id).disabled = !hasR;
    if (hasR) {
      const ms0 = tickToSample(r.startTick) / 48, ms1 = tickToSample(r.endTick) / 48;
      $('region-label').textContent =
        `bars ${barIndexOf(r.startTick) + 1}–${barIndexOf(Math.max(r.startTick, r.endTick - 1)) + 1}` +
        ` · ${fmtTime(ms0)}–${fmtTime(ms1)} · ${fmtTime(ms1 - ms0)}`;
      if (has('slice-name') && !$('slice-name').value)
        $('slice-name').value = `${parentName}_${barIndexOf(r.startTick) + 1}`;
    } else {
      $('region-label').textContent = 'no region';
    }
    opts.onRegionChanged?.(region);
  }

  function drawTimeline() {
    raf = requestAnimationFrame(drawTimeline);
    const { g, w, h, dpr } = canvas2d(tl);
    g.fillStyle = '#101318';
    g.fillRect(0, 0, w, h);
    if (!info) {
      g.fillStyle = 'rgba(255,255,255,0.25)';
      g.font = `${12 * dpr}px ui-monospace`;
      g.fillText('load a .mstk track', 10 * dpr, 20 * dpr);
      return;
    }
    const d = info.density;
    let peak = 1;
    for (const v of d) peak = Math.max(peak, v);
    g.fillStyle = 'rgba(111,214,111,0.35)';
    const bw = w / d.length;
    for (let i = 0; i < d.length; i++) {
      const bh = (d[i] / peak) * (h * 0.72);
      g.fillRect(i * bw, h - bh, bw - 0.5 * dpr, bh);
    }
    const xOfSample = (s) => (s / endSample()) * w;
    const labelEvery = info.barTicks.length > 120 ? 8 : 4;
    g.font = `${9 * dpr}px ui-monospace`;
    info.barTicks.forEach((t, i) => {
      const x = xOfSample(info.barSamples[i]);
      g.strokeStyle = i % labelEvery === 0 ? 'rgba(255,255,255,0.18)' : 'rgba(255,255,255,0.06)';
      g.beginPath(); g.moveTo(x, 0); g.lineTo(x, h); g.stroke();
      if (i % labelEvery === 0) { g.fillStyle = 'rgba(255,255,255,0.35)'; g.fillText(String(i + 1), x + 2 * dpr, 10 * dpr); }
    });
    if (region) {
      const x0 = xOfSample(tickToSample(region.startTick));
      const x1 = xOfSample(tickToSample(region.endTick));
      g.fillStyle = 'rgba(100,181,246,0.18)';
      g.fillRect(x0, 0, x1 - x0, h);
      g.strokeStyle = '#64b5f6';
      g.lineWidth = 1.5 * dpr;
      g.beginPath(); g.moveTo(x0, 0); g.lineTo(x0, h); g.stroke();
      g.beginPath(); g.moveTo(x1, 0); g.lineTo(x1, h); g.stroke();
    }
    if (status.durationMs > 0) {
      const x = (status.timeMs / (info.durationMs || 1)) * w;
      g.strokeStyle = '#7bc67b';
      g.lineWidth = 1.5 * dpr;
      g.beginPath(); g.moveTo(x, 0); g.lineTo(x, h); g.stroke();
    }
  }

  // ---------------- transport ----------------
  const fadeMs = () => (has('fade-ms') ? +$('fade-ms').value : 800);
  if (has('btn-preview')) $('btn-preview').onclick = () => {
    if (!region) return;
    client.loopTicks(region.startTick, region.endTick);
    client.setLoop(true);
    client.seekMs(tickToSample(region.startTick) / 48);
    client.play();
    log(`looping region (${$('region-label').textContent})`);
  };
  if (has('btn-play-all')) $('btn-play-all').onclick = () => {
    if (!info) return;
    client.loopTicks(0, info.endTick);
    client.setLoop(false);
    client.seekMs(0);
    client.play();
  };
  if (has('btn-stop')) $('btn-stop').onclick = () => client.stop();

  // ---------------- cutting ----------------
  function cutRegion(startTick, endTick, name) {
    if (!parentBytes) return Promise.reject(new Error('no track loaded'));
    return client.cutTrack(parentBytes, startTick, endTick, name);
  }
  async function addSlice(r = region, name = null) {
    if (!r) return null;
    name = (name || (has('slice-name') && $('slice-name').value) || 'slice').replace(/\W+/g, '_');
    try {
      const bytes = await cutRegion(r.startTick, r.endTick, name);
      const ms = tickToSample(r.endTick) / 48 - tickToSample(r.startTick) / 48;
      const s = { name, startTick: r.startTick, endTick: r.endTick, ms, bytes };
      slices.push(s);
      renderSlices();
      if (has('slice-name')) $('slice-name').value = '';
      log(`slice '${name}' added (${(bytes.byteLength / 1024).toFixed(1)} KB)`, 'ok');
      changed();
      return s;
    } catch (err) {
      log('cut failed: ' + err.message, 'err');
      return null;
    }
  }
  if (has('btn-add-slice')) $('btn-add-slice').onclick = () => addSlice();
  if (has('btn-xfade-region')) $('btn-xfade-region').onclick = async () => {
    if (!region) return;
    try {
      const bytes = await cutRegion(region.startTick, region.endTick, 'xfade_preview');
      client.crossfadeTrack(bytes, fadeMs()).catch((err) => log('[crossfade] ' + err.message, 'err'));
      log(`crossfading into region over ${fadeMs()} ms (loops there)`);
    } catch (err) {
      log('crossfade failed: ' + err.message, 'err');
    }
  };

  // ---------------- slice list ----------------
  function renderSlices() {
    const rows = $('slice-rows');
    if (!rows) return;
    rows.innerHTML = '';
    if (has('slice-table')) $('slice-table').style.display = slices.length ? '' : 'none';
    if (has('slice-empty')) $('slice-empty').style.display = slices.length ? 'none' : '';
    if (has('btn-export-all')) $('btn-export-all').disabled = !slices.length;
    slices.forEach((s, i) => {
      const tr = document.createElement('tr');
      tr.innerHTML = `
        <td><input type="text" value="${s.name}"></td>
        <td class="hint">bars ${barIndexOf(s.startTick) + 1}–${barIndexOf(Math.max(s.startTick, s.endTick - 1)) + 1}</td>
        <td class="hint">${fmtTime(s.ms)}</td>
        <td class="hint">${(s.bytes.byteLength / 1024).toFixed(1)} KB</td>
        <td>
          <button class="small" data-a="loop">▶ loop</button>
          <button class="small" data-a="xfade">⇄</button>
          <button class="small" data-a="export">⬇</button>
          <button class="small" data-a="del">×</button>
        </td>`;
      tr.querySelector('input').onchange = (e) => { s.name = e.target.value.replace(/\W+/g, '_'); changed(); };
      tr.querySelector('[data-a=loop]').onclick = () => {
        client.loopTicks(s.startTick, s.endTick);
        client.setLoop(true);
        client.seekMs(tickToSample(s.startTick) / 48);
        client.play();
        log(`looping slice '${s.name}' (region of the parent)`);
      };
      tr.querySelector('[data-a=xfade]').onclick = () => {
        client.crossfadeTrack(s.bytes, fadeMs()).catch((err) => log('[crossfade] ' + err.message, 'err'));
        log(`crossfading into slice '${s.name}' over ${fadeMs()} ms`);
      };
      tr.querySelector('[data-a=export]').onclick = () => download(s.name + '.mstk', s.bytes);
      tr.querySelector('[data-a=del]').onclick = () => { slices.splice(i, 1); renderSlices(); changed(); };
      rows.appendChild(tr);
    });
  }
  if (has('btn-export-all')) $('btn-export-all').onclick = () => {
    saveAllBytes(slices.map((s) => ({ name: s.name + '.mstk', data: s.bytes })));
    log(`exported ${slices.length} slice(s)`, 'ok');
  };

  if (opts.autoStart !== false) drawTimeline();
  return {
    loadTrackBytes,
    addSlice,
    cutRegion,
    setRegion,
    slices,
    get region() { return region; },
    get info() { return info; },
    get parentBytes() { return parentBytes; },
    get parentName() { return parentName; },
    tickToSample: (t) => (info ? tickToSample(t) : 0),
    start() { if (!raf) drawTimeline(); },
    stop() { cancelAnimationFrame(raf); raf = 0; },
  };
}
