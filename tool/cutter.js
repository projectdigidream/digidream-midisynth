//
// Track cutter: slices baked .mstk tracks into smaller gameplay-ready
// pieces. Runs on the same worklet/engine as the patch editor; cutting and
// baking happen in the WASM engine (state chase, note closing, canonical
// tempo re-anchoring), so what you export is exactly what you auditioned.
//
const $ = (id) => document.getElementById(id);
const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));

let port = null;
let parentBytes = null;    // the loaded .mstk (kept for cutting)
let parentName = 'track';
let info = null;           // trackInfo: bars, density, duration
let region = null;         // {startTick, endTick}
let status = { timeMs: 0, durationMs: 0 };
let cutBusy = null;        // single-flight cut: {resolve, reject}
const slices = [];         // {name, startTick, endTick, ms, bytes}

// ---------------- log ----------------
function log(msg, cls = '') {
  const el = document.createElement('div');
  if (cls) el.className = cls;
  el.textContent = msg;
  $('log').appendChild(el);
  $('log').scrollTop = $('log').scrollHeight;
  while ($('log').childNodes.length > 200) $('log').removeChild($('log').firstChild);
}

// ---------------- boot ----------------
$('start-btn').onclick = async () => {
  $('start-btn').disabled = true;
  try {
    const ctx = new AudioContext({ sampleRate: 48000 });
    await ctx.audioWorklet.addModule('./synth-worklet.js');
    const node = new AudioWorkletNode(ctx, 'midisynth', {
      numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2],
    });
    node.connect(ctx.destination);
    port = node.port;
    port.onmessage = (e) => onMessage(e.data);
    await ctx.resume();
    requestAnimationFrame(drawTimeline);
  } catch (err) {
    $('overlay').querySelector('p').textContent = 'Failed to start audio: ' + err;
    $('start-btn').disabled = false;
  }
};

function onMessage(m) {
  switch (m.type) {
    case 'ready':
      $('overlay').remove();
      log('engine ready — load a .mstk', 'ok');
      break;
    case 'report': {
      const r = m.report;
      for (const w of r.warnings || []) log(`[${m.what}] ${w}`, 'warn');
      for (const e of r.errors || []) log(`[${m.what}] ${e}`, 'err');
      if (m.what === 'track' && r.ok) {
        port.postMessage({ type: 'trackInfo' });
        log('track loaded', 'ok');
      }
      if (m.what === 'cut' && !r.ok && cutBusy) {
        cutBusy.reject(new Error((r.errors || []).join('; ')));
        cutBusy = null;
      }
      break;
    }
    case 'trackInfo':
      info = m.info;
      $('track-desc').textContent =
        `${parentName} · ${fmtTime(info.durationMs)} · ${info.barTicks.length} bars`;
      $('btn-play-all').disabled = false;
      setRegion(null);
      break;
    case 'cutBaked':
      if (cutBusy) {
        cutBusy.resolve(m.bytes);
        cutBusy = null;
      }
      break;
    case 'status':
      status = m;
      $('m-bar').textContent = m.bar + 1;
      $('m-beat').textContent = m.beat + 1;
      $('m-voices').textContent = m.activeVoices;
      $('t-cur').textContent = fmtTime(m.timeMs);
      break;
    case 'fatal':
      log('worklet failed: ' + m.error, 'err');
      break;
  }
}

function fmtTime(ms) {
  const s = ms / 1000;
  return `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, '0')}.${Math.floor((s % 1) * 10)}`;
}

// ---------------- track loading ----------------
$('track-file').onchange = async (e) => {
  const f = e.target.files[0];
  if (!f) return;
  const buf = await f.arrayBuffer();
  const u8 = new Uint8Array(buf);
  if (!(u8[0] === 0x4D && u8[1] === 0x53 && u8[2] === 0x54 && u8[3] === 0x4B)) {
    log(f.name + ' is not a .mstk track (bake one in the patch editor)', 'err');
    return;
  }
  parentBytes = buf.slice(0);
  parentName = f.name.replace(/\.mstk$/i, '');
  slices.length = 0;
  renderSlices();
  port.postMessage({ type: 'loadTrack', bytes: buf }, [buf]);
};
$('volume').oninput = (e) => port.postMessage({ type: 'masterVolume', milli: +e.target.value });

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
  if (!$('chk-snap').checked) return clamp(tick, 0, info.endTick);
  let best = info.barTicks[0], bd = Infinity;
  for (const t of info.barTicks) {
    const d = Math.abs(t - tick);
    if (d < bd) { bd = d; best = t; }
  }
  const dEnd = Math.abs(info.endTick - tick);
  return dEnd < bd ? info.endTick : best;
}

// ---------------- timeline ----------------
let dragStartTick = null;
function pxToTick(canvas, clientX) {
  const rect = canvas.getBoundingClientRect();
  const frac = clamp((clientX - rect.left) / rect.width, 0, 1);
  return sampleToTick(frac * endSample());
}
$('timeline').addEventListener('pointerdown', (e) => {
  if (!info) return;
  dragStartTick = snapTick(pxToTick($('timeline'), e.clientX));
  $('timeline').setPointerCapture(e.pointerId);
});
$('timeline').addEventListener('pointermove', (e) => {
  if (dragStartTick === null || !info) return;
  const t = snapTick(pxToTick($('timeline'), e.clientX));
  if (t !== dragStartTick) setRegion({ startTick: Math.min(dragStartTick, t), endTick: Math.max(dragStartTick, t) });
});
$('timeline').addEventListener('pointerup', (e) => {
  if (!info) return;
  const t = snapTick(pxToTick($('timeline'), e.clientX));
  if (dragStartTick !== null && t === dragStartTick) {
    // Click: move the playhead there (keeps current loop settings).
    port.postMessage({ type: 'seekMs', ms: Math.round(tickToSample(t) / 48) });
    port.postMessage({ type: 'play' });
  }
  dragStartTick = null;
});

function barIndexOf(tick) {
  let i = 0;
  while (i + 1 < info.barTicks.length && info.barTicks[i + 1] <= tick) i++;
  return i;
}
function setRegion(r) {
  region = r;
  const has = !!r;
  $('btn-preview').disabled = !has;
  $('btn-xfade-region').disabled = !has;
  $('btn-add-slice').disabled = !has;
  if (has) {
    const ms0 = tickToSample(r.startTick) / 48, ms1 = tickToSample(r.endTick) / 48;
    $('region-label').textContent =
      `bars ${barIndexOf(r.startTick) + 1}–${barIndexOf(Math.max(r.startTick, r.endTick - 1)) + 1}` +
      ` · ${fmtTime(ms0)}–${fmtTime(ms1)} · ${fmtTime(ms1 - ms0)}`;
    if (!$('slice-name').value)
      $('slice-name').value = `${parentName}_${barIndexOf(r.startTick) + 1}`;
  } else {
    $('region-label').textContent = 'no region';
  }
}

function drawTimeline() {
  requestAnimationFrame(drawTimeline);
  const c = $('timeline');
  const dpr = window.devicePixelRatio || 1;
  const w = c.clientWidth * dpr, h = c.clientHeight * dpr;
  if (c.width !== w || c.height !== h) { c.width = w; c.height = h; }
  const g = c.getContext('2d');
  g.fillStyle = '#101318';
  g.fillRect(0, 0, w, h);
  if (!info) {
    g.fillStyle = 'rgba(255,255,255,0.25)';
    g.font = `${12 * dpr}px ui-monospace`;
    g.fillText('load a .mstk track', 10 * dpr, 20 * dpr);
    return;
  }
  // Density strip.
  const d = info.density;
  let peak = 1;
  for (const v of d) peak = Math.max(peak, v);
  g.fillStyle = 'rgba(111,214,111,0.35)';
  const bw = w / d.length;
  for (let i = 0; i < d.length; i++) {
    const bh = (d[i] / peak) * (h * 0.72);
    g.fillRect(i * bw, h - bh, bw - 0.5 * dpr, bh);
  }
  // Bar grid (thin every bar, labeled every 4/8 depending on zoom).
  const xOfSample = (s) => (s / endSample()) * w;
  const labelEvery = info.barTicks.length > 120 ? 8 : 4;
  g.font = `${9 * dpr}px ui-monospace`;
  info.barTicks.forEach((t, i) => {
    const x = xOfSample(info.barSamples[i]);
    g.strokeStyle = i % labelEvery === 0 ? 'rgba(255,255,255,0.18)' : 'rgba(255,255,255,0.06)';
    g.beginPath(); g.moveTo(x, 0); g.lineTo(x, h); g.stroke();
    if (i % labelEvery === 0) {
      g.fillStyle = 'rgba(255,255,255,0.35)';
      g.fillText(String(i + 1), x + 2 * dpr, 10 * dpr);
    }
  });
  // Region.
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
  // Playhead.
  if (status.durationMs > 0) {
    const x = (status.timeMs / (info.durationMs || 1)) * w;
    g.strokeStyle = '#7bc67b';
    g.lineWidth = 1.5 * dpr;
    g.beginPath(); g.moveTo(x, 0); g.lineTo(x, h); g.stroke();
  }
}

// ---------------- transport ----------------
$('btn-preview').onclick = () => {
  if (!region) return;
  port.postMessage({ type: 'loopTicks', startTick: region.startTick, endTick: region.endTick });
  port.postMessage({ type: 'setLoop', on: true });
  port.postMessage({ type: 'seekMs', ms: Math.round(tickToSample(region.startTick) / 48) });
  port.postMessage({ type: 'play' });
  log(`looping region (${$('region-label').textContent})`);
};
$('btn-play-all').onclick = () => {
  port.postMessage({ type: 'loopTicks', startTick: 0, endTick: info.endTick });
  port.postMessage({ type: 'setLoop', on: false });
  port.postMessage({ type: 'seekMs', ms: 0 });
  port.postMessage({ type: 'play' });
};
$('btn-stop').onclick = () => port.postMessage({ type: 'stop' });

// ---------------- cutting ----------------
function cutRegion(startTick, endTick, name) {
  return new Promise((resolve, reject) => {
    if (cutBusy) { reject(new Error('cut already in progress')); return; }
    cutBusy = { resolve, reject };
    const buf = parentBytes.slice(0);
    port.postMessage({ type: 'cutTrack', bytes: buf, startTick, endTick, name }, [buf]);
  });
}

$('btn-add-slice').onclick = async () => {
  if (!region) return;
  const name = ($('slice-name').value || 'slice').replace(/\W+/g, '_');
  try {
    const bytes = await cutRegion(region.startTick, region.endTick, name);
    const ms = tickToSample(region.endTick) / 48 - tickToSample(region.startTick) / 48;
    slices.push({ name, startTick: region.startTick, endTick: region.endTick, ms, bytes });
    renderSlices();
    $('slice-name').value = '';
    log(`slice '${name}' added (${(bytes.byteLength / 1024).toFixed(1)} KB)`, 'ok');
  } catch (err) {
    log('cut failed: ' + err.message, 'err');
  }
};

$('btn-xfade-region').onclick = async () => {
  if (!region) return;
  try {
    const bytes = await cutRegion(region.startTick, region.endTick, 'xfade_preview');
    port.postMessage({ type: 'crossfadeTrack', bytes, fadeMs: +$('fade-ms').value },
                     [bytes]);
    log(`crossfading into region over ${$('fade-ms').value} ms (loops there)`);
  } catch (err) {
    log('crossfade failed: ' + err.message, 'err');
  }
};

// ---------------- slice list ----------------
function download(name, bytes) {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([bytes], { type: 'application/octet-stream' }));
  a.download = name + '.mstk';
  a.click();
}
function renderSlices() {
  const rows = $('slice-rows');
  rows.innerHTML = '';
  $('slice-table').style.display = slices.length ? '' : 'none';
  $('slice-empty').style.display = slices.length ? 'none' : '';
  $('btn-export-all').disabled = !slices.length;
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
    tr.querySelector('input').onchange = (e) => { s.name = e.target.value.replace(/\W+/g, '_'); };
    tr.querySelector('[data-a=loop]').onclick = () => {
      port.postMessage({ type: 'loopTicks', startTick: s.startTick, endTick: s.endTick });
      port.postMessage({ type: 'setLoop', on: true });
      port.postMessage({ type: 'seekMs', ms: Math.round(tickToSample(s.startTick) / 48) });
      port.postMessage({ type: 'play' });
      log(`looping slice '${s.name}' (region of the parent)`);
    };
    tr.querySelector('[data-a=xfade]').onclick = () => {
      const copy = s.bytes.slice(0);
      port.postMessage({ type: 'crossfadeTrack', bytes: copy, fadeMs: +$('fade-ms').value },
                       [copy]);
      log(`crossfading into slice '${s.name}' over ${$('fade-ms').value} ms`);
    };
    tr.querySelector('[data-a=export]').onclick = () => download(s.name, s.bytes);
    tr.querySelector('[data-a=del]').onclick = () => { slices.splice(i, 1); renderSlices(); };
    rows.appendChild(tr);
  });
}
$('btn-export-all').onclick = () => {
  for (const s of slices) download(s.name, s.bytes);
  log(`exported ${slices.length} slice(s)`, 'ok');
};
