//
// Fit panel: point it at a recorded sample and it searches for synth
// parameters whose render matches, then drops the result straight into the
// bank or the kit.
//
// Two things are worth understanding before turning knobs here:
//
//  * The ARCHITECTURE is fixed. Oscillator types, filter modes, layer count
//    and the wavetable recipe are chosen by the preset; only the continuous
//    knobs move. Those discrete choices are exactly the ones a gradient
//    cannot cross, which is why they are picked rather than searched.
//
//  * The LIKELIHOOD is yours to build. It decides what "best fit" even
//    means, and it matters more than any other setting: a pure spectrogram
//    term treats a near-silent bin as equal to the fundamental and will
//    happily settle on a sound in the wrong octaves. The band term is what
//    stops that. The preview shows each term separately so you can see which
//    part of the objective is unhappy.
//
import { scoped, canvas2d, clamp } from './ui.js';

export const FIT_PANEL_HTML = `
<section>
  <h2>Target</h2>
  <div class="row">
    <label class="hint">sample <input type="file" id="fit-file" accept="audio/*" style="width:200px"></label>
    <span class="hint" id="fit-target-info">no sample loaded</span>
    <button class="small" id="fit-play-target" disabled>play</button>
  </div>
  <canvas id="fit-wave" style="height:90px"></canvas>
</section>

<section>
  <h2>Architecture</h2>
  <div class="row">
    <label class="hint">preset</label><select id="fit-arch"></select>
    <label class="hint">window ms</label><input type="number" id="fit-window" class="narrow" min="0" max="4000" value="180"
      title="fit only the first N ms; a recording's room tail is not something the synth can reproduce">
    <label class="hint">iters</label><input type="number" id="fit-iters" class="narrow" min="5" max="400" value="60">
    <label class="hint">restarts</label><input type="number" id="fit-restarts" class="narrow" min="1" max="24" value="6"
      title="restarts buy more than iterations on this landscape: descent saturates after ~20 steps">
  </div>
  <span class="hint" id="fit-arch-params"></span>
</section>

<section>
  <h2>Likelihood</h2>
  <span class="hint">What the optimiser is actually minimising. Weights are multiplied onto each term.</span>
  <div class="knob"><label>spectrogram</label>
    <input type="range" id="fit-w-stft" min="0" max="200" value="100">
    <span class="val" id="fit-w-stft-v">1.00</span></div>
  <div class="knob"><label>envelope</label>
    <input type="range" id="fit-w-env" min="0" max="100" value="5">
    <span class="val" id="fit-w-env-v">0.05</span></div>
  <div class="knob"><label>band balance</label>
    <input type="range" id="fit-w-bands" min="0" max="100" value="4">
    <span class="val" id="fit-w-bands-v">0.04</span></div>
  <div class="row">
    <button class="small" data-preset="balanced">balanced</button>
    <button class="small" data-preset="spectral">spectral only</button>
    <button class="small" data-preset="tonal">tonal balance</button>
    <button class="small" data-preset="percussive">percussive</button>
  </div>
</section>

<section>
  <h2>Fit</h2>
  <div class="row">
    <button class="primary" id="fit-run" disabled>Fit</button>
    <button class="small" id="fit-stop" disabled>stop</button>
    <span class="hint" id="fit-status">load a sample to begin</span>
  </div>
  <div class="tbar" style="margin:6px 0"><div id="fit-bar" style="height:100%;width:0;background:var(--accent);border-radius:3px"></div></div>
  <div class="row">
    <span class="hint">loss</span><b id="fit-loss">—</b>
    <span class="hint">spectrogram</span><span class="val" id="fit-loss-stft">—</span>
    <span class="hint">envelope</span><span class="val" id="fit-loss-env">—</span>
    <span class="hint">bands</span><span class="val" id="fit-loss-bands">—</span>
  </div>
  <canvas id="fit-bands" style="height:120px"></canvas>
  <span class="hint">energy per band: target vs fit. A fit that scores well but sits in different bars is the loss being gamed, not a match.</span>
</section>

<section>
  <h2>Result</h2>
  <div class="row">
    <button class="small" id="fit-audition" disabled>audition fit</button>
    <button class="small" id="fit-audition-target" disabled>audition target</button>
    <span class="hint" id="fit-params"></span>
  </div>
  <div class="row">
    <label class="hint">add as</label>
    <select id="fit-dest">
      <option value="bank">bank program</option>
      <option value="kit">kit note</option>
    </select>
    <input type="number" id="fit-slot" class="narrow" min="0" max="127" value="38">
    <input type="text" id="fit-name" value="fitted" style="width:120px">
    <button class="primary" id="fit-add" disabled>Add to instruments</button>
  </div>
  <textarea id="fit-json" spellcheck="false" style="min-height:130px"></textarea>
</section>`;

const PRESETS = {
  balanced: { stft: 1.0, env: 0.05, bands: 0.04 },
  spectral: { stft: 1.0, env: 0.0, bands: 0.0 },
  tonal: { stft: 0.6, env: 0.02, bands: 0.12 },
  percussive: { stft: 1.0, env: 0.25, bands: 0.03 },
};
const BAND_LABELS = ['<35', '35-60', '60-120', '120-250', '250-600', '0.6-1.5k', '1.5-4k', '>4k'];

export function initFitPanel(root, ctx) {
  const $ = scoped(root);
  const { client, log, store } = ctx;
  let worker = null, target = null, targetRate = 48000, result = null, fitAudio = null;
  let running = false;

  const weights = () => ({
    stft: +$('fit-w-stft').value / 100,
    env: +$('fit-w-env').value / 100,
    bands: +$('fit-w-bands').value / 100,
  });
  const showWeights = () => {
    for (const k of ['stft', 'env', 'bands'])
      $(`fit-w-${k}-v`).textContent = (+$(`fit-w-${k}`).value / 100).toFixed(2);
  };
  for (const k of ['stft', 'env', 'bands']) $(`fit-w-${k}`).oninput = showWeights;
  root.querySelectorAll('[data-preset]').forEach((b) => {
    b.onclick = () => {
      const p = PRESETS[b.dataset.preset];
      $('fit-w-stft').value = p.stft * 100;
      $('fit-w-env').value = p.env * 100;
      $('fit-w-bands').value = p.bands * 100;
      showWeights();
    };
  });
  showWeights();

  function ensureWorker() {
    if (worker) return worker;
    worker = new Worker(new URL('../fit-worker.js', import.meta.url), { type: 'module' });
    worker.onmessage = (e) => onWorker(e.data);
    worker.onerror = (e) => { log('[fit] worker failed: ' + e.message, 'err'); setRunning(false); };
    worker.postMessage({ type: 'archs' });
    return worker;
  }

  function setRunning(on) {
    running = on;
    $('fit-run').disabled = on || !target;
    $('fit-stop').disabled = !on;
  }

  // ---------------- target ----------------
  $('fit-file').onchange = async (e) => {
    const f = e.target.files[0];
    if (!f) return;
    try {
      const buf = await f.arrayBuffer();
      // Decode at the engine's rate so no resampling guesswork is needed.
      const ac = new OfflineAudioContext(1, 1, 48000);
      const audio = await ac.decodeAudioData(buf);
      const ch = audio.numberOfChannels;
      const mono = new Float32Array(audio.length);
      for (let c = 0; c < ch; c++) {
        const d = audio.getChannelData(c);
        for (let i = 0; i < audio.length; i++) mono[i] += d[i] / ch;
      }
      target = mono;
      targetRate = audio.sampleRate;
      $('fit-target-info').textContent =
        `${f.name} — ${(audio.length / audio.sampleRate).toFixed(2)}s, ${ch}ch @ ${audio.sampleRate}`;
      $('fit-play-target').disabled = false;
      $('fit-run').disabled = false;
      $('fit-status').textContent = 'ready';
      drawWave(target, null);
      ensureWorker();
    } catch (err) {
      log('[fit] could not decode: ' + err.message, 'err');
    }
  };

  // ---------------- run ----------------
  $('fit-run').onclick = () => {
    if (!target || running) return;
    setRunning(true);
    $('fit-bar').style.width = '0%';
    $('fit-status').textContent = 'fitting…';
    ensureWorker().postMessage({
      type: 'fit',
      target,
      arch: $('fit-arch').value,
      weights: weights(),
      windowMs: +$('fit-window').value || 0,
      iters: +$('fit-iters').value || 60,
      restarts: +$('fit-restarts').value || 6,
      seed: 1,
    }, [/* target is reused, so no transfer */]);
  };
  $('fit-stop').onclick = () => {
    if (worker) { worker.terminate(); worker = null; }
    setRunning(false);
    $('fit-status').textContent = 'stopped';
  };

  function onWorker(m) {
    if (m.type === 'archs') {
      const sel = $('fit-arch');
      sel.innerHTML = m.archs.map((a) => `<option value="${a.name}">${a.name}</option>`).join('');
      sel.onchange = () => {
        const a = m.archs.find((x) => x.name === sel.value);
        $('fit-arch-params').textContent =
          `${a.params.length} free parameters: ` + a.params.map((p) => p.name).join(', ');
      };
      sel.onchange();
    } else if (m.type === 'progress') {
      $('fit-bar').style.width = Math.round(100 * m.restart / m.of) + '%';
      $('fit-status').textContent = `restart ${m.restart}/${m.of} — best ${m.best.toFixed(4)}`;
    } else if (m.type === 'done') {
      result = m.best;
      fitAudio = m.fit;
      setRunning(false);
      $('fit-bar').style.width = '100%';
      $('fit-status').textContent = 'done';
      showResult(m.best, m.detail, m.fit, m.target);
    } else if (m.type === 'analyzed') {
      fitAudio = m.fit;
      showLoss(m.detail);
      drawBands(m.detail);
    } else if (m.type === 'error') {
      log('[fit] ' + m.error, 'err');
      setRunning(false);
      $('fit-status').textContent = 'failed';
    }
  }

  function showLoss(d) {
    $('fit-loss').textContent = d.loss.toFixed(4);
    $('fit-loss-stft').textContent = d.stft.toFixed(3);
    $('fit-loss-env').textContent = d.env.toFixed(2);
    $('fit-loss-bands').textContent = d.bands.toFixed(1);
  }

  function showResult(best, detail, fit, tgt) {
    showLoss(detail);
    drawBands(detail);
    drawWave(tgt, fit);
    $('fit-params').textContent = Object.entries(best.params)
      .map(([k, v]) => `${k} ${(+v).toFixed(v > 20 ? 0 : 2)}`).join(' · ');
    $('fit-json').value = JSON.stringify(best.patch, null, 1);
    $('fit-audition').disabled = false;
    $('fit-audition-target').disabled = false;
    $('fit-add').disabled = false;
    log(`[fit] best loss ${best.loss.toFixed(4)}`, 'ok');
  }

  // ---------------- preview ----------------
  function drawWave(tgt, fit) {
    const { g, w, h, dpr } = canvas2d($('fit-wave'));
    g.clearRect(0, 0, w, h);
    const line = (data, colour, yOff) => {
      if (!data || !data.length) return;
      g.strokeStyle = colour;
      g.lineWidth = dpr;
      g.beginPath();
      const step = Math.max(1, Math.floor(data.length / w));
      for (let x = 0; x < w; x++) {
        let peak = 0;
        for (let i = x * step; i < (x + 1) * step && i < data.length; i++)
          peak = Math.max(peak, Math.abs(data[i]));
        const y = yOff - peak * (h / 2 - 2 * dpr);
        if (x === 0) g.moveTo(x, y); else g.lineTo(x, y);
      }
      g.stroke();
    };
    line(tgt, '#8a8f9c', h / 2);
    line(fit, '#64b5f6', h / 2);
    g.fillStyle = '#8a8f9c';
    g.font = `${11 * dpr}px ui-monospace, monospace`;
    g.fillText('target', 4 * dpr, 12 * dpr);
    if (fit) { g.fillStyle = '#64b5f6'; g.fillText('fit', 52 * dpr, 12 * dpr); }
  }

  function drawBands(d) {
    const { g, w, h, dpr } = canvas2d($('fit-bands'));
    g.clearRect(0, 0, w, h);
    const n = d.target.length;
    const bw = w / n;
    const top = Math.max(1, ...d.target, ...d.candidate);
    for (let i = 0; i < n; i++) {
      const x = i * bw;
      const ht = (d.target[i] / top) * (h - 22 * dpr);
      const hc = (d.candidate[i] / top) * (h - 22 * dpr);
      g.fillStyle = '#4a5160';
      g.fillRect(x + bw * 0.12, h - 16 * dpr - ht, bw * 0.36, ht);
      g.fillStyle = '#64b5f6';
      g.fillRect(x + bw * 0.52, h - 16 * dpr - hc, bw * 0.36, hc);
      g.fillStyle = '#8a8f9c';
      g.font = `${9 * dpr}px ui-monospace, monospace`;
      g.fillText(BAND_LABELS[i] || '', x + 2 * dpr, h - 4 * dpr);
    }
  }

  // ---------------- audition ----------------
  const play = (data) => {
    if (!data || !data.length || !client.ctx) return;
    const buf = client.ctx.createBuffer(1, data.length, 48000);
    buf.copyToChannel(data instanceof Float32Array ? data : new Float32Array(data), 0);
    const src = client.ctx.createBufferSource();
    src.buffer = buf;
    src.connect(client.ctx.destination);
    src.start();
  };
  $('fit-audition').onclick = () => play(fitAudio);
  $('fit-audition-target').onclick = () => play(target);
  $('fit-play-target').onclick = () => play(target);

  // Re-score an edited patch against the same target and weights.
  $('fit-json').onchange = () => {
    if (!worker) return;
    try {
      JSON.parse($('fit-json').value);
      worker.postMessage({ type: 'analyze', patch: $('fit-json').value });
    } catch (err) {
      log('[fit] patch JSON is not valid: ' + err.message, 'err');
    }
  };

  // ---------------- into the instrument roster ----------------
  $('fit-dest').onchange = () => {
    $('fit-slot').value = $('fit-dest').value === 'kit' ? 38 : 39;
  };
  $('fit-add').onclick = () => {
    let patch;
    try { patch = JSON.parse($('fit-json').value); }
    catch (err) { log('[fit] patch JSON is not valid: ' + err.message, 'err'); return; }
    patch.name = $('fit-name').value || 'fitted';
    const slot = clamp(+$('fit-slot').value | 0, 0, 127);
    const kit = $('fit-dest').value === 'kit';
    if (kit && patch.oneShot === undefined) patch.oneShot = true;
    store.mutate(kit ? 'fit -> kit' : 'fit -> bank', (p) => {
      if (kit) {
        p.kit = p.kit ? JSON.parse(JSON.stringify(p.kit)) : { version: 1, notes: {} };
        p.kit.notes = p.kit.notes || {};
        p.kit.notes[String(slot)] = patch;
      } else {
        p.bank = p.bank ? JSON.parse(JSON.stringify(p.bank)) : { version: 1, programs: {} };
        p.bank.programs = p.bank.programs || {};
        p.bank.programs[String(slot)] = patch;
      }
    }, { audio: false, content: true });
    log(`[fit] added '${patch.name}' as ${kit ? 'kit note' : 'program'} ${slot} — it is in the roster now`, 'ok');
  };

  ensureWorker();

  return {
    activate() { if (target) drawWave(target, fitAudio); },
    deactivate() {},
  };
}
