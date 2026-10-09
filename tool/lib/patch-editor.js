//
// Patch editor panel: structured controls <-> patch object <-> live apply
// (single-program hot swap; only voices on the edited patch restart), with
// draggable envelope editors, wavetable/layer previews, filter response,
// A/B, undo, JSON drawer, import/export and an audition keyboard / drum pads.
//
// Hosted inside `root` (a page or a DAW tab): every lookup is root-scoped.
// `store` is the shared {bankDoc, kitDoc} the whole page edits.
//
//   const editor = initPatchEditor(root, client, store, { log, getAuditionChannel });
//   editor.activate();            // keyboard shortcuts on (per visible tab)
//   editor.selectProgram(48);     // navigate (instruments panel, DAW track)
//   editor.midiMessage(bytes);    // route a MIDI device through the audition path
//
import { GM_NAMES, DRUM_NAMES } from './gm.js';
import { clamp, scoped, download, canvas2d } from './ui.js';

export const PATCH_EDITOR_HTML = `
<div class="pe">
  <div class="row">
    <select id="edit-mode">
      <option value="bank">melodic program</option>
      <option value="kit">drum note</option>
    </select>
    <select id="edit-target" style="flex:1"></select>
    <span id="apply-state"></span>
  </div>
  <div class="row">
    <button id="btn-undo" class="small" title="undo last applied edit (Ctrl+Z)">↶ Undo</button>
    <button id="btn-redo" class="small" title="redo (Ctrl+Shift+Z / Ctrl+Y)">↷ Redo</button>
    <button id="btn-ab-store" class="small" title="snapshot the current patch as B">Store B</button>
    <button id="btn-ab-swap" class="small" title="swap working patch with the B snapshot">A⇄B</button>
    <button id="btn-init" class="small" title="reset to a plain init patch">Init</button>
    <span class="hint" id="ab-state"></span>
  </div>
  <div class="row">
    <label>name</label><input type="text" id="p-name" style="flex:1">
    <span id="drum-flags" style="display:none">
      <label><input type="checkbox" id="p-oneshot"> oneShot</label>
      <label>choke</label><input type="number" id="p-choke" class="narrow" min="-1" max="15" value="-1">
    </span>
  </div>
  <fieldset>
    <legend>Layer mix — how the layers combine</legend>
    <canvas id="layers-canvas" style="height:110px"></canvas>
    <div class="row" id="layers-legend"></div>
    <canvas id="layers-env" style="height:74px" title="amp envelopes of all layers"></canvas>
  </fieldset>
  <div class="tabs" id="layer-tabs"></div>
  <div id="layer-panel">
    <fieldset>
      <legend>Oscillator</legend>
      <div class="row">
        <select id="l-osc"><option value="wavetable">wavetable</option><option value="noise">noise</option><option value="metal">metal</option></select>
        <span id="wt-controls">
          <label>classic</label>
          <select id="wt-classic" title="subtractive starting points: sets the generator + parameters">
            <option value="">—</option><option value="sine">sine</option><option value="triangle">triangle</option>
            <option value="saw">saw</option><option value="square">square</option>
            <option value="pulse25">pulse 25%</option><option value="pulse12">pulse 12%</option>
          </select>
          <select id="wt-gen">
            <option value="custom">custom (draw)</option>
            <option value="additiveHarmonicMorph">additive</option>
            <option value="pulse">pulse (PWM)</option>
            <option value="drawbar">drawbar organ</option>
            <option value="waveshaper">waveshaper</option>
            <option value="chebyshev">chebyshev</option>
            <option value="harmonics">harmonics</option>
            <option value="hardsync">hard sync</option>
            <option value="fm2op">FM 2-op</option>
          </select>
          <label>frames</label>
          <input type="number" id="wt-frames" class="narrow" min="1" max="32" value="1">
        </span>
        <span id="noise-hint" class="hint" style="display:none">white noise — shape it with the filter below</span>
      </div>
      <div class="row" id="gen-params"></div>
      <canvas id="wt-preview"></canvas>
      <div class="row" id="wt-morph-row">
        <label>morph preview</label>
        <input type="range" id="wt-morph" min="0" max="1000" value="0" style="flex:1">
        <span class="val" id="wt-morph-val">frame 0</span>
      </div>
    </fieldset>
    <fieldset>
      <legend>Mix &amp; pitch</legend>
      <div class="row">
        <label>gain</label><input type="range" id="l-gain" min="0" max="1000" value="600" style="flex:1">
        <span class="val" id="l-gain-val">0.60</span>
        <label>pan</label><input type="range" id="l-pan" min="-1000" max="1000" value="0" style="flex:1">
        <span class="val" id="l-pan-val">C</span>
      </div>
      <div class="row">
        <label>detune ¢</label><input type="number" id="l-detune" step="0.5" value="0">
        <label>delay ms</label><input type="number" id="l-delay" class="narrow" min="0" max="2000" step="1" value="0"
               title="the layer speaks this much later — flams, claps, stacked strikes">
        <label>vel curve</label>
        <select id="l-velcurve"><option value="squared">squared</option><option value="soft">soft</option><option value="linear">linear</option></select>
        <label>vel</label><input type="number" id="l-vlow" class="narrow" min="0" max="127" value="0">
        <label>–</label><input type="number" id="l-vhigh" class="narrow" min="0" max="127" value="127">
        <span id="pitchnote-wrap">
          <label>fixed pitch (note#)</label>
          <input type="number" id="l-pitchnote" class="narrow" step="0.5" min="-1" max="127" value="-1">
          <span class="hint">−1 = follow played note</span>
        </span>
      </div>
      <div class="row">
        <label>unison</label>
        <input type="number" id="l-unison" class="narrow" min="1" max="7" step="1" value="1" title="voice stack">
        <label>detune ¢</label><input type="number" id="l-unidet" class="narrow" min="0" max="100" step="0.5" value="7">
        <label>spread</label><input type="number" id="l-unispread" class="narrow" min="0" max="1" step="0.05" value="0.5">
        <label>ring ×</label><input type="number" id="l-ringratio" class="narrow" min="0" max="16" step="0.05" value="0"
               title="ring-mod carrier ratio (0 = off)">
        <label>amt</label><input type="number" id="l-ringamt" class="narrow" min="0" max="1" step="0.05" value="1">
        <label>rel-vel</label><input type="number" id="l-relvel" class="narrow" min="0" max="1" step="0.05" value="0"
               title="release velocity scales release time">
        <label>RR</label><input type="number" id="l-rr" class="narrow" min="0" max="15" step="1" value="0"
               title="round-robin group: layers sharing a group alternate per note">
      </div>
      <div class="row">
        <label>sends (blank = channel CC)</label>
        <label>rev</label><input type="number" id="l-sendrev" class="narrow" min="0" max="1" step="0.05" placeholder="ch"
               title="per-layer reverb send override — a snare wetter than the kick on the same channel">
        <label>cho</label><input type="number" id="l-sendcho" class="narrow" min="0" max="1" step="0.05" placeholder="ch">
        <label>dly</label><input type="number" id="l-senddel" class="narrow" min="0" max="1" step="0.05" placeholder="ch">
      </div>
    </fieldset>
    <fieldset>
      <legend>Envelopes <span class="hint">— drag the handles</span></legend>
      <div class="subtabs" id="env-tabs"></div>
      <canvas id="env-canvas"></canvas>
      <div class="row">
        <label>dly</label><input type="number" id="env-dl" class="narrow" min="0" max="10000" value="0" title="pre-attack delay ms">
        <label>A</label><input type="number" id="env-a" min="0" max="10000" value="0">
        <label>H</label><input type="number" id="env-h" class="narrow" min="0" max="10000" value="0" title="hold at full level ms">
        <label>D</label><input type="number" id="env-d" min="0" max="10000" value="0">
        <label>S</label><input type="number" id="env-s" min="0" max="1" step="0.01" value="1">
        <label>R</label><input type="number" id="env-r" min="0" max="10000" value="0">
        <label><input type="checkbox" id="env-loop" title="attack↔decay cycles while held"> loop</label>
        <span class="hint">ms (DAHDSR)</span>
      </div>
      <div class="row">
        <label>curves</label>
        <select id="env-ca"><option>linear</option><option>exp</option><option>expAttack</option><option>sCurve</option></select>
        <select id="env-cd"><option>linear</option><option>exp</option><option>expAttack</option><option>sCurve</option></select>
        <select id="env-cr"><option>linear</option><option>exp</option><option>expAttack</option><option>sCurve</option></select>
        <span id="env-depth-wrap"><label id="env-depth-label">depth</label>
          <input type="number" id="env-depth" step="0.05" value="0"></span>
      </div>
    </fieldset>
    <fieldset>
      <legend>Filter</legend>
      <div class="row">
        <select id="f-mode"><option value="bypass">bypass</option><option value="lowpass">lowpass</option><option value="bandpass">bandpass</option><option value="highpass">highpass</option><option value="notch">notch</option><option value="comb">comb</option><option value="formant">formant</option></select>
        <span id="vowel-wrap" style="display:none">
          <label>vowel</label>
          <select id="f-vowel"><option>a</option><option>e</option><option>i</option><option>o</option><option>u</option></select>
          <label>→</label>
          <select id="f-vowel2"><option value="">—</option><option>a</option><option>e</option><option>i</option><option>o</option><option>u</option></select>
          <span class="hint">filter env morphs vowel → end</span>
        </span>
        <label>cutoff</label><input type="range" id="f-cut" min="0" max="1000" value="600" style="flex:1">
        <span class="val" id="f-cut-val">1200 Hz</span>
      </div>
      <div class="row">
        <label>resonance</label><input type="range" id="f-res" min="0" max="950" value="0" style="flex:1">
        <span class="val" id="f-res-val">0.00</span>
        <label>keytrack</label><input type="range" id="f-key" min="0" max="1000" value="0" style="flex:1">
        <span class="val" id="f-key-val">0.00</span>
      </div>
      <div class="row">
        <label>slope</label>
        <select id="f-slope"><option value="12">12 dB</option><option value="24">24 dB</option></select>
        <label>drive</label><input type="range" id="f-drive" min="1000" max="8000" value="1000" style="flex:1" title="pre-filter saturation (1 = off)">
        <span class="val" id="f-drive-val">off</span>
        <label>post</label><input type="range" id="f-post" min="1000" max="8000" value="1000" style="flex:1" title="post-filter saturation — distorts the filtered signal (not band-limited)">
        <span class="val" id="f-post-val">off</span>
      </div>
      <div class="row">
        <label>filter 2</label>
        <select id="f2-mode"><option value="bypass">bypass</option><option value="lowpass">lowpass</option><option value="bandpass">bandpass</option><option value="highpass">highpass</option><option value="notch">notch</option></select>
        <label>cutoff</label><input type="number" id="f2-cut" class="narrow" min="20" max="12000" step="10" value="200">
        <label>res</label><input type="number" id="f2-res" class="narrow" min="0" max="0.95" step="0.05" value="0">
        <select id="f-routing" title="how the two filters combine"><option value="serial">serial</option><option value="parallel">parallel</option></select>
        <label><input type="checkbox" id="f2-static" title="fixed corner: filter 2 ignores envelope/matrix/CC74/key-tracking (a cabinet low-cut)"> static</label>
        <span class="hint">serial HP+LP with independent corners, or parallel paths</span>
      </div>
      <canvas id="filter-resp" style="height:70px"></canvas>
    </fieldset>
    <fieldset><legend>LFOs</legend><div id="lfo-rows"></div></fieldset>
    <fieldset><legend>Mod matrix</legend><div id="mod-rows"></div><button id="mod-add" class="small">+ routing</button></fieldset>
  </div>
  <div class="row">
    <button id="btn-export-patch">Export patch</button>
    <button id="btn-export-doc">Export bank</button>
    <label style="margin-left:8px">import <input type="file" id="import-file" accept=".json" style="width:180px"></label>
    <button id="btn-json" style="margin-left:auto">JSON ▾</button>
  </div>
  <div id="json-drawer">
    <textarea id="editor" spellcheck="false"></textarea>
    <div class="row"><button id="btn-json-apply" class="primary">Apply JSON</button>
      <span class="hint">two-way: fields ↔ JSON</span></div>
  </div>
  <fieldset>
    <legend>Audition</legend>
    <div class="row">
      <label>velocity</label><input type="range" id="aud-vel" min="1" max="127" value="100" style="flex:1">
      <label>octave</label>
      <select id="aud-oct"><option>2</option><option>3</option><option value="4" selected>4</option><option>5</option><option>6</option></select>
      <label><input type="checkbox" id="aud-sus"> sustain</label>
      <button id="btn-hit" class="small" style="display:none">Hit drum</button>
    </div>
    <div class="row">
      <label>mod</label><input type="range" id="aud-mod" min="0" max="127" value="0" style="flex:1">
      <label>bend</label><input type="range" id="aud-bend" min="-8192" max="8191" value="0" style="flex:1">
    </div>
    <div id="kbd"></div>
    <div class="row"><span class="hint" id="aud-hint">A–K white · W/E/T/Y/U black · Z/X octave · keys audition the edited patch</span></div>
  </fieldset>
</div>`;

const MOD_SOURCES = ["LFO1","LFO2","LFO3","LFO4","AmpEnv","FilterEnv","PitchEnv","WtPosEnv","Velocity","KeyTrack","ModWheel","ChannelAftertouch","PolyAftertouch"];
const MOD_DESTS = ["Pitch","FilterCutoff","FilterRes","Amplitude","WtPosition","Pan","FilterDrive","PostDrive"];
const CURVES = {
  linear: (p) => p,
  exp: (p) => (1 - Math.exp(-6 * p)) / (1 - Math.exp(-6)),
  expAttack: (p) => (Math.exp(6 * p) - 1) / (Math.exp(6) - 1),
  sCurve: (p) => 3 * p * p - 2 * p * p * p,
};
const ENV_KINDS = [
  { key: 'ampEnv', label: 'Amp', depth: null },
  { key: 'filterEnv', label: 'Filter', depth: 'depth', dlabel: 'depth ±1' },
  { key: 'pitchEnv', label: 'Pitch', depth: 'depthSemis', dlabel: 'semis' },
  { key: 'wtPosEnv', label: 'WtPos', depth: 'depth', dlabel: 'depth 0-1' },
];
const GEN_SPECS = {
  additiveHarmonicMorph: [
    { key: 'falloff', label: 'falloff', type: 'sel', options: ['saw', 'square', 'triangle', 'flat'], def: 'saw' },
    { key: 'startHarmonics', label: 'harmonics', type: 'num', min: 1, max: 366, step: 1, def: 16 },
    { key: 'endHarmonics', label: '→ (last frame)', type: 'num', min: 1, max: 366, step: 1, def: 16 },
  ],
  pulse: [
    { key: 'startDuty', label: 'duty', type: 'num', min: 0.02, max: 0.98, step: 0.01, def: 0.5 },
    { key: 'endDuty', label: '→', type: 'num', min: 0.02, max: 0.98, step: 0.01, def: 0.15 },
  ],
  drawbar: [
    { key: 'drawbars', label: 'bars 16′..1′', type: 'bars', def: [8, 0, 6, 0, 0, 4, 0, 0, 0] },
  ],
  waveshaper: [
    { key: 'shape', label: 'shape', type: 'sel', options: ['soft', 'hard', 'fold', 'crush', 'stairs'], def: 'soft' },
    { key: 'input', label: 'input', type: 'sel', options: ['sine', 'triangle', 'saw'], def: 'sine' },
    { key: 'startDrive', label: 'drive', type: 'num', min: 0.25, max: 64, step: 0.25, def: 1 },
    { key: 'endDrive', label: '→', type: 'num', min: 0.25, max: 64, step: 0.25, def: 8 },
    { key: 'bias', label: 'bias', type: 'num', min: -1, max: 1, step: 0.05, def: 0 },
    { key: 'levels', label: 'levels', type: 'num', min: 2, max: 32, step: 1, def: 6, onlyShape: 'crush' },
    { key: 'steps', label: 'steps', type: 'num', min: 2, max: 64, step: 1, def: 16, onlyShape: 'stairs' },
  ],
  custom: [
    { key: 'samples', label: 'draw one cycle', type: 'drawwave', def: null },
  ],
  chebyshev: [
    { key: 'weights', label: 'transfer curve — drag the points', type: 'chebpts', def: [1, 0, 0, 0, 0, 0, 0, 0] },
    { key: 'startDrive', label: 'drive', type: 'num', min: 0, max: 1, step: 0.05, def: 1 },
    { key: 'endDrive', label: '→', type: 'num', min: 0, max: 1, step: 0.05, def: 1 },
  ],
  harmonics: [
    { key: 'amps', label: 'harmonics 1..32 — drag to draw', type: 'hist', def: [1, 0, 0, 0, 0, 0, 0, 0] },
  ],
  hardsync: [
    { key: 'slave', label: 'slave', type: 'sel', options: ['saw', 'pulse'], def: 'saw' },
    { key: 'startRatio', label: 'ratio', type: 'num', min: 1, max: 16, step: 0.05, def: 1 },
    { key: 'endRatio', label: '→', type: 'num', min: 1, max: 16, step: 0.05, def: 3 },
  ],
  fm2op: [
    { key: 'ratio', label: 'ratio', type: 'num', min: 1, max: 16, step: 1, def: 2 },
    { key: 'startIndex', label: 'index', type: 'num', min: 0, max: 12, step: 0.05, def: 0.5 },
    { key: 'endIndex', label: '→', type: 'num', min: 0, max: 12, step: 0.05, def: 4 },
  ],
};
const LAYER_COLORS = ['#64b5f6', '#e0b060', '#7bc67b', '#d678d6'];
const KEYMAP = { a: 0, w: 1, s: 2, e: 3, d: 4, f: 5, t: 6, g: 7, y: 8, h: 9, u: 10, j: 11, k: 12, o: 13, l: 14 };

// ---- float preview mirrors of the engine's generators ----
function harmAmp(falloff, h) {
  switch (falloff) {
    case 'saw': return 1 / h;
    case 'square': return h % 2 ? 1 / h : 0;
    case 'triangle': return h % 2 ? (((h >> 1) % 2) ? -1 : 1) / (h * h) : 0;
    default: return 1;
  }
}
function normalize(out) {
  let peak = 1e-9;
  for (const v of out) peak = Math.max(peak, Math.abs(v));
  for (let i = 0; i < out.length; i++) out[i] /= peak;
  return out;
}
const lerpNum = (a, b, t) => a + (b - a) * t;
export function genWave(gen, P, t, n = 512) {
  const out = new Float64Array(n);
  P = P || {};
  switch (gen) {
    case 'pulse': {
      const duty = lerpNum(P.startDuty ?? 0.5, P.endDuty ?? 0.15, t);
      const mean = 2 * duty - 1;
      for (let i = 0; i < n; i++) out[i] = (i / n < duty ? 1 : -1) - mean;
      break;
    }
    case 'drawbar': {
      const BAR_H = [1, 2, 3, 4, 5, 6, 8, 10, 12];
      const bars = P.drawbars || [8, 0, 6, 0, 0, 4, 0, 0, 0];
      const end = P.endDrawbars || bars;
      for (let b = 0; b < 9; b++) {
        const v = lerpNum(bars[b] ?? 0, end[b] ?? 0, t);
        const a = (v / 8) * (v / 8);
        if (!a) continue;
        for (let i = 0; i < n; i++) out[i] += a * Math.sin((2 * Math.PI * BAR_H[b] * i) / n);
      }
      break;
    }
    case 'custom': {
      const s = P.samples;
      if (!Array.isArray(s) || s.length < 2) { for (let i = 0; i < n; i++) out[i] = Math.sin((2 * Math.PI * i) / n); break; }
      const e = Array.isArray(P.endSamples) && P.endSamples.length > 1 ? P.endSamples : s;
      const at = (arr, i) => {
        const pos = (i / n) * arr.length;
        const i0 = Math.floor(pos) % arr.length, i1 = (i0 + 1) % arr.length;
        return arr[i0] + (arr[i1] - arr[i0]) * (pos - Math.floor(pos));
      };
      for (let i = 0; i < n; i++) out[i] = lerpNum(at(s, i), at(e, i), t);
      let mean = 0;
      for (const v of out) mean += v / n;
      for (let i = 0; i < n; i++) out[i] -= mean;
      break;
    }
    case 'harmonics': {
      const a = P.amps || [1];
      const e = P.endAmps || a;
      for (let h = 1; h <= Math.max(a.length, e.length); h++) {
        const amp = lerpNum(a[h - 1] ?? 0, e[h - 1] ?? 0, t);
        if (!amp) continue;
        for (let i = 0; i < n; i++) out[i] += amp * Math.sin((2 * Math.PI * h * i) / n);
      }
      break;
    }
    case 'hardsync': {
      const ratio = lerpNum(P.startRatio ?? 1, P.endRatio ?? 3, t);
      const pulse = P.slave === 'pulse';
      for (let i = 0; i < n; i++) {
        const ph = ((i / n) * ratio) % 1;
        out[i] = pulse ? (ph < 0.5 ? 1 : -1) : 2 * ph - 1;
      }
      let mean = 0;
      for (const v of out) mean += v / n;
      for (let i = 0; i < n; i++) out[i] -= mean;
      break;
    }
    case 'chebyshev': {
      const d = lerpNum(P.startDrive ?? 1, P.endDrive ?? P.startDrive ?? 1, t);
      const w = P.weights || [1, 0, 0, 0, 0, 0, 0, 0];
      const e = P.endWeights || w;
      for (let i = 0; i < n; i++) {
        const x = d * Math.cos((2 * Math.PI * i) / n);
        let tkm1 = 1, tk = x, acc = 0;
        for (let k = 0; k < 8; k++) {
          const wk = lerpNum(w[k] ?? 0, e[k] ?? 0, t);
          if (wk) acc += wk * tk;
          const tn = 2 * x * tk - tkm1;
          tkm1 = tk; tk = tn;
        }
        out[i] = acc;
      }
      let mean = 0;
      for (const v of out) mean += v / n;
      for (let i = 0; i < n; i++) out[i] -= mean;
      break;
    }
    case 'waveshaper': {
      const drive = lerpNum(P.startDrive ?? 1, P.endDrive ?? 8, t);
      const shape = P.shape || 'soft';
      const bias = P.bias ?? 0;
      const src = (ph) => {
        const u = ph / (2 * Math.PI);  // 0..1
        if (P.input === 'triangle') return u < 0.5 ? 4 * u - 1 : 3 - 4 * u;
        if (P.input === 'saw') return 2 * u - 1;
        return Math.sin(ph);
      };
      for (let i = 0; i < n; i++) {
        let x = drive * src((2 * Math.PI * i) / n) + bias;
        if (shape === 'hard') x = Math.max(-1, Math.min(1, x));
        else if (shape === 'fold') {
          for (let g = 0; g < 64; g++) {
            if (x > 1) x = 2 - x;
            else if (x < -1) x = -2 - x;
            else break;
          }
        } else if (shape === 'crush') {
          const q = 1 / (P.levels ?? 6);
          x = Math.round(Math.max(-1, Math.min(1, x)) / q) * q;
        } else if (shape === 'stairs') {
          const st = P.steps ?? 16;
          const held = Math.floor((i / n) * st) / st;
          x = drive * src(2 * Math.PI * held) + bias;
          x = Math.max(-1, Math.min(1, x));
        } else {
          x = Math.abs(x) > 1 ? Math.sign(x) : 1.5 * x - 0.5 * x * x * x;
        }
        out[i] = x;
      }
      if (bias || P.input === 'saw') {
        let mean = 0;
        for (const v of out) mean += v / n;
        for (let i = 0; i < n; i++) out[i] -= mean;
      }
      break;
    }
    case 'fm2op': {
      const I = lerpNum(P.startIndex ?? 0.5, P.endIndex ?? 4, t);
      const r = Math.max(1, Math.round(P.ratio ?? 2));
      for (let i = 0; i < n; i++) {
        const ph = (2 * Math.PI * i) / n;
        out[i] = Math.sin(ph + I * Math.sin(r * ph));
      }
      break;
    }
    default: {
      const s = P.startHarmonics ?? 1, e = P.endHarmonics ?? 8;
      const harm = Math.max(1, Math.round(lerpNum(s, e, t)));
      for (let h = 1; h <= harm; h++) {
        const a = harmAmp(P.falloff || 'saw', h);
        if (!a) continue;
        for (let i = 0; i < n; i++) out[i] += a * Math.sin((2 * Math.PI * h * i) / n);
      }
    }
  }
  return normalize(out);
}
function panLabel(p) { return Math.abs(p) < 0.005 ? 'C' : (p < 0 ? `L${Math.round(-p * 100)}` : `R${Math.round(p * 100)}`); }

export function initPatchEditor(root, client, store, opts = {}) {
  const $ = scoped(root);
  const log = opts.log || ((m) => console.log(m));
  const getAuditionChannel = opts.getAuditionChannel || (() => (isKit() ? 9 : 0));

  let patch = null;
  let layerIdx = 0;
  let envKind = 0;
  const heldNotes = new Set();
  const keysDown = new Set();
  let padNotes = [];
  let applyTimer = null;
  // History per edit target. One shared stack used to scan past entries for
  // other programs and drop them on the way, so undoing on one patch quietly
  // ate the history of every other.
  const undoStacks = new Map();
  const redoStacks = new Map();
  let abSnapshot = null;
  let envDrag = null;
  let active = false;

  const isKit = () => $('edit-mode').value === 'kit';
  const isKitSafe = () => { try { return isKit() && !!store.kitDoc; } catch { return false; } };
  const targetKey = () => $('edit-target').value;

  function setApplyState(text, cls) {
    const el = $('apply-state');
    el.textContent = text;
    el.className = cls || '';
  }

  // ---------------- patch model ----------------
  function defaultEnv(kind) {
    if (kind === 'ampEnv') return { attack: 1, decay: 0, sustain: 1.0, release: 50 };
    return { attack: 0, decay: 0, sustain: 1.0, release: 50 };
  }
  function defaultLayer() {
    return {
      wavetable: { generator: 'additiveHarmonicMorph', frameCount: 1,
                   params: { startHarmonics: 16, endHarmonics: 16, falloff: 'saw' } },
      mixGain: 0.6,
      ampEnv: defaultEnv('ampEnv'),
    };
  }
  function defaultPatch() { return { name: 'new', layers: [defaultLayer()] }; }
  function layer() {
    if (!patch.layers) patch.layers = [defaultLayer()];
    if (layerIdx >= patch.layers.length) layerIdx = patch.layers.length - 1;
    return patch.layers[layerIdx];
  }
  function env(kindIdx = envKind) {
    const key = ENV_KINDS[kindIdx].key;
    const l = layer();
    if (!l[key]) l[key] = defaultEnv(key);
    return l[key];
  }

  function refreshTargets(keep = null) {
    const sel = $('edit-target');
    sel.innerHTML = '';
    if (!isKit()) {
      const fb = document.createElement('option');
      fb.value = 'fallback';
      fb.textContent = '— fallback patch —';
      sel.appendChild(fb);
      for (let p = 0; p < 128; p++) {
        const o = document.createElement('option');
        o.value = p;
        o.textContent = `${p} ${GM_NAMES[p]}${store.bankDoc?.programs?.[String(p)] ? '' : '  ·fallback'}`;
        sel.appendChild(o);
      }
      sel.value = keep ?? '48';
    } else {
      for (let n = 27; n < 88; n++) {
        const o = document.createElement('option');
        o.value = n;
        o.textContent = `${n} ${DRUM_NAMES[n] || '(unnamed)'}${store.kitDoc?.notes?.[String(n)] ? '' : '  ·silent'}`;
        sel.appendChild(o);
      }
      sel.value = keep ?? '36';
    }
    selectTarget();
  }
  function selectTarget() {
    const key = targetKey();
    let src;
    if (!isKit()) {
      src = key === 'fallback' ? store.bankDoc.fallback : store.bankDoc.programs[key];
      if (key !== 'fallback') client.program(getAuditionChannel(), +key);
    } else {
      src = store.kitDoc.notes[key];
      if (!src) src = { name: (DRUM_NAMES[key] || 'drum').toLowerCase().replace(/\s+/g, '_'),
                        oneShot: true, layers: [defaultLayer()] };
    }
    patch = src ? structuredClone(src) : defaultPatch();
    layerIdx = 0;
    envKind = 0;
    setApplyState('');
    syncUI();
    buildKeyboard();
    if (isKit()) setTimeout(() => hitPad(+key), 60);  // hear what you selected
    refreshHistoryButtons();   // each target carries its own history
    opts.onSelect?.({ mode: $('edit-mode').value, key, patch });
  }
  $('edit-mode').onchange = () => { refreshTargets(); updateModeUI(); buildKeyboard(); };
  $('edit-target').onchange = selectTarget;
  function updateModeUI() {
    $('drum-flags').style.display = isKit() ? '' : 'none';
    $('btn-hit').style.display = isKit() ? '' : 'none';
    $('btn-export-doc').textContent = isKit() ? 'Export kit' : 'Export bank';
    $('pitchnote-wrap').style.display = isKit() ? '' : 'none';
  }

  // ---------------- apply pipeline ----------------
  function scheduleApply(delay = 300) {
    setApplyState('…editing');
    clearTimeout(applyTimer);
    applyTimer = setTimeout(applyNow, delay);
  }
  const histKey = () => $('edit-mode').value + '/' + targetKey();
  const hist = (map) => {
    const k = histKey();
    if (!map.has(k)) map.set(k, []);
    return map.get(k);
  };
  function refreshHistoryButtons() {
    // The editor is embedded in two pages that each own their own markup, so
    // a button can legitimately be absent -- it must not take the page down.
    const u = $('btn-undo'), r = $('btn-redo');
    if (u) u.disabled = hist(undoStacks).length === 0;
    if (r) r.disabled = hist(redoStacks).length === 0;
  }
  function pushUndo() {
    const key = targetKey();
    const prev = isKit() ? store.kitDoc.notes[key]
        : key === 'fallback' ? store.bankDoc.fallback : store.bankDoc.programs[key];
    const u = hist(undoStacks);
    u.push(JSON.stringify(prev ?? null));
    if (u.length > 50) u.shift();
    hist(redoStacks).length = 0;      // a fresh edit invalidates the redo branch
    refreshHistoryButtons();
  }
  function onPatchReport(r) {
    setApplyState(r.ok ? (r.warnings.length ? '✓ applied (warnings)' : '✓ applied') : '✗ rejected',
                  r.ok ? (r.warnings.length ? 'warn' : 'ok') : 'err');
    for (const w of r.warnings || []) log(`[patch] ${w}`, 'warn');
    for (const e of r.errors || []) log(`[patch] ${e}`, 'err');
  }
  function pushToEngine(key, clean) {
    let p;
    if (!isKit()) {
      if (key === 'fallback') {
        store.bankDoc.fallback = clean;
        p = client.loadBank(JSON.stringify(store.bankDoc));
      } else {
        store.bankDoc.programs[key] = clean;
        p = client.loadProgram(+key, JSON.stringify(clean));
      }
    } else {
      store.kitDoc.notes[key] = clean;
      p = client.loadDrumNote(+key, JSON.stringify(clean));
    }
    p.then(onPatchReport, (err) => onPatchReport(err.report || { ok: false, warnings: [], errors: [err.message] }));
    // The target list marks unauthored programs/notes: this one is authored now.
    const opt = $('edit-target').querySelector(`option[value="${key}"]`);
    if (opt) opt.textContent = opt.textContent.replace(/\s+·(fallback|silent)$/, '');
    opts.onChange?.({ mode: $('edit-mode').value, key, patch: clean });
  }
  function applyNow() {
    clearTimeout(applyTimer);
    if (!patch) return;
    pushUndo();
    const key = targetKey();
    pushToEngine(key, structuredClone(patch));
    if ($('json-drawer').classList.contains('open'))
      $('editor').value = JSON.stringify(patch, null, 2);
    // Retrigger held audition notes so a tweak is heard without replaying;
    // drums are one-shots, so auto-hit the edited drum instead.
    if (opts.autoHitOnApply !== false) {
      setTimeout(() => {
        if (isKit()) hitPad(+key);
        else for (const n of heldNotes) sendNoteOn(n);
      }, 60);
    }
  }

  // ---------------- generic control binding ----------------
  const bound = [];
  function bind(id, get, set, { live = true, redraw = null } = {}) {
    const el = $(id);
    const write = (e) => {
      set(el, e);
      if (redraw) redraw();
      scheduleApply(el.type === 'number' || el.tagName === 'SELECT' ? 150 : 350);
    };
    if (live && (el.type === 'range')) el.oninput = write;
    else el.onchange = write;
    el._sync = () => get(el);
    return el;
  }
  function B(...args) { bound.push(bind(...args)); }

  B('p-name', (el) => { el.value = patch.name || ''; }, (el) => { patch.name = el.value; });
  B('p-oneshot', (el) => { el.checked = !!patch.oneShot; }, (el) => { patch.oneShot = el.checked; });
  B('p-choke', (el) => { el.value = patch.exclusiveGroup ?? -1; },
    (el) => { patch.exclusiveGroup = +el.value; });

  function wtParams() {
    const l = layer();
    if (!l.wavetable) l.wavetable = defaultLayer().wavetable;
    if (!l.wavetable.params) l.wavetable.params = { startHarmonics: 16, endHarmonics: 16, falloff: 'saw' };
    return l.wavetable;
  }
  B('l-osc', (el) => { el.value = ['noise', 'metal'].includes(layer().osc) ? layer().osc : 'wavetable'; },
    (el) => { layer().osc = el.value; }, { redraw: () => syncOscVisibility() });
  B('wt-gen', (el) => { el.value = wtParams().generator || 'additiveHarmonicMorph'; },
    (el) => {
      const wt = wtParams();
      wt.generator = el.value;
      wt.params = defaultGenParams(el.value);
    }, { redraw: () => { renderGenParams(); drawWavetable(); drawLayers(); } });
  B('wt-frames', (el) => { el.value = wtParams().frameCount ?? 1; },
    (el) => { wtParams().frameCount = clamp(+el.value, 1, 32); }, { redraw: () => drawWavetable() });

  // ---- draggable histogram (harmonics 1..32) ----
  function buildHistogram(box, wt, f, changed) {
    const N = 32;
    const amps = (wt.params[f.key] || [...f.def]).slice(0, N);
    while (amps.length < N) amps.push(0);
    wt.params[f.key] = amps;
    const c = document.createElement('canvas');
    c.style.cssText = 'width:100%;height:96px;cursor:crosshair;touch-action:none';
    box.appendChild(c);
    const draw = () => {
      const { g, w, h, dpr } = canvas2d(c);
      g.fillStyle = '#101318';
      g.fillRect(0, 0, w, h);
      const bw = w / N;
      for (let i = 0; i < N; i++) {
        const bh = amps[i] * (h - 14 * dpr);
        g.fillStyle = i % 4 === 0 ? '#6fd66f' : 'rgba(111,214,111,0.75)';
        g.fillRect(i * bw + 1 * dpr, h - 12 * dpr - bh, bw - 2 * dpr, bh);
        if (i === 0 || (i + 1) % 8 === 0) {
          g.fillStyle = 'rgba(255,255,255,0.35)';
          g.font = `${8 * dpr}px ui-monospace`;
          g.fillText(String(i + 1), i * bw + 2 * dpr, h - 2 * dpr);
        }
      }
    };
    let last = null;  // interpolate between move events so fast drags fill every bar
    const paint = (e) => {
      const r = c.getBoundingClientRect();
      const i = clamp(Math.floor(((e.clientX - r.left) / r.width) * N), 0, N - 1);
      const v = clamp(1 - (e.clientY - r.top - 2) / (r.height - 14), 0, 1);
      if (last && Math.abs(i - last.i) > 1) {
        const dir = i > last.i ? 1 : -1;
        for (let k = last.i + dir; k !== i; k += dir) {
          const f = (k - last.i) / (i - last.i);
          amps[k] = Math.round((last.v + (v - last.v) * f) * 100) / 100;
        }
      }
      amps[i] = Math.round(v * 100) / 100;
      last = { i, v };
      draw();
      drawWavetable();
      drawLayers();
    };
    let down = false;
    c.onpointerdown = (e) => { down = true; last = null; c.setPointerCapture(e.pointerId); paint(e); };
    c.onpointermove = (e) => { if (down) paint(e); };
    c.onpointerup = () => { if (down) { down = false; changed(); } };
    draw();
  }
  // ---- drawn single-cycle editor (custom generator) ----
  function buildDrawWave(box, wt, f, changed) {
    const N = 128;
    let s = wt.params[f.key];
    if (!Array.isArray(s) || s.length < 2) {
      s = [...Array(N).keys()].map((i) => Math.sin((2 * Math.PI * i) / N));
      wt.params[f.key] = s.map((v) => Math.round(v * 100) / 100);
      s = wt.params[f.key];
    }
    const c = document.createElement('canvas');
    c.style.cssText = 'width:100%;height:110px;cursor:crosshair;touch-action:none';
    box.appendChild(c);
    const draw = () => {
      const { g, w, h, dpr } = canvas2d(c);
      g.fillStyle = '#101318';
      g.fillRect(0, 0, w, h);
      g.strokeStyle = 'rgba(255,255,255,0.12)';
      g.beginPath(); g.moveTo(0, h / 2); g.lineTo(w, h / 2); g.stroke();
      g.strokeStyle = '#6fd66f';
      g.lineWidth = 1.6 * dpr;
      g.beginPath();
      s.forEach((v, i) => {
        const x = (i / (s.length - 1)) * w;
        const y = h / 2 - v * (h / 2 - 4 * dpr);
        if (i === 0) g.moveTo(x, y); else g.lineTo(x, y);
      });
      g.stroke();
    };
    let last = null, down = false;
    const paint = (e) => {
      const r = c.getBoundingClientRect();
      const i = clamp(Math.round(((e.clientX - r.left) / r.width) * (s.length - 1)), 0, s.length - 1);
      const v = clamp(1 - (2 * (e.clientY - r.top)) / r.height, -1, 1);
      if (last && Math.abs(i - last.i) > 1) {
        const dir = i > last.i ? 1 : -1;
        for (let k = last.i + dir; k !== i; k += dir)
          s[k] = Math.round((last.v + ((v - last.v) * (k - last.i)) / (i - last.i)) * 100) / 100;
      }
      s[i] = Math.round(v * 100) / 100;
      last = { i, v };
      draw();
      drawWavetable();
      drawLayers();
    };
    c.onpointerdown = (e) => { down = true; last = null; c.setPointerCapture(e.pointerId); paint(e); };
    c.onpointermove = (e) => { if (down) paint(e); };
    c.onpointerup = () => { if (down) { down = false; changed(); } };
    draw();
  }
  // ---- Chebyshev transfer-curve editor: drag points at the Chebyshev nodes;
  // weights come back exactly via the discrete Chebyshev transform. ----
  const CHEB_N = 9;  // nodes -> T0..T8; T0 (DC) is discarded by the engine
  const chebNodes = [...Array(CHEB_N).keys()].map((k) => Math.cos((Math.PI * (k + 0.5)) / CHEB_N));
  const chebEval = (weights, x) => {
    let tkm1 = 1, tk = x, acc = 0;
    for (let n = 0; n < 8; n++) {
      acc += (weights[n] ?? 0) * tk;
      const tn = 2 * x * tk - tkm1;
      tkm1 = tk; tk = tn;
    }
    return acc;
  };
  function buildChebEditor(box, wt, f, changed) {
    const weights = (wt.params[f.key] || [...f.def]).slice(0, 8);
    while (weights.length < 8) weights.push(0);
    wt.params[f.key] = weights;
    const ys = chebNodes.map((x) => chebEval(weights, x));  // current curve at the nodes
    const c = document.createElement('canvas');
    c.style.cssText = 'width:100%;height:120px;cursor:crosshair;touch-action:none';
    box.appendChild(c);
    const readout = document.createElement('span');
    readout.className = 'hint';
    box.appendChild(readout);
    const YMAX = 2;
    const px = (x, w) => ((x + 1) / 2) * (w - 16) + 8;
    const py = (y, h) => h / 2 - (y / YMAX) * (h / 2 - 8);
    const solve = () => {
      // w_n = (2/N) * sum_k y_k * T_n(x_k), n = 1..8 (n = 0 dropped = DC).
      for (let n = 1; n <= 8; n++) {
        let s = 0;
        for (let k = 0; k < CHEB_N; k++) s += ys[k] * Math.cos((n * Math.PI * (k + 0.5)) / CHEB_N);
        weights[n - 1] = Math.round(((2 / CHEB_N) * s) * 1000) / 1000;
      }
      readout.textContent = 'T1..T8: ' + weights.map((w) => w.toFixed(2)).join(' ');
    };
    const draw = () => {
      const { g, w, h, dpr } = canvas2d(c);
      g.fillStyle = '#101318';
      g.fillRect(0, 0, w, h);
      g.strokeStyle = 'rgba(255,255,255,0.12)';
      g.beginPath(); g.moveTo(0, py(0, h)); g.lineTo(w, py(0, h)); g.stroke();
      g.beginPath(); g.moveTo(px(0, w), 0); g.lineTo(px(0, w), h); g.stroke();
      g.strokeStyle = '#e0b060';
      g.lineWidth = 1.6 * dpr;
      g.beginPath();
      for (let i = 0; i <= 96; i++) {
        const x = -1 + (2 * i) / 96;
        const y = clamp(chebEval(weights, x), -YMAX, YMAX);
        if (i === 0) g.moveTo(px(x, w), py(y, h)); else g.lineTo(px(x, w), py(y, h));
      }
      g.stroke();
      g.fillStyle = '#e8e6e1';
      chebNodes.forEach((x, k) => {
        g.beginPath();
        g.arc(px(x, w), py(clamp(ys[k], -YMAX, YMAX), h), 5 * dpr, 0, Math.PI * 2);
        g.fill();
      });
      g.fillStyle = 'rgba(255,255,255,0.35)';
      g.font = `${9 * dpr}px ui-monospace`;
      g.fillText('in → out (drag the points)', 8 * dpr, 12 * dpr);
    };
    let dragK = -1;
    c.onpointerdown = (e) => {
      const r = c.getBoundingClientRect();
      const dpr = window.devicePixelRatio || 1;
      const mx = (e.clientX - r.left) * (c.width / r.width), my = (e.clientY - r.top) * (c.height / r.height);
      let best = -1, bd = 18 * dpr;
      chebNodes.forEach((x, k) => {
        const d = Math.hypot(px(x, c.width) - mx, py(clamp(ys[k], -YMAX, YMAX), c.height) - my);
        if (d < bd) { bd = d; best = k; }
      });
      if (best >= 0) { dragK = best; c.setPointerCapture(e.pointerId); }
    };
    c.onpointermove = (e) => {
      if (dragK < 0) return;
      const r = c.getBoundingClientRect();
      const my = (e.clientY - r.top) * (c.height / r.height);
      ys[dragK] = clamp(((c.height / 2 - my) / (c.height / 2 - 8)) * YMAX, -YMAX, YMAX);
      solve();
      draw();
      drawWavetable();
      drawLayers();
    };
    c.onpointerup = () => { if (dragK >= 0) { dragK = -1; changed(); } };
    solve();
    draw();
  }
  function defaultGenParams(gen) {
    const p = {};
    for (const f of GEN_SPECS[gen] || []) p[f.key] = Array.isArray(f.def) ? [...f.def] : f.def;
    return p;
  }
  function classicOf(wt) {
    for (const [name, c] of Object.entries(CLASSIC)) {
      if ((wt.generator || 'additiveHarmonicMorph') !== c.generator || (wt.frameCount ?? 1) !== 1) continue;
      if (Object.entries(c.params).every(([k, v]) => (wt.params?.[k] ?? null) === v)) return name;
    }
    return '';
  }
  function renderGenParams() {
    const box = $('gen-params');
    box.innerHTML = '';
    if (['noise', 'metal'].includes(layer().osc)) return;
    const wt = wtParams();
    $('wt-classic').value = classicOf(wt);
    const gen = wt.generator || 'additiveHarmonicMorph';
    if (!wt.params) wt.params = defaultGenParams(gen);
    const changed = () => { drawWavetable(); drawLayers(); scheduleApply(150); };
    for (const f of GEN_SPECS[gen] || []) {
      if (f.onlyShape && (wt.params.shape || 'soft') !== f.onlyShape) continue;
      const lab = document.createElement('label');
      lab.textContent = f.label;
      box.appendChild(lab);
      if (f.type === 'drawwave') {
        buildDrawWave(box, wt, f, changed);
        continue;
      }
      if (f.type === 'hist') {
        buildHistogram(box, wt, f, changed);
        continue;
      }
      if (f.type === 'chebpts') {
        buildChebEditor(box, wt, f, changed);
        continue;
      }
      if (f.type === 'sel') {
        const sel = document.createElement('select');
        for (const o of f.options) {
          const opt = document.createElement('option');
          opt.textContent = o;
          sel.appendChild(opt);
        }
        sel.value = wt.params[f.key] ?? f.def;
        sel.onchange = () => { wt.params[f.key] = sel.value; if (f.key === 'shape') renderGenParams(); changed(); };
        box.appendChild(sel);
      } else if (f.type === 'weights') {
        const w = wt.params[f.key] || [...f.def];
        wt.params[f.key] = w;
        w.forEach((v, i) => {
          const inp = document.createElement('input');
          inp.type = 'number';
          inp.className = 'narrow';
          inp.style.width = '48px';
          inp.min = -1; inp.max = 1; inp.step = 0.05;
          inp.value = v;
          inp.title = 'T' + (i + 1) + ' = harmonic ' + (i + 1) + ' at full drive';
          inp.onchange = () => { w[i] = clamp(+inp.value, -1, 1); changed(); };
          box.appendChild(inp);
        });
      } else if (f.type === 'bars') {
        const bars = wt.params[f.key] || [...f.def];
        wt.params[f.key] = bars;
        bars.forEach((v, i) => {
          const inp = document.createElement('input');
          inp.type = 'number';
          inp.className = 'narrow';
          inp.style.width = '42px';
          inp.min = 0; inp.max = 8; inp.step = 1;
          inp.value = v;
          inp.onchange = () => { bars[i] = clamp(+inp.value, 0, 8); changed(); };
          box.appendChild(inp);
        });
      } else {
        const inp = document.createElement('input');
        inp.type = 'number';
        inp.className = 'narrow';
        inp.min = f.min; inp.max = f.max; inp.step = f.step;
        inp.value = wt.params[f.key] ?? f.def;
        inp.onchange = () => { wt.params[f.key] = clamp(+inp.value, f.min, f.max); changed(); };
        if (gen === 'additiveHarmonicMorph' && f.key === 'endHarmonics' && (wt.frameCount ?? 1) <= 1) {
          inp.disabled = true;
          inp.title = 'only used when frames > 1: the harmonic count sweeps start → end across frames';
        }
        box.appendChild(inp);
      }
    }
    // Falloff shapes the relative partial levels: with one harmonic there is
    // nothing to shape (a sine sounds the same whatever the falloff).
    if (gen === 'additiveHarmonicMorph') {
      const frames = wt.frameCount ?? 1;
      const start = wt.params.startHarmonics ?? 16, end = wt.params.endHarmonics ?? 16;
      const effective = frames > 1 ? Math.max(start, end) : start;
      if (effective <= 1) {
        const hint = document.createElement('span');
        hint.className = 'hint';
        hint.style.color = 'var(--warn)';
        hint.textContent = '1 harmonic = a sine: falloff has no effect — raise "harmonics" (8–32) to hear/see it';
        box.appendChild(hint);
      }
    }
  }

  B('l-gain', (el) => { el.value = Math.round((layer().mixGain ?? 1) * 1000); },
    (el) => { layer().mixGain = +el.value / 1000; $('l-gain-val').textContent = (+el.value / 1000).toFixed(2); });
  B('l-pan', (el) => { el.value = Math.round((layer().pan ?? 0) * 1000); },
    (el) => { layer().pan = +el.value / 1000; $('l-pan-val').textContent = panLabel(+el.value / 1000); });
  B('l-detune', (el) => { el.value = layer().detuneCents ?? 0; }, (el) => { layer().detuneCents = +el.value; });
  B('l-delay', (el) => { el.value = layer().startDelayMs ?? 0; },
    (el) => {
      const v = clamp(+el.value, 0, 2000);
      if (v <= 0) delete layer().startDelayMs; else layer().startDelayMs = v;
    }, { redraw: () => drawLayersEnv() });
  B('l-velcurve', (el) => { el.value = layer().velCurve || 'squared'; },
    (el) => { if (el.value === 'squared') delete layer().velCurve; else layer().velCurve = el.value; });
  B('l-vlow', (el) => { el.value = layer().velLow ?? 0; }, (el) => { layer().velLow = clamp(+el.value, 0, 127); });
  B('l-vhigh', (el) => { el.value = layer().velHigh ?? 127; }, (el) => { layer().velHigh = clamp(+el.value, 0, 127); });
  B('l-pitchnote', (el) => { el.value = layer().pitchNote ?? -1; },
    (el) => { const v = +el.value; if (v < 0) delete layer().pitchNote; else layer().pitchNote = v; });

  B('env-dl', (el) => { el.value = env().delay ?? 0; }, (el) => { const v = Math.max(0, +el.value); if (v) env().delay = v; else delete env().delay; }, { redraw: () => drawEnv() });
  B('env-h', (el) => { el.value = env().hold ?? 0; }, (el) => { const v = Math.max(0, +el.value); if (v) env().hold = v; else delete env().hold; }, { redraw: () => drawEnv() });
  B('env-loop', (el) => { el.checked = !!env().loop; }, (el) => { if (el.checked) env().loop = true; else delete env().loop; });
  B('env-a', (el) => { el.value = env().attack ?? 0; }, (el) => { env().attack = Math.max(0, +el.value); }, { redraw: () => drawEnv() });
  B('env-d', (el) => { el.value = env().decay ?? 0; }, (el) => { env().decay = Math.max(0, +el.value); }, { redraw: () => drawEnv() });
  B('env-s', (el) => { el.value = env().sustain ?? 1; }, (el) => { env().sustain = clamp(+el.value, 0, 1); }, { redraw: () => drawEnv() });
  B('env-r', (el) => { el.value = env().release ?? 0; }, (el) => { env().release = Math.max(0, +el.value); }, { redraw: () => drawEnv() });
  B('env-ca', (el) => { el.value = env().attackCurve || 'linear'; }, (el) => { env().attackCurve = el.value; }, { redraw: () => drawEnv() });
  B('env-cd', (el) => { el.value = env().decayCurve || 'exp'; }, (el) => { env().decayCurve = el.value; }, { redraw: () => drawEnv() });
  B('env-cr', (el) => { el.value = env().releaseCurve || 'exp'; }, (el) => { env().releaseCurve = el.value; }, { redraw: () => drawEnv() });
  B('env-depth', (el) => {
      const k = ENV_KINDS[envKind];
      el.value = k.depth ? (env()[k.depth] ?? (k.depth === 'depth' && k.key === 'filterEnv' ? 0.5 : k.key === 'wtPosEnv' ? 1 : 0)) : 0;
    },
    (el) => { const k = ENV_KINDS[envKind]; if (k.depth) env()[k.depth] = +el.value; });

  const hzFromSlider = (v) => Math.round(20 * Math.pow(600, v / 1000));
  const sliderFromHz = (hz) => Math.round(1000 * Math.log(clamp(hz, 20, 12000) / 20) / Math.log(600));
  function filt() {
    const l = layer();
    if (!l.filter) l.filter = { mode: 'bypass', cutoffHz: 1200, resonance: 0 };
    return l.filter;
  }
  B('f-mode', (el) => { el.value = layer().filter?.mode || 'bypass'; },
    (el) => { filt().mode = el.value; }, { redraw: () => { syncFilterModeUI(); drawFilterResp(); } });
  function syncFilterModeUI() {
    const mode = layer().filter?.mode || 'bypass';
    $('vowel-wrap').style.display = mode === 'formant' ? '' : 'none';
    $('f-cut-val').title = mode === 'comb' ? 'comb resonator frequency'
        : mode === 'formant' ? 'formant scale (1000 Hz = neutral)' : '';
  }
  B('f-vowel', (el) => { el.value = layer().filter?.vowel || 'a'; }, (el) => { filt().vowel = el.value; });
  B('f-vowel2', (el) => { el.value = layer().filter?.endVowel || ''; },
    (el) => { if (el.value) filt().endVowel = el.value; else delete filt().endVowel; });
  B('f-cut', (el) => { el.value = sliderFromHz(layer().filter?.cutoffHz ?? 1200); },
    (el) => { filt().cutoffHz = hzFromSlider(+el.value); $('f-cut-val').textContent = filt().cutoffHz + ' Hz'; },
    { redraw: () => drawFilterResp() });
  B('f-res', (el) => { el.value = Math.round((layer().filter?.resonance ?? 0) * 1000); },
    (el) => { filt().resonance = +el.value / 1000; $('f-res-val').textContent = (+el.value / 1000).toFixed(2); },
    { redraw: () => drawFilterResp() });
  B('f-key', (el) => { el.value = Math.round((layer().keyTrackFilter ?? 0) * 1000); },
    (el) => { layer().keyTrackFilter = +el.value / 1000; $('f-key-val').textContent = (+el.value / 1000).toFixed(2); });
  B('f-slope', (el) => { el.value = String(layer().filter?.slope === 24 ? 24 : 12); },
    (el) => { if (el.value === '24') filt().slope = 24; else delete filt().slope; }, { redraw: () => drawFilterResp() });
  const driveLabel = (d) => (d <= 1 ? 'off' : d.toFixed(2) + '×');
  B('f-post', (el) => { el.value = Math.round((layer().postDrive ?? 1) * 1000); },
    (el) => { const d = +el.value / 1000; if (d <= 1) delete layer().postDrive; else layer().postDrive = d; $('f-post-val').textContent = driveLabel(d); });
  function filt2() {
    const l = layer();
    if (!l.filter2) l.filter2 = { mode: 'bypass', cutoffHz: 200, resonance: 0 };
    return l.filter2;
  }
  B('f2-mode', (el) => { el.value = layer().filter2?.mode || 'bypass'; },
    (el) => { if (el.value === 'bypass') delete layer().filter2; else filt2().mode = el.value; });
  B('f2-cut', (el) => { el.value = layer().filter2?.cutoffHz ?? 200; }, (el) => { filt2().cutoffHz = clamp(+el.value, 20, 12000); });
  B('f2-res', (el) => { el.value = layer().filter2?.resonance ?? 0; }, (el) => { filt2().resonance = clamp(+el.value, 0, 0.95); });
  B('f2-static', (el) => { el.checked = !!layer().filter2?.static; },
    (el) => { if (el.checked) filt2().static = true; else if (layer().filter2) delete layer().filter2.static; });
  B('f-routing', (el) => { el.value = layer().filterRouting || 'serial'; },
    (el) => { if (el.value === 'serial') delete layer().filterRouting; else layer().filterRouting = el.value; });
  function uni() {
    const l = layer();
    if (!l.unison) l.unison = { voices: 1, detuneCents: 7, spread: 0.5 };
    return l.unison;
  }
  B('l-unison', (el) => { el.value = layer().unison?.voices ?? 1; },
    (el) => { const v = clamp(Math.round(+el.value), 1, 7); if (v <= 1) delete layer().unison; else uni().voices = v; });
  B('l-unidet', (el) => { el.value = layer().unison?.detuneCents ?? 7; }, (el) => { if (layer().unison) uni().detuneCents = clamp(+el.value, 0, 100); });
  B('l-unispread', (el) => { el.value = layer().unison?.spread ?? 0.5; }, (el) => { if (layer().unison) uni().spread = clamp(+el.value, 0, 1); });
  B('l-ringratio', (el) => { el.value = layer().ring?.ratio ?? 0; },
    (el) => {
      const v = clamp(+el.value, 0, 16);
      if (v <= 0) delete layer().ring;
      else { if (!layer().ring) layer().ring = { amount: 1 }; layer().ring.ratio = v; }
    });
  B('l-ringamt', (el) => { el.value = layer().ring?.amount ?? 1; }, (el) => { if (layer().ring) layer().ring.amount = clamp(+el.value, 0, 1); });
  B('l-relvel', (el) => { el.value = layer().releaseVel ?? 0; },
    (el) => { const v = clamp(+el.value, 0, 1); if (v <= 0) delete layer().releaseVel; else layer().releaseVel = v; });
  const bindSend = (id, key) => B(id,
    (el) => { el.value = layer()[key] ?? ''; },
    (el) => { const v = el.value === '' ? null : clamp(+el.value, 0, 1); if (v === null) delete layer()[key]; else layer()[key] = v; });
  bindSend('l-sendrev', 'sendReverb');
  bindSend('l-sendcho', 'sendChorus');
  bindSend('l-senddel', 'sendDelay');
  B('l-rr', (el) => { el.value = layer().rrGroup ?? 0; },
    (el) => { const v = clamp(Math.round(+el.value), 0, 15); if (v <= 0) delete layer().rrGroup; else layer().rrGroup = v; });
  B('f-drive', (el) => { el.value = Math.round((layer().filter?.drive ?? 1) * 1000); },
    (el) => { const d = +el.value / 1000; if (d <= 1) delete filt().drive; else filt().drive = d; $('f-drive-val').textContent = driveLabel(d); });

  // Classic subtractive starting points: one pick sets generator + params.
  const CLASSIC = {
    sine: { generator: 'additiveHarmonicMorph', params: { startHarmonics: 1, endHarmonics: 1, falloff: 'flat' } },
    triangle: { generator: 'additiveHarmonicMorph', params: { startHarmonics: 32, endHarmonics: 32, falloff: 'triangle' } },
    saw: { generator: 'additiveHarmonicMorph', params: { startHarmonics: 64, endHarmonics: 64, falloff: 'saw' } },
    square: { generator: 'additiveHarmonicMorph', params: { startHarmonics: 48, endHarmonics: 48, falloff: 'square' } },
    pulse25: { generator: 'pulse', params: { startDuty: 0.25, endDuty: 0.25 } },
    pulse12: { generator: 'pulse', params: { startDuty: 0.125, endDuty: 0.125 } },
  };
  $('wt-classic').onchange = (e) => {
    const c = CLASSIC[e.target.value];
    if (!c) return;
    const wt = wtParams();
    wt.generator = c.generator;
    wt.params = structuredClone(c.params);
    wt.frameCount = 1;
    $('wt-gen').value = c.generator;
    $('wt-frames').value = 1;
    renderGenParams();
    drawWavetable();
    drawLayers();
    scheduleApply(150);
  };

  // ---------------- layers / env tabs ----------------
  function renderLayerTabs() {
    const tabs = $('layer-tabs');
    tabs.innerHTML = '';
    (patch.layers || []).forEach((_, i) => {
      const b = document.createElement('button');
      b.textContent = `Layer ${i + 1}`;
      b.className = i === layerIdx ? 'active' : '';
      b.onclick = () => { layerIdx = i; syncUI(); };
      tabs.appendChild(b);
    });
    if ((patch.layers || []).length > 1) {
      const del = document.createElement('button');
      del.textContent = '×';
      del.title = 'remove current layer';
      del.onclick = () => { patch.layers.splice(layerIdx, 1); layerIdx = Math.max(0, layerIdx - 1); syncUI(); scheduleApply(); };
      tabs.appendChild(del);
    }
    if ((patch.layers || []).length < 4) {
      const add = document.createElement('button');
      add.textContent = '＋';
      add.title = 'add layer';
      add.onclick = () => { patch.layers.push(defaultLayer()); layerIdx = patch.layers.length - 1; syncUI(); scheduleApply(); };
      tabs.appendChild(add);
    }
  }
  function renderEnvTabs() {
    const tabs = $('env-tabs');
    tabs.innerHTML = '';
    ENV_KINDS.forEach((k, i) => {
      const b = document.createElement('button');
      const present = !!layer()[k.key];
      b.textContent = k.label + (present || i === 0 ? '' : ' ·');
      b.className = i === envKind ? 'active' : '';
      b.onclick = () => { envKind = i; syncEnvControls(); };
      tabs.appendChild(b);
    });
  }
  function syncEnvControls() {
    renderEnvTabs();
    for (const id of ['env-dl', 'env-a', 'env-h', 'env-d', 'env-s', 'env-r', 'env-ca', 'env-cd', 'env-cr', 'env-depth', 'env-loop']) $(id)._sync();
    const k = ENV_KINDS[envKind];
    $('env-depth-wrap').style.display = k.depth ? '' : 'none';
    $('env-depth-label').textContent = k.dlabel || 'depth';
    drawEnv();
  }

  // ---------------- LFOs / mod matrix ----------------
  function renderLfos() {
    const box = $('lfo-rows');
    box.innerHTML = '';
    const l = layer();
    for (let i = 0; i < 4; i++) {
      const row = document.createElement('div');
      row.className = 'row';
      const has = l.lfos && l.lfos[i];
      const SYNCS = ['', '1/1', '1/2', '1/4', '1/4T', '1/8', '1/8T', '1/16'];
      row.innerHTML = `
        <label><input type="checkbox" ${has ? 'checked' : ''} data-k="on"> LFO${i + 1}</label>
        <select data-k="shape" ${has ? '' : 'disabled'}>
          ${['sine', 'triangle', 'saw', 'square', 'sampleHold'].map((s) =>
            `<option ${has && l.lfos[i].shape === s ? 'selected' : ''}>${s}</option>`).join('')}
        </select>
        <label>rate Hz</label>
        <input type="number" data-k="rate" class="narrow" step="0.1" min="0" max="40"
               value="${has ? l.lfos[i].rateHz ?? 5 : 5}" ${has ? '' : 'disabled'}>
        <label>sync</label>
        <select data-k="sync" ${has ? '' : 'disabled'} title="tempo-synced cycle (overrides Hz)">
          ${SYNCS.map((s) => `<option value="${s}" ${has && (l.lfos[i].sync || '') === s ? 'selected' : ''}>${s || 'free'}</option>`).join('')}
        </select>
        <label>delay ms</label>
        <input type="number" data-k="delay" min="0" max="10000"
               value="${has ? l.lfos[i].delayMs ?? 0 : 0}" ${has ? '' : 'disabled'}>`;
      row.querySelector('[data-k=on]').onchange = (e) => {
        if (!l.lfos) l.lfos = [];
        if (e.target.checked) { while (l.lfos.length <= i) l.lfos.push({ shape: 'sine', rateHz: 5, delayMs: 0 }); }
        else { l.lfos.splice(i, 1); if (l.lfos.length === 0) delete l.lfos; }
        renderLfos();
        scheduleApply();
      };
      if (has) {
        row.querySelector('[data-k=shape]').onchange = (e) => { l.lfos[i].shape = e.target.value; scheduleApply(); };
        row.querySelector('[data-k=rate]').onchange = (e) => { l.lfos[i].rateHz = +e.target.value; scheduleApply(); };
        row.querySelector('[data-k=sync]').onchange = (e) => {
          if (e.target.value) l.lfos[i].sync = e.target.value; else delete l.lfos[i].sync;
          scheduleApply();
        };
        row.querySelector('[data-k=delay]').onchange = (e) => { l.lfos[i].delayMs = +e.target.value; scheduleApply(); };
      }
      box.appendChild(row);
    }
  }
  function renderMods() {
    const box = $('mod-rows');
    box.innerHTML = '';
    const l = layer();
    (l.modMatrix || []).forEach((r, i) => {
      const row = document.createElement('div');
      row.className = 'modrow';
      row.innerHTML = `
        <select>${MOD_SOURCES.map((s) => `<option ${r.src === s ? 'selected' : ''}>${s}</option>`).join('')}</select>
        <select>${MOD_DESTS.map((s) => `<option ${r.dst === s ? 'selected' : ''}>${s}</option>`).join('')}</select>
        <input type="number" step="0.005" value="${r.amount ?? 0}">
        <button class="small">×</button>`;
      const [srcSel, dstSel] = row.querySelectorAll('select');
      srcSel.onchange = () => { r.src = srcSel.value; scheduleApply(); };
      dstSel.onchange = () => { r.dst = dstSel.value; scheduleApply(); };
      row.querySelector('input').onchange = (e) => { r.amount = +e.target.value; scheduleApply(); };
      row.querySelector('button').onclick = () => {
        l.modMatrix.splice(i, 1);
        if (!l.modMatrix.length) delete l.modMatrix;
        renderMods();
        scheduleApply();
      };
      box.appendChild(row);
    });
  }
  $('mod-add').onclick = () => {
    const l = layer();
    if (!l.modMatrix) l.modMatrix = [];
    l.modMatrix.push({ src: 'LFO1', dst: 'Pitch', amount: 0.005 });
    renderMods();
    scheduleApply();
  };

  // ---------------- envelope canvas ----------------
  function envGeometry() {
    const e = env();
    const A = e.attack ?? 0, D = e.decay ?? 0, S = clamp(e.sustain ?? 1, 0, 1), R = e.release ?? 0;
    const oneShot = isKit() && patch.oneShot;
    const base = A + D + R;
    const hold = oneShot ? 0 : Math.max(90, base * 0.18);
    const T = Math.max(400, (A + D + hold + R) * 1.08);
    return { A, D, S: oneShot ? 0 : S, R, hold, T, oneShot };
  }
  function drawEnv() {
    const c = $('env-canvas');
    const { g, w, h, dpr } = canvas2d(c);
    g.fillStyle = '#101318';
    g.fillRect(0, 0, w, h);
    const { A, D, S, R, hold, T, oneShot } = envGeometry();
    const e = env();
    const pad = 8 * dpr;
    const X = (ms) => pad + (ms / T) * (w - 2 * pad);
    const Y = (lvl) => h - pad - lvl * (h - 2 * pad);
    const curve = (name) => CURVES[name] || CURVES.linear;
    g.strokeStyle = 'rgba(255,255,255,0.05)';
    g.fillStyle = 'rgba(255,255,255,0.25)';
    g.font = `${10 * dpr}px ui-monospace`;
    const tick = T > 3000 ? 1000 : T > 800 ? 250 : 100;
    for (let ms = 0; ms <= T; ms += tick) {
      g.beginPath(); g.moveTo(X(ms), 0); g.lineTo(X(ms), h); g.stroke();
      if (ms > 0) g.fillText(ms >= 1000 ? (ms / 1000) + 's' : ms + '', X(ms) + 2, h - 2 * dpr);
    }
    const seg = (x0, y0, x1, y1, cname) => {
      const f = curve(cname);
      for (let i = 0; i <= 48; i++) {
        const p = i / 48;
        const x = x0 + (x1 - x0) * p;
        const y = y0 + (y1 - y0) * f(p);
        if (i === 0 && x0 === X(0)) g.moveTo(x, y); else g.lineTo(x, y);
      }
    };
    g.strokeStyle = '#64b5f6';
    g.lineWidth = 2 * dpr;
    g.beginPath();
    g.moveTo(X(0), Y(0));
    seg(X(0), Y(0), X(A), Y(1), e.attackCurve || 'linear');
    seg(X(A), Y(1), X(A + D), Y(S), e.decayCurve || 'exp');
    if (!oneShot) {
      g.lineTo(X(A + D + hold), Y(S));
      seg(X(A + D + hold), Y(S), X(A + D + hold + R), Y(0), e.releaseCurve || 'exp');
    }
    g.stroke();
    if (!oneShot) {
      g.strokeStyle = 'rgba(100,181,246,0.35)';
      g.setLineDash([4 * dpr, 4 * dpr]);
      g.beginPath(); g.moveTo(X(A + D), Y(S)); g.lineTo(X(A + D + hold), Y(S)); g.stroke();
      g.setLineDash([]);
    }
    g.fillStyle = '#e8e6e1';
    for (const hd of envHandles()) {
      g.beginPath();
      g.arc(X(hd.ms), Y(hd.lvl), 5 * dpr, 0, Math.PI * 2);
      g.fill();
    }
    if (oneShot) {
      g.fillStyle = 'rgba(255,255,255,0.3)';
      g.fillText('one-shot: A→D→off (no sustain/release)', pad, 12 * dpr);
    }
  }
  function envHandles() {
    const { A, D, S, R, hold, oneShot } = envGeometry();
    const hs = [{ id: 'a', ms: A, lvl: 1 }, { id: 'd', ms: A + D, lvl: S }];
    if (!oneShot) hs.push({ id: 'r', ms: A + D + hold + R, lvl: 0 });
    return hs;
  }
  $('env-canvas').addEventListener('pointerdown', (ev) => {
    const c = $('env-canvas');
    const dpr = window.devicePixelRatio || 1;
    const rect = c.getBoundingClientRect();
    const px = (ev.clientX - rect.left) * dpr, py = (ev.clientY - rect.top) * dpr;
    const { T } = envGeometry();
    const pad = 8 * dpr, w = c.width, h = c.height;
    const X = (ms) => pad + (ms / T) * (w - 2 * pad);
    const Y = (lvl) => h - pad - lvl * (h - 2 * pad);
    let best = null, bestDist = 20 * dpr;
    for (const hd of envHandles()) {
      const d = Math.hypot(X(hd.ms) - px, Y(hd.lvl) - py);
      if (d < bestDist) { best = hd.id; bestDist = d; }
    }
    if (best) { envDrag = best; c.setPointerCapture(ev.pointerId); }
  });
  $('env-canvas').addEventListener('pointermove', (ev) => {
    if (!envDrag) return;
    const c = $('env-canvas');
    const dpr = window.devicePixelRatio || 1;
    const rect = c.getBoundingClientRect();
    const px = (ev.clientX - rect.left) * dpr, py = (ev.clientY - rect.top) * dpr;
    const { A, D, hold, T } = envGeometry();
    const pad = 8 * dpr, w = c.width, h = c.height;
    const msAt = (x) => clamp(((x - pad) / (w - 2 * pad)) * T, 0, 20000);
    const lvlAt = (y) => clamp((h - pad - y) / (h - 2 * pad), 0, 1);
    const e = env();
    if (envDrag === 'a') e.attack = Math.round(msAt(px));
    else if (envDrag === 'd') {
      e.decay = Math.round(Math.max(0, msAt(px) - A));
      e.sustain = Math.round(lvlAt(py) * 100) / 100;
    } else if (envDrag === 'r') {
      e.release = Math.round(Math.max(0, msAt(px) - A - D - hold));
    }
    for (const id of ['env-a', 'env-d', 'env-s', 'env-r']) $(id)._sync();
    drawEnv();
  });
  $('env-canvas').addEventListener('pointerup', () => {
    if (envDrag) { envDrag = null; scheduleApply(100); }
  });

  // ---------------- previews ----------------
  function layerWave(l, t) {
    if (l.osc === 'noise' || l.osc === 'metal') return null;
    const wt = l.wavetable || {};
    return genWave(wt.generator || 'additiveHarmonicMorph', wt.params, t);
  }
  function drawWavetable() {
    const c = $('wt-preview');
    const { g, w, h, dpr } = canvas2d(c);
    g.fillStyle = '#101318';
    g.fillRect(0, 0, w, h);
    if (['noise', 'metal'].includes(layer().osc)) {
      g.fillStyle = 'rgba(255,255,255,0.3)';
      g.font = `${11 * dpr}px ui-monospace`;
      g.fillText(layer().osc === 'metal' ? 'metal cluster (6 squares, 808 ratios)' : 'noise source', 8 * dpr, 16 * dpr);
      return;
    }
    const wt = wtParams();
    const frames = wt.frameCount ?? 1;
    const gen = wt.generator || 'additiveHarmonicMorph';
    const morph = +$('wt-morph').value / 1000;
    const N = 512;
    const draw = (buf, style, lw) => {
      g.strokeStyle = style;
      g.lineWidth = lw * dpr;
      g.beginPath();
      for (let i = 0; i < N; i++) {
        const x = (i / (N - 1)) * w;
        const y = h / 2 - buf[i] * (h / 2 - 4 * dpr);
        if (i === 0) g.moveTo(x, y); else g.lineTo(x, y);
      }
      g.stroke();
    };
    if (frames > 1) {
      draw(genWave(gen, wt.params, 0, N), 'rgba(255,255,255,0.10)', 1);
      draw(genWave(gen, wt.params, 1, N), 'rgba(255,255,255,0.10)', 1);
    }
    draw(genWave(gen, wt.params, frames > 1 ? morph : 0, N), '#6fd66f', 2);
    // Shaper family: inset the transfer curve (input -1..1 -> output).
    if (gen === 'waveshaper' || gen === 'chebyshev') {
      const t = frames > 1 ? morph : 0;
      const P = wt.params || {};
      const bx = w - w * 0.24 - 6 * dpr, by = 6 * dpr, bw = w * 0.24, bh = h - 12 * dpr;
      g.fillStyle = 'rgba(16,19,24,0.85)';
      g.fillRect(bx, by, bw, bh);
      g.strokeStyle = 'rgba(255,255,255,0.25)';
      g.strokeRect(bx, by, bw, bh);
      g.strokeStyle = 'rgba(255,255,255,0.12)';
      g.beginPath(); g.moveTo(bx + bw / 2, by); g.lineTo(bx + bw / 2, by + bh); g.stroke();
      g.beginPath(); g.moveTo(bx, by + bh / 2); g.lineTo(bx + bw, by + bh / 2); g.stroke();
      const transfer = (x) => {
        if (gen === 'chebyshev') {
          const wts = P.weights || [1, 0, 0, 0, 0, 0, 0, 0];
          const e = P.endWeights || wts;
          let tkm1 = 1, tk = x, acc = 0;
          for (let k = 0; k < 8; k++) {
            const wk = (wts[k] ?? 0) + (((e[k] ?? 0) - (wts[k] ?? 0)) * t);
            if (wk) acc += wk * tk;
            const tn = 2 * x * tk - tkm1;
            tkm1 = tk; tk = tn;
          }
          return clamp(acc / 2, -1, 1) * 2;  // fit +/-2 into the box
        }
        const drive = (P.startDrive ?? 1) + ((P.endDrive ?? 8) - (P.startDrive ?? 1)) * t;
        let y = drive * x + (P.bias ?? 0);
        const shape = P.shape || 'soft';
        if (shape === 'hard' || shape === 'stairs') y = clamp(y, -1, 1);
        else if (shape === 'fold') { for (let k = 0; k < 64; k++) { if (y > 1) y = 2 - y; else if (y < -1) y = -2 - y; else break; } }
        else if (shape === 'crush') { const q = 1 / (P.levels ?? 6); y = Math.round(clamp(y, -1, 1) / q) * q; }
        else y = Math.abs(y) > 1 ? Math.sign(y) : 1.5 * y - 0.5 * y ** 3;
        return y;
      };
      g.strokeStyle = '#e0b060';
      g.lineWidth = 1.5 * dpr;
      g.beginPath();
      for (let i = 0; i <= 48; i++) {
        const x = -1 + (2 * i) / 48;
        const y = clamp(transfer(x), -2, 2);
        const px = bx + ((x + 1) / 2) * bw;
        const py = by + bh / 2 - (y / 2) * (bh / 2 - 2 * dpr) * 2 / 2 - (y / 2) * 0;
        const pyy = by + bh / 2 - (y / 2) * (bh - 4 * dpr) / 2;
        if (i === 0) g.moveTo(px, pyy); else g.lineTo(px, pyy);
      }
      g.stroke();
      g.fillStyle = 'rgba(255,255,255,0.4)';
      g.font = `${8 * dpr}px ui-monospace`;
      g.fillText('in→out', bx + 3 * dpr, by + 9 * dpr);
    }
    const pos = morph * Math.max(0, frames - 1);
    $('wt-morph-val').textContent = frames > 1 ? `frame ${pos.toFixed(1)}` : 'frame 0';
    $('wt-morph-row').style.display = frames > 1 ? '' : 'none';
  }
  $('wt-morph').oninput = () => drawWavetable();
  function syncOscVisibility() {
    const nonWt = ['noise', 'metal'].includes(layer().osc);
    $('wt-controls').style.display = nonWt ? 'none' : '';
    $('noise-hint').style.display = nonWt ? '' : 'none';
    $('noise-hint').textContent = layer().osc === 'metal'
        ? 'six detuned squares (808 ratios) — bandpass 4–8 kHz for hats, wider for crashes; pitchNote sets the base'
        : 'white noise — shape it with the filter below';
    renderGenParams();
    drawWavetable();
  }
  function drawLayers() {
    const c = $('layers-canvas');
    const { g, w, h, dpr } = canvas2d(c);
    g.fillStyle = '#101318';
    g.fillRect(0, 0, w, h);
    const N = 512;
    const layers = patch.layers || [];
    const sum = new Float64Array(N);
    let sumPeak = 1e-9;
    const waves = layers.map((l) => {
      const wv = layerWave(l, 0);
      if (wv) { const gain = l.mixGain ?? 1; for (let i = 0; i < N; i++) sum[i] += wv[i] * gain; }
      return wv;
    });
    for (const v of sum) sumPeak = Math.max(sumPeak, Math.abs(v));
    const scale = Math.min(1, 1 / sumPeak);
    const trace = (buf, mul, style, lw, dash) => {
      g.strokeStyle = style;
      g.lineWidth = lw * dpr;
      g.setLineDash(dash ? [3 * dpr, 3 * dpr] : []);
      g.beginPath();
      for (let i = 0; i < N; i++) {
        const x = (i / (N - 1)) * w;
        const y = h / 2 - buf[i] * mul * (h / 2 - 4 * dpr);
        if (i === 0) g.moveTo(x, y); else g.lineTo(x, y);
      }
      g.stroke();
      g.setLineDash([]);
    };
    waves.forEach((wv, i) => {
      if (!wv) return;
      const gain = (layers[i].mixGain ?? 1) * scale;
      trace(wv, gain, LAYER_COLORS[i] + (i === layerIdx ? '' : '80'), i === layerIdx ? 1.6 : 1.1);
    });
    if (layers.length > 1) trace(sum, scale, 'rgba(255,255,255,0.9)', 2.2);
    const legend = $('layers-legend');
    legend.innerHTML = '';
    layers.forEach((l, i) => {
      const chip = document.createElement('span');
      chip.className = 'lchip' + (i === layerIdx ? ' active' : '');
      const kind = l.osc === 'noise' ? 'noise'
          : (l.wavetable?.generator || 'additive').replace('additiveHarmonicMorph', 'additive');
      chip.innerHTML = `<span class="sw" style="background:${LAYER_COLORS[i]}"></span>` +
          `L${i + 1} ${kind} · ${(l.mixGain ?? 1).toFixed(2)}` +
          `${l.pan ? ' · ' + panLabel(l.pan) : ''}` +
          `${l.detuneCents ? ' · ' + l.detuneCents + '¢' : ''}` +
        `${l.startDelayMs ? ' · +' + l.startDelayMs + 'ms' : ''}`;
      chip.onclick = () => { layerIdx = i; syncUI(); };
      legend.appendChild(chip);
    });
    if (layers.length > 1) {
      const note = document.createElement('span');
      note.className = 'hint';
      note.textContent = 'white = weighted sum (noise layers excluded)';
      legend.appendChild(note);
    }
    drawLayersEnv();
  }
  function drawLayersEnv() {
    const c = $('layers-env');
    const { g, w, h, dpr } = canvas2d(c);
    g.fillStyle = '#101318';
    g.fillRect(0, 0, w, h);
    const layers = patch.layers || [];
    let T = 400;
    const geo = layers.map((l) => {
      const e = l.ampEnv || { attack: 1, decay: 0, sustain: 1, release: 50 };
      const A = e.attack ?? 0, D = e.decay ?? 0, S = e.sustain ?? 1, R = e.release ?? 0;
      const hold = patch.oneShot ? 0 : 120;
      const off = l.startDelayMs ?? 0;
      T = Math.max(T, off + (A + D + hold + R) * 1.08);
      return { A, D, S: patch.oneShot ? 0 : S, R, hold, e, off };
    });
    const pad = 4 * dpr;
    const X = (ms) => pad + (ms / T) * (w - 2 * pad);
    const Y = (lvl) => h - pad - lvl * (h - 2 * pad);
    geo.forEach((q, i) => {
      const curve = (name) => CURVES[name] || CURVES.linear;
      g.strokeStyle = LAYER_COLORS[i] + (i === layerIdx ? '' : '80');
      g.lineWidth = (i === layerIdx ? 1.8 : 1.1) * dpr;
      g.beginPath();
      g.moveTo(X(0), Y(0));
      const seg = (x0, y0, x1, y1, cn) => {
        const f = curve(cn);
        for (let k = 1; k <= 24; k++) { const p = k / 24; g.lineTo(x0 + (x1 - x0) * p, y0 + (y1 - y0) * f(p)); }
      };
      const o = q.off || 0;  // startDelayMs shifts the whole envelope right
      if (o) g.lineTo(X(o), Y(0));
      seg(X(o), Y(0), X(o + q.A), Y(1), q.e.attackCurve || 'linear');
      seg(X(o + q.A), Y(1), X(o + q.A + q.D), Y(q.S), q.e.decayCurve || 'exp');
      if (!patch.oneShot) {
        g.lineTo(X(o + q.A + q.D + q.hold), Y(q.S));
        seg(X(o + q.A + q.D + q.hold), Y(q.S), X(o + q.A + q.D + q.hold + q.R), Y(0), q.e.releaseCurve || 'exp');
      }
      g.stroke();
    });
    g.fillStyle = 'rgba(255,255,255,0.25)';
    g.font = `${9 * dpr}px ui-monospace`;
    g.fillText('amp envelopes · ' + Math.round(T) + 'ms', pad + 2, 10 * dpr);
  }
  function drawFilterResp() {
    const c = $('filter-resp');
    const { g, w, h, dpr } = canvas2d(c);
    g.fillStyle = '#101318';
    g.fillRect(0, 0, w, h);
    const f = layer().filter;
    const mode = f?.mode || 'bypass';
    const fMin = 20, fMax = 24000;
    const xOf = (hz) => (Math.log(hz / fMin) / Math.log(fMax / fMin)) * w;
    g.fillStyle = 'rgba(224,112,112,0.08)';
    g.fillRect(xOf(12000), 0, w - xOf(12000), h);
    g.strokeStyle = 'rgba(255,255,255,0.07)';
    for (const hz of [100, 1000, 10000]) { g.beginPath(); g.moveTo(xOf(hz), 0); g.lineTo(xOf(hz), h); g.stroke(); }
    if (mode === 'bypass') {
      g.fillStyle = 'rgba(255,255,255,0.3)';
      g.font = `${10 * dpr}px ui-monospace`;
      g.fillText('filter bypassed', 8 * dpr, 14 * dpr);
      return;
    }
    if (mode === 'comb' || mode === 'formant') {
      // Comb: harmonic peaks at multiples of the tuned frequency; formant:
      // three vowel peaks. Sketch, not a transfer-exact plot.
      g.strokeStyle = '#64b5f6';
      g.lineWidth = 1.6 * dpr;
      const peaks = [];
      if (mode === 'comb') {
        const f0 = Math.min(f?.cutoffHz ?? 440, 12000);
        for (let k = 1; k * f0 < 12000 && k <= 24; k++) peaks.push([k * f0, 1]);
      } else {
        const FORMANTS = { a: [800, 1150, 2900], e: [400, 1600, 2700], i: [250, 1750, 3400], o: [400, 800, 2600], u: [350, 600, 2700] };
        const scale = (f?.cutoffHz ?? 1000) / 1000;
        (FORMANTS[f?.vowel || 'a'] || FORMANTS.a).forEach((hz, k) => peaks.push([hz * scale, [1, 0.6, 0.35][k]]));
      }
      const dbTop2 = 18, dbBot2 = -36;
      const yOf2 = (db) => ((dbTop2 - db) / (dbTop2 - dbBot2)) * h;
      g.beginPath();
      for (let px = 0; px <= w; px += 2) {
        const hz = fMin * Math.pow(fMax / fMin, px / w);
        let mag = 0.03;
        for (const [pf, pg] of peaks) {
          const q = 18;
          const wr = hz / pf;
          mag += pg / Math.sqrt(1 + q * q * (wr - 1 / wr) ** 2);
        }
        const y = yOf2(Math.max(dbBot2, Math.min(dbTop2, 20 * Math.log10(mag))));
        if (px === 0) g.moveTo(px, y); else g.lineTo(px, y);
      }
      g.stroke();
      g.fillStyle = 'rgba(255,255,255,0.35)';
      g.font = `${9 * dpr}px ui-monospace`;
      g.fillText(mode === 'comb' ? `comb ${f?.cutoffHz ?? 440}Hz fb=${(f?.resonance ?? 0).toFixed(2)}`
                                 : `formant '${f?.vowel || 'a'}'${f?.endVowel ? '→' + f.endVowel : ''}`,
                 8 * dpr, 11 * dpr);
      return;
    }
    const fc = Math.min(f?.cutoffHz ?? 1200, 12000);
    const res = Math.min(f?.resonance ?? 0, 0.95);
    const Q = 1 / (2 - 2 * res);
    const stages = f?.slope === 24 ? 2 : 1;
    const dbTop = 18, dbBot = -36;
    const yOf = (db) => ((dbTop - db) / (dbTop - dbBot)) * h;
    g.strokeStyle = '#64b5f6';
    g.lineWidth = 1.8 * dpr;
    g.beginPath();
    for (let px = 0; px <= w; px += 2) {
      const hz = fMin * Math.pow(fMax / fMin, px / w);
      const wr = hz / fc;
      const den = Math.sqrt((1 - wr * wr) ** 2 + (wr / Q) ** 2);
      let mag;
      if (mode === 'lowpass') mag = 1 / den;
      else if (mode === 'highpass') mag = (wr * wr) / den;
      else if (mode === 'notch') mag = Math.abs(1 - wr * wr) / den;
      else mag = (wr / Q) / den;
      const db = Math.max(dbBot, stages * 20 * Math.log10(mag + 1e-9));
      const y = yOf(Math.min(dbTop, db));
      if (px === 0) g.moveTo(px, y); else g.lineTo(px, y);
    }
    g.stroke();
    g.strokeStyle = 'rgba(255,255,255,0.2)';
    g.setLineDash([2 * dpr, 3 * dpr]);
    g.beginPath(); g.moveTo(0, yOf(0)); g.lineTo(w, yOf(0)); g.stroke();
    g.setLineDash([]);
    g.fillStyle = 'rgba(255,255,255,0.35)';
    g.font = `${9 * dpr}px ui-monospace`;
    g.fillText(`${mode} ${fc}Hz Q=${Q.toFixed(2)} · ${stages * 12} dB${f?.drive > 1 ? ` · drive ${f.drive.toFixed(1)}×` : ''}`, 8 * dpr, 11 * dpr);
  }

  // ---------------- full sync ----------------
  function syncUI() {
    if (!patch) return;
    renderLayerTabs();
    for (const el of bound) el._sync();
    $('l-gain-val').textContent = (layer().mixGain ?? 1).toFixed(2);
    $('l-pan-val').textContent = panLabel(layer().pan ?? 0);
    $('f-cut-val').textContent = (layer().filter?.cutoffHz ?? 1200) + ' Hz';
    $('f-res-val').textContent = (layer().filter?.resonance ?? 0).toFixed(2);
    $('f-key-val').textContent = (layer().keyTrackFilter ?? 0).toFixed(2);
    $('f-drive-val').textContent = driveLabel(layer().filter?.drive ?? 1);
    syncFilterModeUI();
    syncEnvControls();
    syncOscVisibility();
    renderLfos();
    renderMods();
    drawLayers();
    drawFilterResp();
    updateModeUI();
    if ($('json-drawer').classList.contains('open')) $('editor').value = JSON.stringify(patch, null, 2);
  }

  // ---------------- JSON drawer / export / import / undo / A-B ----------------
  $('btn-json').onclick = () => {
    const d = $('json-drawer');
    d.classList.toggle('open');
    $('btn-json').textContent = d.classList.contains('open') ? 'JSON ▴' : 'JSON ▾';
    if (d.classList.contains('open')) $('editor').value = JSON.stringify(patch, null, 2);
  };
  $('btn-json-apply').onclick = () => {
    try { patch = JSON.parse($('editor').value); }
    catch (err) { log('editor JSON invalid: ' + err.message, 'err'); return; }
    layerIdx = 0;
    syncUI();
    applyNow();
  };
  // Undo and redo are the same move in opposite directions: pop the state to
  // go to off one stack, put the state being left on the other.
  function step(from, to, note, empty) {
    const src = hist(from);
    while (src.length) {
      const next = JSON.parse(src.pop());
      if (JSON.stringify(next) === JSON.stringify(patch)) continue;   // no-op entry
      hist(to).push(JSON.stringify(patch));
      patch = next ?? defaultPatch();
      layerIdx = 0;
      syncUI();
      pushToEngine(targetKey(), structuredClone(patch));  // no new history entry
      setApplyState(note, 'ok');
      refreshHistoryButtons();
      return true;
    }
    setApplyState(empty, 'warn');
    refreshHistoryButtons();
    return false;
  }
  const doUndo = () => step(undoStacks, redoStacks, '↶ undone', 'nothing to undo');
  const doRedo = () => step(redoStacks, undoStacks, '↷ redone', 'nothing to redo');
  if ($('btn-undo')) $('btn-undo').onclick = doUndo;
  if ($('btn-redo')) $('btn-redo').onclick = doRedo;
  $('btn-ab-store').onclick = () => {
    abSnapshot = JSON.stringify(patch);
    $('ab-state').textContent = 'B stored: ' + (patch.name || targetKey());
  };
  $('btn-ab-swap').onclick = () => {
    if (!abSnapshot) { $('ab-state').textContent = 'store B first'; return; }
    const b = JSON.parse(abSnapshot);
    abSnapshot = JSON.stringify(patch);
    patch = b;
    layerIdx = 0;
    syncUI();
    applyNow();
    $('ab-state').textContent = 'swapped (B now holds the other take)';
  };
  $('btn-init').onclick = () => {
    patch = defaultPatch();
    patch.name = isKit() ? (DRUM_NAMES[targetKey()] || 'drum').toLowerCase().replace(/\s+/g, '_') : 'init';
    if (isKit()) patch.oneShot = true;
    layerIdx = 0;
    syncUI();
    applyNow();
  };
  $('btn-export-patch').onclick = () => {
    const key = targetKey();
    const base = (patch.name || (isKit() ? 'drum_' + key : 'program_' + key)).replace(/\W+/g, '_');
    download(base + '.json', JSON.stringify(patch, null, 2), 'application/json');
  };
  $('btn-export-doc').onclick = () => {
    applyNow();
    if (isKit()) download('gm_kit.json', JSON.stringify(store.kitDoc, null, 2), 'application/json');
    else download('gm_bank.json', JSON.stringify(store.bankDoc, null, 2), 'application/json');
  };
  $('import-file').onchange = async (e) => {
    const f = e.target.files[0];
    if (!f) return;
    let doc;
    try { doc = JSON.parse(await f.text()); }
    catch (err) { log('import failed: ' + err.message, 'err'); return; }
    if (!importDoc(doc, f.name)) log('import: not a bank or kit file', 'err');
  };
  // Adopt a bank or kit document (import menu, DAW project load).
  function importDoc(doc, name = 'document') {
    if (doc.programs || doc.fallback) {
      store.bankDoc = doc;
      client.loadBank(JSON.stringify(doc)).catch((err) => log('bank: ' + err.message, 'err'));
      $('edit-mode').value = 'bank';
      log('bank imported: ' + name, 'ok');
    } else if (doc.notes) {
      store.kitDoc = doc;
      client.loadDrums(JSON.stringify(doc)).catch((err) => log('kit: ' + err.message, 'err'));
      $('edit-mode').value = 'kit';
      log('kit imported: ' + name, 'ok');
    } else {
      return false;
    }
    refreshTargets();
    updateModeUI();
    opts.onChange?.({ mode: $('edit-mode').value, key: null, patch: null });
    return true;
  }

  // ---------------- audition ----------------
  function sendNoteOn(note, vel) { client.noteOn(getAuditionChannel(), note, vel ?? +$('aud-vel').value); }
  function sendNoteOff(note) { client.noteOff(getAuditionChannel(), note); }
  $('btn-hit').onclick = () => { const note = +targetKey(); sendNoteOn(note); setTimeout(() => sendNoteOff(note), 150); };
  $('aud-sus').onchange = (e) => client.cc(getAuditionChannel(), 64, e.target.checked ? 127 : 0);
  $('aud-mod').oninput = (e) => client.cc(getAuditionChannel(), 1, +e.target.value);
  $('aud-bend').oninput = (e) => client.bend(getAuditionChannel(), +e.target.value);
  $('aud-bend').onpointerup = (e) => { e.target.value = 0; e.target.oninput(e); };

  function buildKeyboard() {
    if (isKitSafe()) { buildPads(); return; }
    const kbd = $('kbd');
    kbd.classList.remove('pads');
    $('aud-oct').style.display = '';
    const hintEl = $('aud-hint');
    if (hintEl) hintEl.textContent = 'A–K white · W/E/T/Y/U black · Z/X octave · keys audition the edited patch';
    kbd.innerHTML = '';
    const baseNote = () => (+$('aud-oct').value + 1) * 12;
    const els = [];
    for (let i = 0; i < 14; i++) {
      const wk = document.createElement('div');
      wk.className = 'wk';
      wk.dataset.semi = [0, 2, 4, 5, 7, 9, 11][i % 7] + 12 * Math.floor(i / 7);
      kbd.appendChild(wk);
      els.push(wk);
      if ([0, 1, 3, 4, 5].includes(i % 7)) {
        const bk = document.createElement('div');
        bk.className = 'bk';
        bk.dataset.semi = +wk.dataset.semi + 1;
        wk.appendChild(bk);
        els.push(bk);
      }
    }
    const press = (el) => {
      const note = baseNote() + +el.dataset.semi;
      if (heldNotes.has(note)) return;
      heldNotes.add(note);
      el.classList.add('on');
      sendNoteOn(note);
    };
    const release = (el) => {
      const note = baseNote() + +el.dataset.semi;
      if (!heldNotes.has(note)) return;
      heldNotes.delete(note);
      el.classList.remove('on');
      sendNoteOff(note);
    };
    for (const el of els) {
      el.onpointerdown = (e) => { e.stopPropagation(); press(el); el.setPointerCapture(e.pointerId); };
      el.onpointerup = (e) => { e.stopPropagation(); release(el); };
      el.onpointerleave = () => release(el);
    }
  }
  function buildPads() {
    const kbd = $('kbd');
    kbd.classList.add('pads');
    $('aud-oct').style.display = 'none';
    const hintEl = $('aud-hint');
    if (hintEl) hintEl.textContent = 'click a pad to hit AND select that drum · A–L keys hit the first pads';
    kbd.innerHTML = '';
    padNotes = Object.keys(store.kitDoc.notes).map(Number).sort((a, b) => a - b);
    const sel = +targetKey();
    if (!padNotes.includes(sel)) padNotes.push(sel);
    padNotes.sort((a, b) => a - b);
    for (const n of padNotes) {
      const pad = document.createElement('div');
      pad.className = 'pad' + (n === sel ? ' sel' : '');
      pad.dataset.note = n;
      const name = store.kitDoc.notes[String(n)]?.name || DRUM_NAMES[n] || 'drum';
      pad.innerHTML = `${n}<small>${name}</small>`;
      pad.onpointerdown = (e) => {
        e.preventDefault();
        hitPad(n);
        if (String(n) !== targetKey()) { $('edit-target').value = String(n); selectTarget(); }
      };
      kbd.appendChild(pad);
    }
  }
  function hitPad(note) {
    sendNoteOn(note);
    setTimeout(() => sendNoteOff(note), 120);
    const pad = root.querySelector(`.pad[data-note="${note}"]`);
    if (pad) { pad.classList.add('on'); setTimeout(() => pad.classList.remove('on'), 160); }
  }
  $('aud-oct').onchange = buildKeyboard;

  const onKeyDown = (e) => {
    if (['TEXTAREA', 'INPUT', 'SELECT'].includes(e.target.tagName) || e.repeat) return;
    if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === 'z') {
      e.preventDefault();
      if (e.shiftKey) doRedo(); else doUndo();
      return;
    }
    if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === 'y') { e.preventDefault(); doRedo(); return; }
    if (e.metaKey || e.ctrlKey) return;
    const k = e.key.toLowerCase();
    if (k === 'z') { $('aud-oct').value = Math.max(2, +$('aud-oct').value - 1); buildKeyboard(); return; }
    if (k === 'x') { $('aud-oct').value = Math.min(6, +$('aud-oct').value + 1); buildKeyboard(); return; }
    if (k in KEYMAP && !keysDown.has(k)) {
      keysDown.add(k);
      if (isKitSafe()) { const n = padNotes[KEYMAP[k]]; if (n !== undefined) hitPad(n); return; }
      const note = (+$('aud-oct').value + 1) * 12 + KEYMAP[k];
      heldNotes.add(note);
      sendNoteOn(note);
    }
  };
  const onKeyUp = (e) => {
    const k = e.key.toLowerCase();
    if (k in KEYMAP && keysDown.has(k)) {
      keysDown.delete(k);
      if (isKitSafe()) return;
      const note = (+$('aud-oct').value + 1) * 12 + KEYMAP[k];
      heldNotes.delete(note);
      sendNoteOff(note);
    }
  };

  // Status feed: light the pads of drums actually sounding.
  const onStatus = (s) => {
    const drumSet = new Set(String(s.drumNotes || '').split(',').filter(Boolean).map(Number));
    root.querySelectorAll('#kbd .pad').forEach((el) => el.classList.toggle('lit', drumSet.has(+el.dataset.note)));
  };
  client.addEventListener('status', (e) => onStatus(e.detail));

  buildKeyboard();
  if (store.bankDoc && store.kitDoc) refreshTargets();

  return {
    activate() {
      if (active) return;
      active = true;
      document.addEventListener('keydown', onKeyDown);
      document.addEventListener('keyup', onKeyUp);
    },
    deactivate() {
      if (!active) return;
      active = false;
      document.removeEventListener('keydown', onKeyDown);
      document.removeEventListener('keyup', onKeyUp);
      for (const n of heldNotes) sendNoteOff(n);
      heldNotes.clear();
      keysDown.clear();
    },
    refreshTargets,
    selectProgram(prog) {
      $('edit-mode').value = 'bank';
      refreshTargets(String(prog));
      updateModeUI();
    },
    selectDrum(note) {
      $('edit-mode').value = 'kit';
      refreshTargets(String(note));
      updateModeUI();
    },
    applyNow,
    undo: doUndo,
    redo: doRedo,
    canUndo: () => hist(undoStacks).length > 0,
    canRedo: () => hist(redoStacks).length > 0,
    importDoc,
    noteOn: (note, vel) => { heldNotes.add(note); sendNoteOn(note, vel); },
    noteOff: (note) => { heldNotes.delete(note); sendNoteOff(note); },
    midiMessage(d) {
      const kind = d[0] & 0xF0;
      const ch = getAuditionChannel();
      switch (kind) {
        case 0x90:
          if (d[2] > 0) { heldNotes.add(d[1]); sendNoteOn(d[1], d[2]); }
          else { heldNotes.delete(d[1]); sendNoteOff(d[1]); }
          break;
        case 0x80: heldNotes.delete(d[1]); sendNoteOff(d[1]); break;
        case 0xB0: client.cc(ch, d[1], d[2]); break;
        case 0xE0: client.bend(ch, ((d[2] << 7) | d[1]) - 8192); break;
        case 0xD0: client.aftertouch(ch, d[1]); break;
        default: break;
      }
    },
    get patch() { return patch; },
    get mode() { return $('edit-mode').value; },
    get key() { return targetKey(); },
    store,
  };
}
