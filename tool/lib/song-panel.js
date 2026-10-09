//
// Song panel: transport, seek, tempo/volume, channel grid (mute), song /
// track loading (.mid or .mstk), "instruments in this song" scan, song-level
// send FX sliders, offline WAV render (worker) and .mstk bake.
//
//   const song = initSongPanel(root, client, store, { log, editor });
//   song.loadSongBytes(arrayBuffer, 'name.mid');
//
import { GM_NAMES, DRUM_NAMES } from './gm.js';
import { scoped, fmtTime, download, isMstk } from './ui.js';

// Minimal SMF walk (running status aware): which programs each channel uses
// and which drum notes channel 10 hits. Mirrors tools/scan_programs.py.
export function scanInstruments(d) {
  const programs = new Map();
  const drums = new Set();
  const channelsWithNotes = new Set();
  const chProg = new Array(16).fill(0);
  const noteProg = (prog, ch) => {
    if (!programs.has(prog)) programs.set(prog, new Set());
    programs.get(prog).add(ch);
  };
  if (d.length < 14 || d[0] !== 0x4D || d[1] !== 0x54) return null;
  let pos = 8 + ((d[4] << 24) | (d[5] << 16) | (d[6] << 8) | d[7]);
  const vlq = (p) => {
    let v = 0;
    for (let i = 0; i < 4; i++) { const b = d[p++]; v = (v << 7) | (b & 0x7F); if (!(b & 0x80)) break; }
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
      if (st < 0x80) break;
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
  return { programs, drums, channelsWithNotes };
}

export function initSongPanel(root, client, store, opts = {}) {
  const $ = scoped(root);
  const log = opts.log || ((m) => console.log(m));
  const editor = opts.editor || null;
  const has = (id) => !!$(id);

  let status = { durationMs: 0 };
  let seekDragging = false;
  const mutedChannels = new Set();
  let lastSongBytes = null;
  let lastSongIsTrack = false;
  let lastSongName = 'song';
  let songInstruments = null;
  let renderWorker = null;

  // ---------------- transport ----------------
  if (has('btn-play')) $('btn-play').onclick = () => client.play();
  if (has('btn-pause')) $('btn-pause').onclick = () => client.pause();
  if (has('btn-stop')) $('btn-stop').onclick = () => client.stop();
  if (has('chk-loop')) $('chk-loop').onchange = (e) => client.setLoop(e.target.checked);
  if (has('chk-dither')) $('chk-dither').onchange = (e) => client.dither(e.target.checked);
  if (has('seek')) {
    $('seek').onpointerdown = () => { seekDragging = true; };
    $('seek').onchange = (e) => {
      seekDragging = false;
      if (status.durationMs > 0) client.seekMs((e.target.value / 1000) * status.durationMs);
    };
  }
  if (has('tempo')) $('tempo').oninput = (e) => {
    if (has('tempo-val')) $('tempo-val').textContent = (e.target.value / 1000).toFixed(2) + '×';
    client.tempoScale(+e.target.value);
  };
  if (has('volume')) $('volume').oninput = (e) => client.masterVolume(+e.target.value);

  if (has('channels')) {
    for (let i = 0; i < 16; i++) {
      const el = document.createElement('div');
      el.className = 'ch';
      el.innerHTML = `${i + 1}${i === 9 ? 'd' : ''}<span class="dot"></span>`;
      el.title = 'click: mute/unmute (300ms fade)';
      el.onclick = () => {
        const muted = !mutedChannels.has(i);
        if (muted) mutedChannels.add(i); else mutedChannels.delete(i);
        el.classList.toggle('muted', muted);
        client.channelMute(i, muted, 300);
      };
      $('channels').appendChild(el);
    }
  }

  function updateStatus(s) {
    status = s;
    const set = (id, v) => { const el = $(id); if (el) el.textContent = v; };
    set('m-voices', s.activeVoices);
    set('m-peak', s.peakVoices);
    set('m-steals', s.steals);
    set('m-drops', s.drops);
    set('m-bar', s.bar + 1);
    set('m-beat', s.beat + 1);
    set('t-cur', fmtTime(s.timeMs));
    set('t-dur', fmtTime(s.durationMs));
    if (has('seek') && !seekDragging && s.durationMs > 0)
      $('seek').value = Math.round((s.timeMs / s.durationMs) * 1000);
    if (has('btn-play')) $('btn-play').classList.toggle('active', s.playing);
    root.querySelectorAll('#channels .ch').forEach((el, i) => {
      el.classList.toggle('lit', s.channelNotes[i] > 0);
      if (s.channelPrograms)
        el.title = (i === 9 ? 'drums' : `${s.channelPrograms[i]} ${GM_NAMES[s.channelPrograms[i]]}`) +
                   ' — click: mute/unmute (300ms fade)';
    });
    const drumSet = new Set(String(s.drumNotes || '').split(',').filter(Boolean).map(Number));
    root.querySelectorAll('#instruments .inst').forEach((el) => {
      let lit;
      if (el.dataset.note !== undefined) lit = drumSet.has(+el.dataset.note);
      else {
        const chs = el.dataset.channels.split(',').map(Number);
        const prog = +el.dataset.prog;
        lit = chs.some((c) => s.channelNotes[c] > 0 && (!s.channelPrograms || s.channelPrograms[c] === prog));
      }
      el.classList.toggle('lit', lit);
    });
  }
  client.addEventListener('status', (e) => updateStatus(e.detail));

  // ---------------- song loading ----------------
  async function loadSongBytes(buf, name) {
    lastSongBytes = buf.slice(0);
    if (name) lastSongName = name.replace(/\.(midi?|mstk)$/i, '');
    const u8 = new Uint8Array(lastSongBytes);
    lastSongIsTrack = isMstk(u8);
    let p;
    if (lastSongIsTrack) {
      songInstruments = null;
      renderInstruments();
      if (has('instruments')) {
        $('instruments').textContent = 'baked .mstk track — content is embedded and pre-shaken';
        $('instruments').className = 'hint';
      }
      p = client.loadTrack(u8);
    } else {
      songInstruments = scanInstruments(u8);
      renderInstruments();
      p = client.loadSong(u8);
    }
    client.setLoop(has('chk-loop') ? $('chk-loop').checked : false);
    client.play();
    try {
      const r = await p;
      for (const w of r.warnings || []) log(`[song] ${w}`, 'warn');
      log(`[song] ${lastSongName} loaded`, 'ok');
    } catch (err) {
      log('[song] ' + err.message, 'err');
    }
    opts.onSongLoaded?.({ bytes: lastSongBytes, name: lastSongName, isTrack: lastSongIsTrack });
  }

  function renderInstruments() {
    const box = $('instruments');
    if (!box) return;
    box.innerHTML = '';
    if (!songInstruments || (!songInstruments.programs.size && !songInstruments.drums.size)) {
      box.textContent = 'load a song to see its instruments';
      box.className = 'hint';
      return;
    }
    box.className = '';
    const rows = [...songInstruments.programs.entries()].sort((a, b) => a[0] - b[0]);
    for (const [prog, chs] of rows) {
      const authored = !!store.bankDoc?.programs?.[String(prog)];
      const el = document.createElement('div');
      el.className = 'inst';
      el.dataset.channels = [...chs].join(',');
      el.dataset.prog = prog;
      el.innerHTML = `<span class="now"></span><span class="prog">${prog}</span>
        <span>${GM_NAMES[prog]}</span>
        <span class="chs">ch ${[...chs].map((c) => c + 1).sort((a, b) => a - b).join(',')}</span>
        ${authored ? '' : '<span class="flag">fallback</span>'}`;
      el.title = 'click to edit this patch';
      el.onclick = () => editor?.selectProgram(prog);
      box.appendChild(el);
    }
    if (songInstruments.drums.size) {
      for (const n of [...songInstruments.drums].sort((a, b) => a - b)) {
        const mapped = !!store.kitDoc?.notes?.[String(n)];
        const el = document.createElement('div');
        el.className = 'inst';
        el.dataset.channels = '9';
        el.dataset.note = n;
        el.innerHTML = `<span class="now"></span><span class="prog">${n}</span>
          <span>${DRUM_NAMES[n] || 'drum ' + n}</span>
          <span class="chs">ch 10</span>
          ${mapped ? '' : '<span class="flag" style="color:var(--bad)">silent!</span>'}`;
        el.title = 'click to edit this drum';
        el.onclick = () => editor?.selectDrum(n);
        box.appendChild(el);
      }
    }
  }
  if (has('song-file')) $('song-file').onchange = async (e) => {
    const f = e.target.files[0];
    if (f) loadSongBytes(await f.arrayBuffer(), f.name);
  };
  root.querySelectorAll('[data-song]').forEach((btn) => {
    btn.onclick = async () =>
      loadSongBytes(await (await fetch(new URL('../../' + btn.dataset.song, import.meta.url))).arrayBuffer(),
                    btn.dataset.song.split('/').pop());
  });

  // ---------------- song FX ----------------
  root.querySelectorAll('[data-fx]').forEach((el) => {
    el.oninput = () => {
      const [fx, id] = el.dataset.fx.split(':');
      if (fx === 'reverb') client.reverbParam(+id, +el.value);
      else client.chorusParam(+id, +el.value);
      opts.onFxChanged?.(currentFx());
    };
  });
  function currentFx() {
    const get = (sel, def) => {
      const el = root.querySelector(`[data-fx="${sel}"]`);
      return el ? +el.value / 1000 : def;
    };
    return {
      version: 1,
      reverb: { roomSize: get('reverb:0', 0.8), damping: get('reverb:1', 0.4),
                width: get('reverb:2', 1), wetGain: get('reverb:3', 0.3) },
      chorus: { rateHz: get('chorus:0', 0.8), depthMs: get('chorus:1', 3.5), wetGain: get('chorus:2', 0.2) },
    };
  }
  function setFx(fx) {
    const set = (sel, v) => { const el = root.querySelector(`[data-fx="${sel}"]`); if (el && v !== undefined) el.value = Math.round(v * 1000); };
    set('reverb:0', fx?.reverb?.roomSize); set('reverb:1', fx?.reverb?.damping);
    set('reverb:2', fx?.reverb?.width); set('reverb:3', fx?.reverb?.wetGain);
    set('chorus:0', fx?.chorus?.rateHz); set('chorus:1', fx?.chorus?.depthMs); set('chorus:2', fx?.chorus?.wetGain);
    client.loadSongConfig(JSON.stringify(fx)).catch((err) => log('[fx] ' + err.message, 'err'));
  }

  // ---------------- offline render ----------------
  function renderWav({ seconds = 0, loop = false, fxJson = null, masterJson = null } = {}) {
    if (!lastSongBytes) { log('render: load a song first', 'err'); return; }
    if (renderWorker) { log('render already running', 'warn'); return; }
    editor?.applyNow();
    const btn = $('btn-render');
    if (btn) btn.disabled = true;
    const state = $('render-state'), wrap = $('render-bar-wrap'), bar = $('render-bar');
    if (state) state.textContent = 'rendering… 0%';
    if (wrap) wrap.style.display = 'block';
    if (bar) bar.style.width = '0%';
    renderWorker = new Worker(opts.renderWorkerUrl || new URL('../render-worker.js', import.meta.url), { type: 'module' });
    const finish = () => { renderWorker.terminate(); renderWorker = null; if (btn) btn.disabled = false; };
    renderWorker.onmessage = (e) => {
      const m = e.data;
      if (m.type === 'progress') {
        if (state) state.textContent = `rendering… ${m.pct}%`;
        if (bar) bar.style.width = m.pct + '%';
      } else if (m.type === 'reports') {
        for (const [what, r] of m.reports) for (const w of r.warnings || []) log(`[render:${what}] ${w}`, 'warn');
      } else if (m.type === 'done') {
        download(lastSongName + '.wav', m.wav, 'audio/wav');
        if (bar) bar.style.width = '100%';
        if (wrap) setTimeout(() => { wrap.style.display = 'none'; }, 800);
        if (state) state.textContent = `done: ${(m.frames / 48000).toFixed(1)}s · peak ${m.peakVoices} voices` +
                                       (m.drops > 0 ? ` · ${m.drops} drops!` : '');
        log(`render done (${(m.frames / 48000).toFixed(1)}s, peak ${m.peakVoices} voices, ${m.steals} steals)`, 'ok');
        finish();
      } else if (m.type === 'error') {
        log('render failed: ' + m.error, 'err');
        if (state) state.textContent = 'render failed';
        if (wrap) wrap.style.display = 'none';
        finish();
      }
    };
    const buf = lastSongBytes.slice(0);
    renderWorker.postMessage({
      bankJson: JSON.stringify(store.bankDoc),
      kitJson: JSON.stringify(store.kitDoc),
      songBytes: buf,
      seconds,
      dither: has('chk-dither') ? $('chk-dither').checked : false,
      loop,
      fxJson: fxJson ?? JSON.stringify(currentFx()),
      masterJson,
    }, [buf]);
  }
  if (has('btn-render')) $('btn-render').onclick = () =>
    renderWav({ seconds: +($('render-secs')?.value || 0) });

  // ---------------- bake ----------------
  async function bake({ fxJson = null, name = null, verifySeconds = 0 } = {}) {
    if (!lastSongBytes) { log('bake: load a song (.mid) first', 'err'); return null; }
    if (lastSongIsTrack) { log('bake: already a baked track — load the source .mid to rebake', 'err'); return null; }
    editor?.applyNow();
    const trackName = name || lastSongName;
    log('baking ' + trackName + '.mstk …');
    try {
      const bytes = await client.bakeTrack({
        smfBytes: lastSongBytes,
        bankJson: JSON.stringify(store.bankDoc),
        kitJson: JSON.stringify(store.kitDoc),
        fxJson: fxJson ?? JSON.stringify(currentFx()),
        name: trackName,
        verifySeconds,
      });
      return bytes;
    } catch (err) {
      log('[bake] ' + err.message, 'err');
      return null;
    }
  }
  if (has('btn-export-track')) $('btn-export-track').onclick = async () => {
    const bytes = await bake();
    if (!bytes) return;
    download(lastSongName + '.mstk', bytes);
    log(`baked track downloaded: ${lastSongName}.mstk (${(bytes.byteLength / 1024).toFixed(1)} KB, tree-shaken)`, 'ok');
  };

  return {
    loadSongBytes,
    renderWav,
    bake,
    currentFx,
    setFx,
    get lastSongBytes() { return lastSongBytes; },
    get lastSongName() { return lastSongName; },
    get lastSongIsTrack() { return lastSongIsTrack; },
    get status() { return status; },
  };
}
