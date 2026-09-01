//
// Authoring tool UI. The synth runs inside the AudioWorklet; this file is
// pure presentation: structured controls <-> patch object <-> live apply
// (single-program hot swap; only voices on the edited patch restart), plus a
// live output scope, draggable envelope editors and a wavetable preview.
//
const GM_NAMES = ["Acoustic Grand Piano","Bright Acoustic Piano","Electric Grand Piano","Honky-tonk Piano","Electric Piano 1","Electric Piano 2","Harpsichord","Clavinet","Celesta","Glockenspiel","Music Box","Vibraphone","Marimba","Xylophone","Tubular Bells","Dulcimer","Drawbar Organ","Percussive Organ","Rock Organ","Church Organ","Reed Organ","Accordion","Harmonica","Tango Accordion","Acoustic Guitar (nylon)","Acoustic Guitar (steel)","Electric Guitar (jazz)","Electric Guitar (clean)","Electric Guitar (muted)","Overdriven Guitar","Distortion Guitar","Guitar Harmonics","Acoustic Bass","Electric Bass (finger)","Electric Bass (pick)","Fretless Bass","Slap Bass 1","Slap Bass 2","Synth Bass 1","Synth Bass 2","Violin","Viola","Cello","Contrabass","Tremolo Strings","Pizzicato Strings","Orchestral Harp","Timpani","String Ensemble 1","String Ensemble 2","Synth Strings 1","Synth Strings 2","Choir Aahs","Voice Oohs","Synth Voice","Orchestra Hit","Trumpet","Trombone","Tuba","Muted Trumpet","French Horn","Brass Section","Synth Brass 1","Synth Brass 2","Soprano Sax","Alto Sax","Tenor Sax","Baritone Sax","Oboe","English Horn","Bassoon","Clarinet","Piccolo","Flute","Recorder","Pan Flute","Blown Bottle","Shakuhachi","Whistle","Ocarina","Lead 1 (square)","Lead 2 (sawtooth)","Lead 3 (calliope)","Lead 4 (chiff)","Lead 5 (charang)","Lead 6 (voice)","Lead 7 (fifths)","Lead 8 (bass+lead)","Pad 1 (new age)","Pad 2 (warm)","Pad 3 (polysynth)","Pad 4 (choir)","Pad 5 (bowed)","Pad 6 (metallic)","Pad 7 (halo)","Pad 8 (sweep)","FX 1 (rain)","FX 2 (soundtrack)","FX 3 (crystal)","FX 4 (atmosphere)","FX 5 (brightness)","FX 6 (goblins)","FX 7 (echoes)","FX 8 (sci-fi)","Sitar","Banjo","Shamisen","Koto","Kalimba","Bag pipe","Fiddle","Shanai","Tinkle Bell","Agogo","Steel Drums","Woodblock","Taiko Drum","Melodic Tom","Synth Drum","Reverse Cymbal","Guitar Fret Noise","Breath Noise","Seashore","Bird Tweet","Telephone Ring","Helicopter","Applause","Gunshot"];
const DRUM_NAMES = {35:"Acoustic Bass Drum",36:"Bass Drum 1",37:"Side Stick",38:"Acoustic Snare",39:"Hand Clap",40:"Electric Snare",41:"Low Floor Tom",42:"Closed Hi-Hat",43:"High Floor Tom",44:"Pedal Hi-Hat",45:"Low Tom",46:"Open Hi-Hat",47:"Low-Mid Tom",48:"Hi-Mid Tom",49:"Crash Cymbal 1",50:"High Tom",51:"Ride Cymbal 1",52:"Chinese Cymbal",53:"Ride Bell",54:"Tambourine",55:"Splash Cymbal",56:"Cowbell",57:"Crash Cymbal 2",58:"Vibraslap",59:"Ride Cymbal 2",60:"Hi Bongo",61:"Low Bongo",62:"Mute Hi Conga",63:"Open Hi Conga",64:"Low Conga",65:"High Timbale",66:"Low Timbale",67:"High Agogo",68:"Low Agogo",69:"Cabasa",70:"Maracas",71:"Short Whistle",72:"Long Whistle",73:"Short Guiro",74:"Long Guiro",75:"Claves",76:"Hi Wood Block",77:"Low Wood Block",78:"Mute Cuica",79:"Open Cuica",80:"Mute Triangle",81:"Open Triangle"};
const MOD_SOURCES = ["LFO1","LFO2","AmpEnv","FilterEnv","PitchEnv","WtPosEnv","Velocity","KeyTrack","ModWheel","ChannelAftertouch"];
const MOD_DESTS = ["Pitch","FilterCutoff","FilterRes","Amplitude","WtPosition","Pan"];
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

const $ = (id) => document.getElementById(id);
const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));

let port = null;
let analyser = null;
let bankDoc = null, kitDoc = null;
let patch = null;      // working copy of the selected patch
let layerIdx = 0;
let envKind = 0;
let status = { durationMs: 0 };
let seekDragging = false;
const mutedChannels = new Set();
const heldNotes = new Set();  // audition notes currently down (for retrigger)

// ---------------- logging / apply state ----------------
function log(msg, cls = '') {
  const el = document.createElement('div');
  if (cls) el.className = cls;
  el.textContent = msg;
  $('log').appendChild(el);
  $('log').scrollTop = $('log').scrollHeight;
  while ($('log').childNodes.length > 150) $('log').removeChild($('log').firstChild);
}
function setApplyState(text, cls) {
  const el = $('apply-state');
  el.textContent = text;
  el.className = cls || '';
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
    analyser = ctx.createAnalyser();
    analyser.fftSize = 4096;
    analyser.smoothingTimeConstant = 0.6;
    node.connect(analyser);
    analyser.connect(ctx.destination);
    port = node.port;
    port.onmessage = (e) => onWorkletMessage(e.data);
    await ctx.resume();
    requestAnimationFrame(drawScope);
  } catch (err) {
    $('overlay').querySelector('p').textContent = 'Failed to start audio: ' + err;
    $('start-btn').disabled = false;
  }
};

async function onWorkletMessage(m) {
  switch (m.type) {
    case 'ready': {
      $('overlay').remove();
      const [bankText, kitText] = await Promise.all([
        fetch('../content/patches/gm_bank.json').then((r) => r.text()),
        fetch('../content/drums/gm_kit.json').then((r) => r.text()),
      ]);
      bankDoc = JSON.parse(bankText);
      kitDoc = JSON.parse(kitText);
      port.postMessage({ type: 'loadBank', json: bankText });
      port.postMessage({ type: 'loadDrums', json: kitText });
      refreshTargets();
      setupMidi();
      break;
    }
    case 'fatal': log('worklet failed: ' + m.error, 'err'); break;
    case 'report': {
      const r = m.report;
      for (const w of r.warnings || []) log(`[${m.what}] ${w}`, 'warn');
      for (const e of r.errors || []) log(`[${m.what}] ${e}`, 'err');
      if (m.what === 'patch')
        setApplyState(r.ok ? (r.warnings.length ? '✓ applied (warnings)' : '✓ applied')
                           : '✗ rejected', r.ok ? (r.warnings.length ? 'warn' : 'ok') : 'err');
      else if (r.ok) log(`[${m.what}] loaded ok`, 'ok');
      break;
    }
    case 'bakedTrack': {
      const a = document.createElement('a');
      a.href = URL.createObjectURL(new Blob([m.bytes], { type: 'application/octet-stream' }));
      a.download = (m.name || 'track') + '.mstk';
      a.click();
      log('baked track downloaded: ' + a.download +
          ` (${(m.bytes.byteLength / 1024).toFixed(1)} KB, tree-shaken)`, 'ok');
      break;
    }
    case 'status': status = m; updateStatus(m); break;
  }
}

// ---------------- live output scope ----------------
const scopeTime = new Uint8Array(4096);
let scopeFreq = null;
function drawScope() {
  requestAnimationFrame(drawScope);
  const c = $('scope');
  const dpr = window.devicePixelRatio || 1;
  const w = c.clientWidth * dpr, h = c.clientHeight * dpr;
  if (c.width !== w || c.height !== h) { c.width = w; c.height = h; }
  const g = c.getContext('2d');
  g.fillStyle = '#101318';
  g.fillRect(0, 0, w, h);
  if (!analyser) return;
  if (!scopeFreq) scopeFreq = new Uint8Array(analyser.frequencyBinCount);

  // Spectrum (log-x bars), faint, behind the waveform.
  analyser.getByteFrequencyData(scopeFreq);
  g.fillStyle = 'rgba(74,144,217,0.28)';
  const bars = 96;
  for (let i = 0; i < bars; i++) {
    const f0 = Math.pow(scopeFreq.length, i / bars);
    const f1 = Math.pow(scopeFreq.length, (i + 1) / bars);
    let peak = 0;
    for (let k = Math.floor(f0); k < Math.max(f0 + 1, f1); k++) peak = Math.max(peak, scopeFreq[k] || 0);
    const bh = (peak / 255) * h * 0.9;
    g.fillRect((i / bars) * w, h - bh, w / bars - 1 * dpr, bh);
  }

  // Waveform with a rising-edge trigger for a stable trace.
  analyser.getByteTimeDomainData(scopeTime);
  let start = 0;
  for (let i = 1; i < 2048; i++)
    if (scopeTime[i - 1] < 128 && scopeTime[i] >= 128) { start = i; break; }
  g.strokeStyle = '#6fd66f';
  g.lineWidth = 1.5 * dpr;
  g.beginPath();
  const n = 2048;
  for (let i = 0; i < n; i++) {
    const v = scopeTime[start + i] / 255;
    const x = (i / n) * w;
    const y = h - v * h;
    if (i === 0) g.moveTo(x, y); else g.lineTo(x, y);
  }
  g.stroke();
  g.strokeStyle = 'rgba(255,255,255,0.08)';
  g.lineWidth = 1;
  g.beginPath(); g.moveTo(0, h / 2); g.lineTo(w, h / 2); g.stroke();
}

// ---------------- status / transport / channels ----------------
function fmtTime(ms) {
  const s = Math.floor(ms / 1000);
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
}
function updateStatus(s) {
  $('m-voices').textContent = s.activeVoices;
  $('m-peak').textContent = s.peakVoices;
  $('m-steals').textContent = s.steals;
  $('m-drops').textContent = s.drops;
  $('m-bar').textContent = s.bar + 1;
  $('m-beat').textContent = s.beat + 1;
  $('t-cur').textContent = fmtTime(s.timeMs);
  $('t-dur').textContent = fmtTime(s.durationMs);
  if (!seekDragging && s.durationMs > 0)
    $('seek').value = Math.round((s.timeMs / s.durationMs) * 1000);
  $('btn-play').classList.toggle('active', s.playing);
  document.querySelectorAll('#channels .ch').forEach((el, i) => {
    el.classList.toggle('lit', s.channelNotes[i] > 0);
    if (s.channelPrograms)
      el.title = (i === 9 ? 'drums' : `${s.channelPrograms[i]} ${GM_NAMES[s.channelPrograms[i]]}`) +
                 ' — click: mute/unmute (300ms fade)';
  });
  const drumSet = new Set((s.drumNotes || '').split(',').filter(Boolean).map(Number));
  document.querySelectorAll('#instruments .inst').forEach((el) => {
    let lit;
    if (el.dataset.note !== undefined) {
      // Drum rows light per NOTE actually sounding, not per channel.
      lit = drumSet.has(+el.dataset.note);
    } else {
      // Melodic rows: a channel sounds AND its current program matches
      // (mid-song program changes track correctly).
      const chs = el.dataset.channels.split(',').map(Number);
      const prog = +el.dataset.prog;
      lit = chs.some((c) => s.channelNotes[c] > 0 &&
          (!s.channelPrograms || s.channelPrograms[c] === prog));
    }
    el.classList.toggle('lit', lit);
  });
  document.querySelectorAll('#kbd .pad').forEach((el) =>
    el.classList.toggle('lit', drumSet.has(+el.dataset.note)));
}

for (let i = 0; i < 16; i++) {
  const el = document.createElement('div');
  el.className = 'ch';
  el.innerHTML = `${i + 1}${i === 9 ? 'd' : ''}<span class="dot"></span>`;
  el.title = 'click: mute/unmute (300ms fade)';
  el.onclick = () => {
    const muted = !mutedChannels.has(i);
    if (muted) mutedChannels.add(i); else mutedChannels.delete(i);
    el.classList.toggle('muted', muted);
    port.postMessage({ type: 'channelMute', ch: i, muted, fadeMs: 300 });
  };
  $('channels').appendChild(el);
}

$('btn-play').onclick = () => port.postMessage({ type: 'play' });
$('btn-pause').onclick = () => port.postMessage({ type: 'pause' });
$('btn-stop').onclick = () => port.postMessage({ type: 'stop' });
$('chk-loop').onchange = (e) => port.postMessage({ type: 'setLoop', on: e.target.checked });
$('chk-dither').onchange = (e) => port.postMessage({ type: 'dither', on: e.target.checked });
$('seek').onpointerdown = () => { seekDragging = true; };
$('seek').onchange = (e) => {
  seekDragging = false;
  if (status.durationMs > 0)
    port.postMessage({ type: 'seekMs', ms: Math.round((e.target.value / 1000) * status.durationMs) });
};
$('tempo').oninput = (e) => {
  $('tempo-val').textContent = (e.target.value / 1000).toFixed(2) + '×';
  port.postMessage({ type: 'tempoScale', milli: +e.target.value });
};
$('volume').oninput = (e) => port.postMessage({ type: 'masterVolume', milli: +e.target.value });

let lastSongBytes = null;   // kept for the offline render worker + baking
let lastSongIsTrack = false;
let lastSongName = 'song';
const isMstk = (u8) => u8.length >= 4 && u8[0] === 0x4D && u8[1] === 0x53 &&
                       u8[2] === 0x54 && u8[3] === 0x4B;  // "MSTK"
async function loadSongBytes(buf, name) {
  lastSongBytes = buf.slice(0);  // the original is transferred to the worklet
  if (name) lastSongName = name.replace(/\.(midi?|mstk)$/i, '');
  const u8 = new Uint8Array(lastSongBytes);
  lastSongIsTrack = isMstk(u8);
  if (lastSongIsTrack) {
    songInstruments = null;
    renderInstruments();
    $('instruments').textContent =
        'baked .mstk track — content is embedded and pre-shaken';
    $('instruments').className = 'hint';
    port.postMessage({ type: 'loadTrack', bytes: buf }, [buf]);
  } else {
    scanInstruments(u8);
    port.postMessage({ type: 'loadSong', bytes: buf }, [buf]);
  }
  port.postMessage({ type: 'setLoop', on: $('chk-loop').checked });
  port.postMessage({ type: 'play' });
}

// ---------------- song instrument scan ----------------
// Minimal SMF walk (running status aware): which programs each channel uses,
// and which drum notes channel 10 hits. Mirrors tools/scan_programs.py.
let songInstruments = null;  // { programs: Map<prog, Set<ch>>, drums: Set<note>, activeCh: Map }
function scanInstruments(d) {
  const programs = new Map();   // prog -> Set of channels
  const drums = new Set();      // drum notes hit on ch 10
  const channelsWithNotes = new Set();
  const chProg = new Array(16).fill(0);  // running program per channel
  const noteProg = (prog, ch) => {
    if (!programs.has(prog)) programs.set(prog, new Set());
    programs.get(prog).add(ch);
  };
  if (d.length < 14 || d[0] !== 0x4D || d[1] !== 0x54) { songInstruments = null; renderInstruments(); return; }
  let pos = 8 + ((d[4] << 24) | (d[5] << 16) | (d[6] << 8) | d[7]);
  const vlq = (p) => {
    let v = 0;
    for (let i = 0; i < 4; i++) {
      const b = d[p++];
      v = (v << 7) | (b & 0x7F);
      if (!(b & 0x80)) break;
    }
    return [v, p];
  };
  while (pos + 8 <= d.length) {
    const kind = String.fromCharCode(d[pos], d[pos + 1], d[pos + 2], d[pos + 3]);
    const clen = (d[pos + 4] << 24) | (d[pos + 5] << 16) | (d[pos + 6] << 8) | d[pos + 7];
    let p = pos + 8;
    const end = Math.min(p + clen, d.length);
    pos = p + clen;
    if (kind !== 'MTrk') continue;
    let running = 0;
    while (p < end) {
      [, p] = vlq(p);
      if (p >= end) break;
      let st = d[p];
      if (st < 0x80) st = running;
      else { p++; if (st < 0xF0) running = st; }
      if (st === 0xFF) { p++; let ml; [ml, p] = vlq(p); p += ml; continue; }
      if (st === 0xF0 || st === 0xF7) { let sl; [sl, p] = vlq(p); p += sl; continue; }
      if (st < 0x80) break;  // corrupt: stop this track
      const hi = st & 0xF0, ch = st & 0x0F;
      const nbytes = (hi === 0xC0 || hi === 0xD0) ? 1 : 2;
      const d1 = d[p], d2 = d[p + 1];
      p += nbytes;
      if (hi === 0xC0) chProg[ch] = d1 & 0x7F;
      else if (hi === 0x90 && (d2 & 0x7F) > 0) {
        channelsWithNotes.add(ch);
        if (ch === 9) drums.add(d1 & 0x7F);
        else noteProg(chProg[ch], ch);
      }
    }
  }
  songInstruments = { programs, drums, channelsWithNotes };
  renderInstruments();
}

function renderInstruments() {
  const box = $('instruments');
  box.innerHTML = '';
  if (!songInstruments || (!songInstruments.programs.size && !songInstruments.drums.size)) {
    box.textContent = 'load a song to see its instruments';
    box.className = 'hint';
    return;
  }
  box.className = '';
  const rows = [...songInstruments.programs.entries()].sort((a, b) => a[0] - b[0]);
  for (const [prog, chs] of rows) {
    const authored = !!bankDoc?.programs?.[String(prog)];
    const el = document.createElement('div');
    el.className = 'inst';
    el.dataset.channels = [...chs].join(',');
    el.dataset.prog = prog;
    el.innerHTML = `<span class="now"></span><span class="prog">${prog}</span>
      <span>${GM_NAMES[prog]}</span>
      <span class="chs">ch ${[...chs].map((c) => c + 1).sort((a, b) => a - b).join(',')}</span>
      ${authored ? '' : '<span class="flag">fallback</span>'}`;
    el.title = 'click to edit this patch';
    el.onclick = () => {
      $('edit-mode').value = 'bank';
      refreshTargets();
      updateModeUI();
      $('edit-target').value = String(prog);
      selectTarget();
    };
    box.appendChild(el);
  }
  if (songInstruments.drums.size) {
    const notes = [...songInstruments.drums].sort((a, b) => a - b);
    for (const n of notes) {
      const mapped = !!kitDoc?.notes?.[String(n)];
      const el = document.createElement('div');
      el.className = 'inst';
      el.dataset.channels = '9';
      el.dataset.note = n;
      el.innerHTML = `<span class="now"></span><span class="prog">${n}</span>
        <span>${DRUM_NAMES[n] || 'drum ' + n}</span>
        <span class="chs">ch 10</span>
        ${mapped ? '' : '<span class="flag" style="color:var(--bad)">silent!</span>'}`;
      el.title = 'click to edit this drum';
      el.onclick = () => {
        $('edit-mode').value = 'kit';
        refreshTargets();
        updateModeUI();
        $('edit-target').value = String(n);
        selectTarget();
      };
      box.appendChild(el);
    }
  }
}
$('song-file').onchange = async (e) => {
  const f = e.target.files[0];
  if (f) loadSongBytes(await f.arrayBuffer(), f.name);
};
document.querySelectorAll('[data-song]').forEach((btn) => {
  btn.onclick = async () =>
    loadSongBytes(await (await fetch('../' + btn.dataset.song)).arrayBuffer(),
                  btn.dataset.song.split('/').pop());
});

// ---------------- offline render (worker: same engine, faster than realtime) ----------------
let renderWorker = null;
$('btn-render').onclick = () => {
  if (!lastSongBytes) { log('render: load a song first', 'err'); return; }
  if (renderWorker) { log('render already running', 'warn'); return; }
  applyNow();  // fold any pending edit into the docs
  const btn = $('btn-render');
  btn.disabled = true;
  $('render-state').textContent = 'rendering… 0%';
  $('render-bar-wrap').style.display = 'block';
  $('render-bar').style.width = '0%';
  renderWorker = new Worker('./render-worker.js', { type: 'module' });
  renderWorker.onmessage = (e) => {
    const m = e.data;
    if (m.type === 'progress') {
      $('render-state').textContent = `rendering… ${m.pct}%`;
      $('render-bar').style.width = m.pct + '%';
    } else if (m.type === 'reports') {
      for (const [what, r] of m.reports)
        for (const w of r.warnings || []) log(`[render:${what}] ${w}`, 'warn');
    } else if (m.type === 'done') {
      const a = document.createElement('a');
      a.href = URL.createObjectURL(new Blob([m.wav], { type: 'audio/wav' }));
      a.download = lastSongName + '.wav';
      a.click();
      $('render-bar').style.width = '100%';
      setTimeout(() => { $('render-bar-wrap').style.display = 'none'; }, 800);
      $('render-state').textContent =
        `done: ${(m.frames / 48000).toFixed(1)}s · peak ${m.peakVoices} voices` +
        (m.drops > 0 ? ` · ${m.drops} drops!` : '');
      log(`render done (${(m.frames / 48000).toFixed(1)}s, peak ${m.peakVoices} voices, ` +
          `${m.steals} steals)`, 'ok');
      renderWorker.terminate(); renderWorker = null; btn.disabled = false;
    } else if (m.type === 'error') {
      log('render failed: ' + m.error, 'err');
      $('render-state').textContent = 'render failed';
      $('render-bar-wrap').style.display = 'none';
      renderWorker.terminate(); renderWorker = null; btn.disabled = false;
    }
  };
  const buf = lastSongBytes.slice(0);
  renderWorker.postMessage({
    bankJson: JSON.stringify(bankDoc),
    kitJson: JSON.stringify(kitDoc),
    songBytes: buf,
    seconds: +($('render-secs').value || 0),
    dither: $('chk-dither').checked,
    loop: false,
  }, [buf]);
};
document.querySelectorAll('[data-fx]').forEach((el) => {
  el.oninput = () => {
    const [fx, id] = el.dataset.fx.split(':');
    port.postMessage({ type: fx + 'Param', id: +id, milli: +el.value });
  };
});

// ---------------- patch model ----------------
const isKit = () => $('edit-mode').value === 'kit';
const isKitSafe = () => { try { return isKit() && kitDoc; } catch { return false; } };
const targetKey = () => $('edit-target').value;

function defaultEnv(kind) {
  if (kind === 'ampEnv') return { attack: 1, decay: 0, sustain: 1.0, release: 50 };
  return { attack: 0, decay: 0, sustain: 1.0, release: 50 };
}
function defaultLayer() {
  return {
    wavetable: { generator: 'additiveHarmonicMorph', frameCount: 1,
                 params: { startHarmonics: 1, endHarmonics: 8, falloff: 'saw' } },
    mixGain: 0.6,
    ampEnv: defaultEnv('ampEnv'),
  };
}
function defaultPatch() {
  return { name: 'new', layers: [defaultLayer()] };
}
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

function refreshTargets() {
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
      o.textContent = `${p} ${GM_NAMES[p]}${bankDoc.programs[String(p)] ? '' : '  ·fallback'}`;
      sel.appendChild(o);
    }
    sel.value = '48';
  } else {
    for (let n = 27; n < 88; n++) {
      const o = document.createElement('option');
      o.value = n;
      o.textContent = `${n} ${DRUM_NAMES[n] || '(unnamed)'}${kitDoc.notes[String(n)] ? '' : '  ·silent'}`;
      sel.appendChild(o);
    }
    sel.value = '36';
  }
  selectTarget();
}
function selectTarget() {
  const key = targetKey();
  let src;
  if (!isKit()) {
    src = key === 'fallback' ? bankDoc.fallback : bankDoc.programs[key];
    if (key !== 'fallback') port.postMessage({ type: 'program', ch: 0, prog: +key });
  } else {
    src = kitDoc.notes[key];
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
let applyTimer = null;
function scheduleApply(delay = 300) {
  setApplyState('…editing');
  clearTimeout(applyTimer);
  applyTimer = setTimeout(applyNow, delay);
}
const undoStack = [];
let abSnapshot = null;
function pushUndo() {
  const key = targetKey();
  const prev = isKit() ? kitDoc.notes[key]
      : key === 'fallback' ? bankDoc.fallback : bankDoc.programs[key];
  undoStack.push({ mode: $('edit-mode').value, key, json: JSON.stringify(prev ?? null) });
  if (undoStack.length > 50) undoStack.shift();
}
function applyNow() {
  clearTimeout(applyTimer);
  pushUndo();
  const key = targetKey();
  const clean = structuredClone(patch);
  if (!isKit()) {
    if (key === 'fallback') {
      bankDoc.fallback = clean;
      port.postMessage({ type: 'loadBank', json: JSON.stringify(bankDoc) });
    } else {
      bankDoc.programs[key] = clean;
      port.postMessage({ type: 'loadProgram', prog: +key, json: JSON.stringify(clean) });
    }
  } else {
    kitDoc.notes[key] = clean;
    port.postMessage({ type: 'loadDrumNote', note: +key, json: JSON.stringify(clean) });
  }
  if ($('json-drawer').classList.contains('open'))
    $('editor').value = JSON.stringify(patch, null, 2);
  // Retrigger held audition notes so a tweak is heard without replaying;
  // drums are one-shots, so auto-hit the edited drum instead.
  setTimeout(() => {
    if (isKit()) hitPad(+key);
    else for (const n of heldNotes) sendNoteOn(n);
  }, 60);
}

// ---------------- generic control binding ----------------
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
const bound = [];
function B(...args) { bound.push(bind(...args)); }

// patch-level
B('p-name', (el) => { el.value = patch.name || ''; }, (el) => { patch.name = el.value; });
B('p-oneshot', (el) => { el.checked = !!patch.oneShot; }, (el) => { patch.oneShot = el.checked; });
B('p-choke', (el) => { el.value = patch.exclusiveGroup ?? -1; },
  (el) => { patch.exclusiveGroup = +el.value; });

// oscillator
function wtParams() {
  const l = layer();
  if (!l.wavetable) l.wavetable = defaultLayer().wavetable;
  if (!l.wavetable.params) l.wavetable.params = { startHarmonics: 1, endHarmonics: 8, falloff: 'saw' };
  return l.wavetable;
}
B('l-osc', (el) => { el.value = layer().osc === 'noise' ? 'noise' : 'wavetable'; },
  (el) => { layer().osc = el.value; }, { redraw: syncOscVisibility });
B('wt-gen', (el) => { el.value = wtParams().generator || 'additiveHarmonicMorph'; },
  (el) => {
    const wt = wtParams();
    wt.generator = el.value;
    wt.params = defaultGenParams(el.value);  // fresh sensible params per generator
  }, { redraw: () => { renderGenParams(); drawWavetable(); drawLayers(); } });
B('wt-frames', (el) => { el.value = wtParams().frameCount ?? 1; },
  (el) => { wtParams().frameCount = clamp(+el.value, 1, 32); }, { redraw: drawWavetable });

// ---------------- generator parameter specs (drive the dynamic UI) ----------
const GEN_SPECS = {
  additiveHarmonicMorph: [
    { key: 'falloff', label: 'falloff', type: 'sel', options: ['saw', 'square', 'triangle', 'flat'], def: 'saw' },
    { key: 'startHarmonics', label: 'harmonics', type: 'num', min: 1, max: 366, step: 1, def: 1 },
    { key: 'endHarmonics', label: '→', type: 'num', min: 1, max: 366, step: 1, def: 8 },
  ],
  pulse: [
    { key: 'startDuty', label: 'duty', type: 'num', min: 0.02, max: 0.98, step: 0.01, def: 0.5 },
    { key: 'endDuty', label: '→', type: 'num', min: 0.02, max: 0.98, step: 0.01, def: 0.15 },
  ],
  drawbar: [
    { key: 'drawbars', label: 'bars 16′..1′', type: 'bars', def: [8, 0, 6, 0, 0, 4, 0, 0, 0] },
  ],
  waveshaper: [
    { key: 'shape', label: 'shape', type: 'sel', options: ['soft', 'hard', 'fold'], def: 'soft' },
    { key: 'startDrive', label: 'drive', type: 'num', min: 0.25, max: 64, step: 0.25, def: 1 },
    { key: 'endDrive', label: '→', type: 'num', min: 0.25, max: 64, step: 0.25, def: 8 },
  ],
  fm2op: [
    { key: 'ratio', label: 'ratio', type: 'num', min: 1, max: 16, step: 1, def: 2 },
    { key: 'startIndex', label: 'index', type: 'num', min: 0, max: 12, step: 0.05, def: 0.5 },
    { key: 'endIndex', label: '→', type: 'num', min: 0, max: 12, step: 0.05, def: 4 },
  ],
};
function defaultGenParams(gen) {
  const p = {};
  for (const f of GEN_SPECS[gen] || [])
    p[f.key] = Array.isArray(f.def) ? [...f.def] : f.def;
  return p;
}
function renderGenParams() {
  const box = $('gen-params');
  box.innerHTML = '';
  if (layer().osc === 'noise') return;
  const wt = wtParams();
  const gen = wt.generator || 'additiveHarmonicMorph';
  if (!wt.params) wt.params = defaultGenParams(gen);
  for (const f of GEN_SPECS[gen] || []) {
    const lab = document.createElement('label');
    lab.textContent = f.label;
    box.appendChild(lab);
    if (f.type === 'sel') {
      const sel = document.createElement('select');
      for (const o of f.options) {
        const opt = document.createElement('option');
        opt.textContent = o;
        sel.appendChild(opt);
      }
      sel.value = wt.params[f.key] ?? f.def;
      sel.onchange = () => { wt.params[f.key] = sel.value; drawWavetable(); drawLayers(); scheduleApply(150); };
      box.appendChild(sel);
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
        inp.onchange = () => { bars[i] = clamp(+inp.value, 0, 8); drawWavetable(); drawLayers(); scheduleApply(150); };
        box.appendChild(inp);
      });
    } else {
      const inp = document.createElement('input');
      inp.type = 'number';
      inp.className = 'narrow';
      inp.min = f.min; inp.max = f.max; inp.step = f.step;
      inp.value = wt.params[f.key] ?? f.def;
      inp.onchange = () => { wt.params[f.key] = clamp(+inp.value, f.min, f.max); drawWavetable(); drawLayers(); scheduleApply(150); };
      box.appendChild(inp);
    }
  }
}

// mix & pitch
B('l-gain', (el) => { el.value = Math.round((layer().mixGain ?? 1) * 1000); },
  (el) => { layer().mixGain = +el.value / 1000; $('l-gain-val').textContent = (+el.value / 1000).toFixed(2); });
B('l-pan', (el) => { el.value = Math.round((layer().pan ?? 0) * 1000); },
  (el) => { layer().pan = +el.value / 1000; $('l-pan-val').textContent = panLabel(+el.value / 1000); });
B('l-detune', (el) => { el.value = layer().detuneCents ?? 0; },
  (el) => { layer().detuneCents = +el.value; });
B('l-velcurve', (el) => { el.value = layer().velCurve || 'squared'; },
  (el) => {
    if (el.value === 'squared') delete layer().velCurve;
    else layer().velCurve = el.value;
  });
B('l-vlow', (el) => { el.value = layer().velLow ?? 0; }, (el) => { layer().velLow = clamp(+el.value, 0, 127); });
B('l-vhigh', (el) => { el.value = layer().velHigh ?? 127; }, (el) => { layer().velHigh = clamp(+el.value, 0, 127); });
B('l-pitchnote', (el) => { el.value = layer().pitchNote ?? -1; },
  (el) => {
    const v = +el.value;
    if (v < 0) delete layer().pitchNote; else layer().pitchNote = v;
  });
function panLabel(p) { return Math.abs(p) < 0.005 ? 'C' : (p < 0 ? `L${Math.round(-p * 100)}` : `R${Math.round(p * 100)}`); }

// envelopes
B('env-a', (el) => { el.value = env().attack ?? 0; }, (el) => { env().attack = Math.max(0, +el.value); }, { redraw: drawEnv });
B('env-d', (el) => { el.value = env().decay ?? 0; }, (el) => { env().decay = Math.max(0, +el.value); }, { redraw: drawEnv });
B('env-s', (el) => { el.value = env().sustain ?? 1; }, (el) => { env().sustain = clamp(+el.value, 0, 1); }, { redraw: drawEnv });
B('env-r', (el) => { el.value = env().release ?? 0; }, (el) => { env().release = Math.max(0, +el.value); }, { redraw: drawEnv });
B('env-ca', (el) => { el.value = env().attackCurve || 'linear'; }, (el) => { env().attackCurve = el.value; }, { redraw: drawEnv });
B('env-cd', (el) => { el.value = env().decayCurve || 'exp'; }, (el) => { env().decayCurve = el.value; }, { redraw: drawEnv });
B('env-cr', (el) => { el.value = env().releaseCurve || 'exp'; }, (el) => { env().releaseCurve = el.value; }, { redraw: drawEnv });
B('env-depth', (el) => {
    const k = ENV_KINDS[envKind];
    el.value = k.depth ? (env()[k.depth] ?? (k.depth === 'depth' && k.key === 'filterEnv' ? 0.5 : k.key === 'wtPosEnv' ? 1 : 0)) : 0;
  },
  (el) => {
    const k = ENV_KINDS[envKind];
    if (k.depth) env()[k.depth] = +el.value;
  });

// filter
const hzFromSlider = (v) => Math.round(20 * Math.pow(600, v / 1000));
const sliderFromHz = (hz) => Math.round(1000 * Math.log(clamp(hz, 20, 12000) / 20) / Math.log(600));
function filt() {
  const l = layer();
  if (!l.filter) l.filter = { mode: 'bypass', cutoffHz: 1200, resonance: 0 };
  return l.filter;
}
B('f-mode', (el) => { el.value = layer().filter?.mode || 'bypass'; },
  (el) => { filt().mode = el.value; }, { redraw: drawFilterResp });
B('f-cut', (el) => { el.value = sliderFromHz(layer().filter?.cutoffHz ?? 1200); },
  (el) => { filt().cutoffHz = hzFromSlider(+el.value); $('f-cut-val').textContent = filt().cutoffHz + ' Hz'; },
  { redraw: drawFilterResp });
B('f-res', (el) => { el.value = Math.round((layer().filter?.resonance ?? 0) * 1000); },
  (el) => { filt().resonance = +el.value / 1000; $('f-res-val').textContent = (+el.value / 1000).toFixed(2); },
  { redraw: drawFilterResp });
B('f-key', (el) => { el.value = Math.round((layer().keyTrackFilter ?? 0) * 1000); },
  (el) => { layer().keyTrackFilter = +el.value / 1000; $('f-key-val').textContent = (+el.value / 1000).toFixed(2); });

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
    del.onclick = () => {
      patch.layers.splice(layerIdx, 1);
      layerIdx = Math.max(0, layerIdx - 1);
      syncUI();
      scheduleApply();
    };
    tabs.appendChild(del);
  }
  if ((patch.layers || []).length < 4) {
    const add = document.createElement('button');
    add.textContent = '＋';
    add.title = 'add layer';
    add.onclick = () => {
      patch.layers.push(defaultLayer());
      layerIdx = patch.layers.length - 1;
      syncUI();
      scheduleApply();
    };
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
  for (const id of ['env-a', 'env-d', 'env-s', 'env-r', 'env-ca', 'env-cd', 'env-cr', 'env-depth'])
    $(id)._sync();
  const k = ENV_KINDS[envKind];
  $('env-depth-wrap').style.display = k.depth ? '' : 'none';
  $('env-depth-label').textContent = k.dlabel || 'depth';
  drawEnv();
}

// ---------------- LFOs ----------------
function renderLfos() {
  const box = $('lfo-rows');
  box.innerHTML = '';
  const l = layer();
  for (let i = 0; i < 2; i++) {
    const row = document.createElement('div');
    row.className = 'row';
    const has = l.lfos && l.lfos[i];
    row.innerHTML = `
      <label><input type="checkbox" ${has ? 'checked' : ''} data-k="on"> LFO${i + 1}</label>
      <select data-k="shape" ${has ? '' : 'disabled'}>
        ${['sine', 'triangle', 'saw', 'square', 'sampleHold'].map((s) =>
          `<option ${has && l.lfos[i].shape === s ? 'selected' : ''}>${s}</option>`).join('')}
      </select>
      <label>rate Hz</label>
      <input type="number" data-k="rate" class="narrow" step="0.1" min="0" max="40"
             value="${has ? l.lfos[i].rateHz ?? 5 : 5}" ${has ? '' : 'disabled'}>
      <label>delay ms</label>
      <input type="number" data-k="delay" min="0" max="10000"
             value="${has ? l.lfos[i].delayMs ?? 0 : 0}" ${has ? '' : 'disabled'}>`;
    row.querySelector('[data-k=on]').onchange = (e) => {
      if (!l.lfos) l.lfos = [];
      if (e.target.checked) {
        while (l.lfos.length <= i) l.lfos.push({ shape: 'sine', rateHz: 5, delayMs: 0 });
      } else {
        l.lfos.splice(i, 1);
        if (l.lfos.length === 0) delete l.lfos;
      }
      renderLfos();
      scheduleApply();
    };
    if (has) {
      row.querySelector('[data-k=shape]').onchange = (e) => { l.lfos[i].shape = e.target.value; scheduleApply(); };
      row.querySelector('[data-k=rate]').onchange = (e) => { l.lfos[i].rateHz = +e.target.value; scheduleApply(); };
      row.querySelector('[data-k=delay]').onchange = (e) => { l.lfos[i].delayMs = +e.target.value; scheduleApply(); };
    }
    box.appendChild(row);
  }
}

// ---------------- mod matrix ----------------
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
let envDrag = null;
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
  const dpr = window.devicePixelRatio || 1;
  const w = c.clientWidth * dpr, h = c.clientHeight * dpr;
  if (c.width !== w || c.height !== h) { c.width = w; c.height = h; }
  const g = c.getContext('2d');
  g.fillStyle = '#101318';
  g.fillRect(0, 0, w, h);
  const { A, D, S, R, hold, T, oneShot } = envGeometry();
  const e = env();
  const pad = 8 * dpr;
  const X = (ms) => pad + (ms / T) * (w - 2 * pad);
  const Y = (lvl) => h - pad - lvl * (h - 2 * pad);
  const curve = (name) => CURVES[name] || CURVES.linear;

  // grid: 100ms ticks (thin), labels sparse
  g.strokeStyle = 'rgba(255,255,255,0.05)';
  g.fillStyle = 'rgba(255,255,255,0.25)';
  g.font = `${10 * dpr}px ui-monospace`;
  const tick = T > 3000 ? 1000 : T > 800 ? 250 : 100;
  for (let ms = 0; ms <= T; ms += tick) {
    g.beginPath(); g.moveTo(X(ms), 0); g.lineTo(X(ms), h); g.stroke();
    if (ms > 0) g.fillText(ms >= 1000 ? (ms / 1000) + 's' : ms + '', X(ms) + 2, h - 2 * dpr);
  }

  // segments
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

  // handles
  const handles = envHandles();
  g.fillStyle = '#e8e6e1';
  for (const hd of handles) {
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
  const hs = [
    { id: 'a', ms: A, lvl: 1 },
    { id: 'd', ms: A + D, lvl: S },
  ];
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

// ---------------- wavetable preview ----------------
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
// One cycle of generator `gen` at morph position t in [0,1] (float preview
// mirror of the engine's fixed-point generators).
function genWave(gen, P, t, n = 512) {
  const out = new Float64Array(n);
  P = P || {};
  switch (gen) {
    case 'pulse': {
      const duty = lerpNum(P.startDuty ?? 0.5, P.endDuty ?? 0.15, t);
      let mean = 2 * duty - 1;
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
    case 'waveshaper': {
      const drive = lerpNum(P.startDrive ?? 1, P.endDrive ?? 8, t);
      const shape = P.shape || 'soft';
      for (let i = 0; i < n; i++) {
        let x = drive * Math.sin((2 * Math.PI * i) / n);
        if (shape === 'hard') x = Math.max(-1, Math.min(1, x));
        else if (shape === 'fold') {
          for (let g = 0; g < 64; g++) {
            if (x > 1) x = 2 - x;
            else if (x < -1) x = -2 - x;
            else break;
          }
        } else {
          x = Math.abs(x) > 1 ? Math.sign(x) : 1.5 * x - 0.5 * x * x * x;
        }
        out[i] = x;
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
    default: {  // additiveHarmonicMorph
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
// Preview wave of a whole layer at morph t (null for noise layers).
function layerWave(l, t) {
  if (l.osc === 'noise') return null;
  const wt = l.wavetable || {};
  return genWave(wt.generator || 'additiveHarmonicMorph', wt.params, t);
}
function drawWavetable() {
  const c = $('wt-preview');
  const dpr = window.devicePixelRatio || 1;
  const w = c.clientWidth * dpr, h = c.clientHeight * dpr;
  if (c.width !== w || c.height !== h) { c.width = w; c.height = h; }
  const g = c.getContext('2d');
  g.fillStyle = '#101318';
  g.fillRect(0, 0, w, h);
  if (layer().osc === 'noise') {
    g.fillStyle = 'rgba(255,255,255,0.3)';
    g.font = `${11 * dpr}px ui-monospace`;
    g.fillText('noise source', 8 * dpr, 16 * dpr);
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
  const pos = morph * Math.max(0, frames - 1);
  $('wt-morph-val').textContent = frames > 1 ? `frame ${pos.toFixed(1)}` : 'frame 0';
  $('wt-morph-row').style.display = frames > 1 ? '' : 'none';
}
$('wt-morph').oninput = drawWavetable;

function syncOscVisibility() {
  const noise = layer().osc === 'noise';
  $('wt-controls').style.display = noise ? 'none' : '';
  $('noise-hint').style.display = noise ? '' : 'none';
  renderGenParams();
  drawWavetable();
}

// ---------------- layers interaction panel ----------------
const LAYER_COLORS = ['#64b5f6', '#e0b060', '#7bc67b', '#d678d6'];
function drawLayers() {
  const c = $('layers-canvas');
  const dpr = window.devicePixelRatio || 1;
  const w = c.clientWidth * dpr, h = c.clientHeight * dpr;
  if (c.width !== w || c.height !== h) { c.width = w; c.height = h; }
  const g = c.getContext('2d');
  g.fillStyle = '#101318';
  g.fillRect(0, 0, w, h);
  const N = 512;
  const layers = patch.layers || [];
  const sum = new Float64Array(N);
  let sumPeak = 1e-9;
  const waves = layers.map((l) => {
    const wv = layerWave(l, 0);
    if (wv) {
      const gain = l.mixGain ?? 1;
      for (let i = 0; i < N; i++) sum[i] += wv[i] * gain;
    }
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

  // Legend chips: gain/pan/type per layer, click to activate.
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
        `${l.detuneCents ? ' · ' + l.detuneCents + '¢' : ''}`;
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
  const dpr = window.devicePixelRatio || 1;
  const w = c.clientWidth * dpr, h = c.clientHeight * dpr;
  if (c.width !== w || c.height !== h) { c.width = w; c.height = h; }
  const g = c.getContext('2d');
  g.fillStyle = '#101318';
  g.fillRect(0, 0, w, h);
  const layers = patch.layers || [];
  // Shared time axis across all layers' amp envelopes.
  let T = 400;
  const geo = layers.map((l) => {
    const e = l.ampEnv || { attack: 1, decay: 0, sustain: 1, release: 50 };
    const A = e.attack ?? 0, D = e.decay ?? 0, S = e.sustain ?? 1, R = e.release ?? 0;
    const hold = patch.oneShot ? 0 : 120;
    T = Math.max(T, (A + D + hold + R) * 1.08);
    return { A, D, S: patch.oneShot ? 0 : S, R, hold, e };
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
      for (let k = 1; k <= 24; k++) {
        const p = k / 24;
        g.lineTo(x0 + (x1 - x0) * p, y0 + (y1 - y0) * f(p));
      }
    };
    seg(X(0), Y(0), X(q.A), Y(1), q.e.attackCurve || 'linear');
    seg(X(q.A), Y(1), X(q.A + q.D), Y(q.S), q.e.decayCurve || 'exp');
    if (!patch.oneShot) {
      g.lineTo(X(q.A + q.D + q.hold), Y(q.S));
      seg(X(q.A + q.D + q.hold), Y(q.S), X(q.A + q.D + q.hold + q.R), Y(0),
          q.e.releaseCurve || 'exp');
    }
    g.stroke();
  });
  g.fillStyle = 'rgba(255,255,255,0.25)';
  g.font = `${9 * dpr}px ui-monospace`;
  g.fillText('amp envelopes · ' + Math.round(T) + 'ms', pad + 2, 10 * dpr);
}

// ---------------- filter response curve ----------------
function drawFilterResp() {
  const c = $('filter-resp');
  const dpr = window.devicePixelRatio || 1;
  const w = c.clientWidth * dpr, h = c.clientHeight * dpr;
  if (c.width !== w || c.height !== h) { c.width = w; c.height = h; }
  const g = c.getContext('2d');
  g.fillStyle = '#101318';
  g.fillRect(0, 0, w, h);
  const f = layer().filter;
  const mode = f?.mode || 'bypass';
  // 12 kHz engine cap: shade the forbidden zone.
  const fMin = 20, fMax = 24000;
  const xOf = (hz) => (Math.log(hz / fMin) / Math.log(fMax / fMin)) * w;
  g.fillStyle = 'rgba(224,112,112,0.08)';
  g.fillRect(xOf(12000), 0, w - xOf(12000), h);
  g.strokeStyle = 'rgba(255,255,255,0.07)';
  for (const hz of [100, 1000, 10000]) {
    g.beginPath(); g.moveTo(xOf(hz), 0); g.lineTo(xOf(hz), h); g.stroke();
  }
  if (mode === 'bypass') {
    g.fillStyle = 'rgba(255,255,255,0.3)';
    g.font = `${10 * dpr}px ui-monospace`;
    g.fillText('filter bypassed', 8 * dpr, 14 * dpr);
    return;
  }
  const fc = Math.min(f?.cutoffHz ?? 1200, 12000);
  const res = Math.min(f?.resonance ?? 0, 0.95);
  const Q = 1 / (2 - 2 * res);
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
    else mag = (wr / Q) / den;
    const db = Math.max(dbBot, 20 * Math.log10(mag + 1e-9));
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
  g.fillText(`${mode} ${fc}Hz Q=${Q.toFixed(2)}`, 8 * dpr, 11 * dpr);
}

// ---------------- full sync ----------------
function syncUI() {
  renderLayerTabs();
  for (const el of bound) el._sync();
  $('l-gain-val').textContent = (layer().mixGain ?? 1).toFixed(2);
  $('l-pan-val').textContent = panLabel(layer().pan ?? 0);
  $('f-cut-val').textContent = (layer().filter?.cutoffHz ?? 1200) + ' Hz';
  $('f-res-val').textContent = (layer().filter?.resonance ?? 0).toFixed(2);
  $('f-key-val').textContent = (layer().keyTrackFilter ?? 0).toFixed(2);
  syncEnvControls();
  syncOscVisibility();
  renderLfos();
  renderMods();
  drawLayers();
  drawFilterResp();
  updateModeUI();
  if ($('json-drawer').classList.contains('open'))
    $('editor').value = JSON.stringify(patch, null, 2);
}

// ---------------- JSON drawer / export / import ----------------
$('btn-json').onclick = () => {
  const d = $('json-drawer');
  d.classList.toggle('open');
  $('btn-json').textContent = d.classList.contains('open') ? 'JSON ▴' : 'JSON ▾';
  if (d.classList.contains('open')) $('editor').value = JSON.stringify(patch, null, 2);
};
$('btn-json-apply').onclick = () => {
  try {
    patch = JSON.parse($('editor').value);
  } catch (err) {
    log('editor JSON invalid: ' + err.message, 'err');
    return;
  }
  layerIdx = 0;
  syncUI();
  applyNow();
};
function download(name, text) {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([text], { type: 'application/json' }));
  a.download = name;
  a.click();
}
$('btn-undo').onclick = doUndo;
function doUndo() {
  // Pop entries for the current target until one differs from the present.
  while (undoStack.length) {
    const u = undoStack.pop();
    if (u.mode !== $('edit-mode').value || u.key !== targetKey()) continue;
    const prev = JSON.parse(u.json);
    if (JSON.stringify(prev) === JSON.stringify(patch)) continue;
    patch = prev ?? defaultPatch();
    layerIdx = 0;
    syncUI();
    // Re-apply without pushing this state as a new undo entry.
    const clean = structuredClone(patch);
    if (!isKit()) {
      if (targetKey() === 'fallback') {
        bankDoc.fallback = clean;
        port.postMessage({ type: 'loadBank', json: JSON.stringify(bankDoc) });
      } else {
        bankDoc.programs[targetKey()] = clean;
        port.postMessage({ type: 'loadProgram', prog: +targetKey(), json: JSON.stringify(clean) });
      }
    } else {
      kitDoc.notes[targetKey()] = clean;
      port.postMessage({ type: 'loadDrumNote', note: +targetKey(), json: JSON.stringify(clean) });
    }
    setApplyState('↶ undone', 'ok');
    return;
  }
  setApplyState('nothing to undo', 'warn');
}
document.addEventListener('keydown', (e) => {
  if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === 'z' &&
      !['TEXTAREA', 'INPUT'].includes(e.target.tagName)) {
    e.preventDefault();
    doUndo();
  }
});

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
  patch.name = isKit() ? (DRUM_NAMES[targetKey()] || 'drum').toLowerCase().replace(/\s+/g, '_')
                       : 'init';
  if (isKit()) patch.oneShot = true;
  layerIdx = 0;
  syncUI();
  applyNow();
};

function currentFxJson() {
  const get = (sel) => +document.querySelector(`[data-fx="${sel}"]`).value / 1000;
  return JSON.stringify({
    version: 1,
    reverb: { roomSize: get('reverb:0'), damping: get('reverb:1'),
              width: get('reverb:2'), wetGain: get('reverb:3') },
    chorus: { rateHz: get('chorus:0'), depthMs: get('chorus:1'),
              wetGain: get('chorus:2') },
  });
}
$('btn-export-track').onclick = () => {
  if (!lastSongBytes) { log('bake: load a song (.mid) first', 'err'); return; }
  if (lastSongIsTrack) { log('bake: already a baked track — load the source .mid to rebake', 'err'); return; }
  applyNow();  // fold pending edits into the docs
  const buf = lastSongBytes.slice(0);
  port.postMessage({
    type: 'bakeTrack',
    smfBytes: buf,
    bankJson: JSON.stringify(bankDoc),
    kitJson: JSON.stringify(kitDoc),
    fxJson: currentFxJson(),
    name: lastSongName,
    verifySeconds: 0,
  }, [buf]);
  log('baking ' + lastSongName + '.mstk …');
};

$('btn-export-patch').onclick = () => {
  const key = targetKey();
  const base = (patch.name || (isKit() ? 'drum_' + key : 'program_' + key)).replace(/\W+/g, '_');
  download(base + '.json', JSON.stringify(patch, null, 2));
};
$('btn-export-doc').onclick = () => {
  applyNow();  // make sure the doc holds the latest edits
  if (isKit()) download('gm_kit.json', JSON.stringify(kitDoc, null, 2));
  else download('gm_bank.json', JSON.stringify(bankDoc, null, 2));
};
$('import-file').onchange = async (e) => {
  const f = e.target.files[0];
  if (!f) return;
  let doc;
  try {
    doc = JSON.parse(await f.text());
  } catch (err) {
    log('import failed: ' + err.message, 'err');
    return;
  }
  if (doc.programs || doc.fallback) {
    bankDoc = doc;
    port.postMessage({ type: 'loadBank', json: JSON.stringify(bankDoc) });
    $('edit-mode').value = 'bank';
    log('bank imported: ' + f.name, 'ok');
  } else if (doc.notes) {
    kitDoc = doc;
    port.postMessage({ type: 'loadDrums', json: JSON.stringify(kitDoc) });
    $('edit-mode').value = 'kit';
    log('kit imported: ' + f.name, 'ok');
  } else {
    log('import: not a bank or kit file', 'err');
    return;
  }
  refreshTargets();
  updateModeUI();
};

// ---------------- audition ----------------
const auditionCh = () => (isKit() ? 9 : 0);
function sendNoteOn(note, vel) {
  port.postMessage({ type: 'noteOn', ch: auditionCh(), note, vel: vel ?? +$('aud-vel').value });
}
function sendNoteOff(note) {
  port.postMessage({ type: 'noteOff', ch: auditionCh(), note });
}
$('btn-hit').onclick = () => {
  const note = +targetKey();
  sendNoteOn(note);
  setTimeout(() => sendNoteOff(note), 150);
};
$('aud-sus').onchange = (e) =>
  port.postMessage({ type: 'cc', ch: auditionCh(), num: 64, val: e.target.checked ? 127 : 0 });
$('aud-mod').oninput = (e) =>
  port.postMessage({ type: 'cc', ch: auditionCh(), num: 1, val: +e.target.value });
$('aud-bend').oninput = (e) =>
  port.postMessage({ type: 'bend', ch: auditionCh(), value: +e.target.value });
$('aud-bend').onpointerup = (e) => { e.target.value = 0; e.target.oninput(e); };

let padNotes = [];  // kit mode: mapped drum notes, in pad order
function buildKeyboard() {
  if (isKitSafe()) { buildPads(); return; }
  const kbd = $('kbd');
  kbd.classList.remove('pads');
  $('aud-oct').parentElement && ($('aud-oct').style.display = '');
  const hintEl = $('aud-hint'); if (hintEl) hintEl.textContent = 'A–K white · W/E/T/Y/U black · Z/X octave · keys audition the edited patch';
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
  const hintEl = $('aud-hint'); if (hintEl) hintEl.textContent = 'click a pad to hit AND select that drum · A–L keys hit the first pads';
  kbd.innerHTML = '';
  padNotes = Object.keys(kitDoc.notes).map(Number).sort((a, b) => a - b);
  const sel = +targetKey();
  if (!padNotes.includes(sel)) padNotes.push(sel);   // editing an unmapped note: give it a pad
  padNotes.sort((a, b) => a - b);
  for (const n of padNotes) {
    const pad = document.createElement('div');
    pad.className = 'pad' + (n === sel ? ' sel' : '');
    pad.dataset.note = n;
    const name = kitDoc.notes[String(n)]?.name || DRUM_NAMES[n] || 'drum';
    pad.innerHTML = `${n}<small>${name}</small>`;
    pad.onpointerdown = (e) => {
      e.preventDefault();
      hitPad(n);
      if (String(n) !== targetKey()) {   // click also selects for editing
        $('edit-target').value = String(n);
        selectTarget();
      }
    };
    kbd.appendChild(pad);
  }
}
function hitPad(note) {
  sendNoteOn(note);
  setTimeout(() => sendNoteOff(note), 120);
  const pad = document.querySelector(`.pad[data-note="${note}"]`);
  if (pad) {
    pad.classList.add('on');
    setTimeout(() => pad.classList.remove('on'), 160);
  }
}
buildKeyboard();
$('aud-oct').onchange = buildKeyboard;

const KEYMAP = { a: 0, w: 1, s: 2, e: 3, d: 4, f: 5, t: 6, g: 7, y: 8, h: 9, u: 10, j: 11, k: 12, o: 13, l: 14 };
const keysDown = new Set();
document.addEventListener('keydown', (e) => {
  if (['TEXTAREA', 'INPUT', 'SELECT'].includes(e.target.tagName) || e.repeat || e.metaKey || e.ctrlKey) return;
  const k = e.key.toLowerCase();
  if (k === 'z') { $('aud-oct').value = Math.max(2, +$('aud-oct').value - 1); buildKeyboard(); return; }
  if (k === 'x') { $('aud-oct').value = Math.min(6, +$('aud-oct').value + 1); buildKeyboard(); return; }
  if (k in KEYMAP && !keysDown.has(k)) {
    keysDown.add(k);
    if (isKitSafe()) {
      const n = padNotes[KEYMAP[k]];
      if (n !== undefined) hitPad(n);
      return;
    }
    const note = (+$('aud-oct').value + 1) * 12 + KEYMAP[k];
    heldNotes.add(note);
    sendNoteOn(note);
  }
});
document.addEventListener('keyup', (e) => {
  const k = e.key.toLowerCase();
  if (k in KEYMAP && keysDown.has(k)) {
    keysDown.delete(k);
    if (isKitSafe()) return;  // pads are one-shot hits
    const note = (+$('aud-oct').value + 1) * 12 + KEYMAP[k];
    heldNotes.delete(note);
    sendNoteOff(note);
  }
});

// ---------------- Web MIDI ----------------
async function setupMidi() {
  if (!navigator.requestMIDIAccess) {
    $('midi-state').textContent = 'MIDI: unsupported';
    return;
  }
  try {
    const access = await navigator.requestMIDIAccess();
    const attach = () => {
      const names = [];
      for (const input of access.inputs.values()) {
        names.push(input.name);
        input.onmidimessage = (e) => onMidiMessage(e.data);
      }
      $('midi-state').textContent = names.length ? 'MIDI: ' + names.join(', ') : 'MIDI: no devices';
    };
    access.onstatechange = attach;
    attach();
  } catch {
    $('midi-state').textContent = 'MIDI: permission denied';
  }
}
function onMidiMessage(d) {
  const kind = d[0] & 0xF0;
  const ch = auditionCh();
  switch (kind) {
    case 0x90:
      if (d[2] > 0) { heldNotes.add(d[1]); sendNoteOn(d[1], d[2]); }
      else { heldNotes.delete(d[1]); sendNoteOff(d[1]); }
      break;
    case 0x80: heldNotes.delete(d[1]); sendNoteOff(d[1]); break;
    case 0xB0: port.postMessage({ type: 'cc', ch, num: d[1], val: d[2] }); break;
    case 0xE0: port.postMessage({ type: 'bend', ch, value: ((d[2] << 7) | d[1]) - 8192 }); break;
    case 0xD0: port.postMessage({ type: 'aftertouch', ch, pressure: d[1] }); break;
    default: break;
  }
}
