//
// DigidreamSynth DAW shell: one engine, one page, tabs. Owns the project store,
// the playback pipeline (project -> SMF -> hot-swap keeping position), the
// transport, the File menu and tab switching; each tab is its own module.
//
import { SynthClient } from '../lib/synth-client.js';
import { makeLogger, logReport, fmtTime, bootStatus } from '../lib/ui.js';
import { createScope } from '../lib/scope.js';
import { initPatchEditor, PATCH_EDITOR_HTML } from '../lib/patch-editor.js';
import { initCutterPanel, CUTTER_HTML } from '../lib/cutter-panel.js';
import { initFitPanel, FIT_PANEL_HTML } from '../lib/fit-panel.js';
import { setupMidi } from '../lib/midi-input.js';
import * as desktop from '../lib/desktop.js';
import { GM_NAMES } from '../lib/gm.js';
import { ProjectStore, newProject, tickToMs, msToTick, barBeatAt, gridTicks, projectEndTick } from './project.js';
import { projectToSmf } from './smf.js';
import { initArrange } from './arrange.js';
import { initPianoRoll } from './pianoroll.js';
import { initRecorder } from './record.js';
import { initCues } from './cues.js';
import { initMaster } from './master.js';
import * as files from './export.js';

const $ = (id) => document.getElementById(id);
const log = makeLogger($('log'), 300);

// The desktop shell runs with autoplay allowed, so it boots straight into
// the DAW instead of asking for a click first.
if (desktop.isDesktop) queueMicrotask(() => $('start-btn').click());

$('start-btn').onclick = async () => {
  const setStage = bootStatus($('overlay'));
  let client;
  try {
    setStage('compiling the engine (WASM)…');
    client = await SynthClient.create({ analyser: true });
    await boot(client, setStage);
    $('overlay').remove();
  } catch (err) {
    setStage('failed to start: ' + err + ' — reload the page to retry', true);
    return;
  }
};

async function boot(client, setStage = () => {}) {
  client.addEventListener('fatal', (e) => log('worklet failed: ' + e.detail.error, 'err'));
  client.addEventListener('report', (e) => {
    const m = e.detail;
    if (['patch', 'song', 'track', 'replace', 'cut', 'bake', 'queue', 'master', 'fx', 'bank', 'drums'].includes(m.what) && m.report.ok) {
      for (const w of m.report.warnings || []) log(`[${m.what}] ${w}`, 'warn');
      return;
    }
    logReport(log, m.what, m.report, null);
  });

  // ---------------- content defaults ----------------
  setStage('loading instruments (bank + kit)…');
  const [bankText, kitText] = await Promise.all([
    fetch(new URL('../../content/patches/gm_bank.json', import.meta.url)).then((r) => r.text()),
    fetch(new URL('../../content/drums/gm_kit.json', import.meta.url)).then((r) => r.text()),
  ]);
  const defaultBank = JSON.parse(bankText), defaultKit = JSON.parse(kitText);

  const store = new ProjectStore(newProject());
  const editorStore = { bankDoc: null, kitDoc: null };
  const pieces = new Map();   // cue pieces: name -> {bytes, source, ms}
  let mode = 'project';       // 'project' | 'pieces' (Cut/Cues tabs drive the engine directly)

  // ---------------- ctx shared by the tabs ----------------
  const ctx = {
    store, client, log, pieces, editorStore,
    bankJson: (pretty = false) => JSON.stringify(store.p.bank || defaultBank, null, pretty ? 2 : 0),
    kitJson: (pretty = false) => JSON.stringify(store.p.kit || defaultKit, null, pretty ? 2 : 0),
    fxDoc: () => ({ ...store.p.fx, master: store.p.master }),
    enginePiecesMode: () => { mode = 'pieces'; },
    inPiecesMode: () => mode === 'pieces',
    loadProject: null, editor: null, cutter: null, cues: null,
    playback: null, ui: null,
    projectPath: null,
    setProjectPath: (path) => { ctx.projectPath = path || null; syncDocument(true); },
    removeSlice: (name) => store.mutate('remove slice', (pp) => { pp.slices = pp.slices.filter((s) => s.name !== name); }, { audio: false }),
  };

  // ---------------- background-task progress toast ----------------
  let taskCancel = null;
  ctx.progress = {
    begin(label, { cancel = null, indeterminate = false } = {}) {
      taskCancel = cancel;
      $('task-label').textContent = label;
      $('task-pct').textContent = '';
      $('task-bar').style.width = indeterminate ? '' : '0%';
      $('task-toast').classList.toggle('indet', indeterminate);
      $('task-cancel').style.display = cancel ? '' : 'none';
      $('task-toast').style.display = 'block';
    },
    update(pct) {
      $('task-bar').style.width = pct + '%';
      $('task-pct').textContent = pct + '%';
    },
    done() {
      taskCancel = null;
      $('task-toast').style.display = 'none';
    },
  };
  $('task-cancel').onclick = () => { taskCancel?.(); ctx.progress.done(); };

  // ---------------- playback pipeline ----------------
  let status = null, anchorPerf = performance.now(), anchorMs = 0;
  let loadedOffset = 0;      // ticks of count-in in the loaded song
  let reloadTimer = null;
  let loaded = false;
  const shiftedProject = (off) => (off ? { ...store.p, tempoMap: store.p.tempoMap.map((t) => ({ tick: t.tick ? t.tick + off : 0, bpm: t.bpm })) } : store.p);
  const playback = ctx.playback = {
    status: () => status,
    isPlaying: () => !!status?.playing && mode === 'project',
    msNow: () => (status ? status.timeMs + (status.playing ? performance.now() - anchorPerf : 0) : 0),
    msAt: (ts) => anchorMs + (ts - anchorPerf),
    positionTick() {
      if (!status || mode !== 'project') return 0;
      return Math.max(0, msToTick(shiftedProject(loadedOffset), this.msNow()) - loadedOffset);
    },
    seekTick(tick) {
      const sp = shiftedProject(loadedOffset);
      client.seekMs(tickToMs(sp, Math.max(0, tick) + loadedOffset));
    },
    get recOffsetTicks() { return loadedOffset; },
    async reload({ keepPosition = true, click = null, offsetTicks = 0 } = {}) {
      clearTimeout(reloadTimer);
      mode = 'project';
      const p = store.p;
      const useClick = click ?? $('chk-click').checked;
      const bytes = projectToSmf(p, { click: useClick, mutes: true, offsetTicks });
      const wasTick = loaded ? this.positionTick() : 0;
      const playing = !!status?.playing;
      loadedOffset = offsetTicks;
      try {
        if (loaded && playing && keepPosition && offsetTicks === 0) {
          await client.replaceSong(bytes);
        } else {
          await client.loadSong(bytes);
          if (keepPosition && wasTick > 0 && offsetTicks === 0) this.seekTick(wasTick);
          if (playing && keepPosition) client.play();
          // Loading resets the channels and the song's program changes are only
          // dispatched while it plays: re-assert every track's program so the
          // keyboard / MIDI audition hears the right instrument when stopped.
          assertPrograms();
        }
        client.setLoop(p.loop.enabled);
        loaded = true;
      } catch (err) {
        log('[song] ' + err.message, 'err');
      }
    },
    scheduleReload() {
      clearTimeout(reloadTimer);
      reloadTimer = setTimeout(() => this.reload({ keepPosition: true }), 150);
    },
    async play() {
      if (mode !== 'project' || !loaded) await this.reload({ keepPosition: false });
      client.play();
    },
    stop() { client.stop(); },
  };
  const assertPrograms = () => {
    for (const t of store.p.tracks) if (t.channel !== 9) client.program(t.channel, t.program);
  };
  const restoreContent = async () => {
    // The Cut/Cues tabs load .mstk files whose tree-shaken content replaced the
    // engine's bank/kit: put the project's content back.
    await Promise.all([client.loadBank(ctx.bankJson()), client.loadDrums(ctx.kitJson())]).catch(() => {});
    master.pushAll();
    await playback.reload({ keepPosition: false });
  };

  // ---------------- window chrome (desktop) ----------------
  // Title bar, the macOS edited dot and the proxy icon follow the project.
  let lastDoc = '';
  function syncDocument(force = false) {
    if (!desktop.isDesktop) return;
    const doc = { name: store.p.name, path: ctx.projectPath, dirty: !!store.dirty };
    const key = JSON.stringify(doc);
    if (!force && key === lastDoc) return;
    lastDoc = key;
    desktop.setDocument(doc);
  }

  // ---------------- transport UI ----------------
  const fmtPos = (tick) => {
    const bb = barBeatAt(store.p, Math.max(0, Math.floor(tick)));
    return `${bb.bar + 1}.${bb.beat + 1}.${String(bb.ticks).padStart(3, '0')}`;
  };
  client.addEventListener('status', (e) => {
    status = e.detail;
    anchorPerf = performance.now();
    anchorMs = status.timeMs;
    $('m-voices').textContent = status.activeVoices;
    $('btn-play').classList.toggle('active', status.playing);
    if (mode === 'project') {
      $('pos').textContent = fmtPos(playback.positionTick());
      $('t-cur').textContent = fmtTime(Math.max(0, playback.msNow() - tickToMs(shiftedProject(loadedOffset), loadedOffset)));
    } else {
      $('pos').textContent = `${status.bar + 1}.${status.beat + 1}`;
      $('t-cur').textContent = fmtTime(status.timeMs);
    }
    master.onStatus(status);
    cues.onStatus(status);
    syncDocument();
    // Synth tab: green dot on every track the engine is sounding right now.
    if (currentTab === 'synth') {
      document.querySelectorAll('#synth-instruments .inst').forEach((el) => {
        el.classList.toggle('snd', (status.channelNotes?.[+el.dataset.ch] || 0) > 0);
      });
    }
  });
  $('btn-play').onclick = () => (status?.playing ? client.pause() : playback.play());
  $('btn-stop').onclick = () => { if (recorder.recording) recorder.stop(); else playback.stop(); };
  $('btn-rec').onclick = () => recorder.toggle();
  $('volume').oninput = (e) => client.masterVolume(+e.target.value);
  $('chk-loop').onchange = (e) => store.mutate('loop', (pp) => { pp.loop.enabled = e.target.checked; });
  $('chk-click').onchange = () => playback.scheduleReload();
  const grooveChange = () => store.mutate('groove', (pp) => {
    pp.groove = { amount: Math.max(0, Math.min(100, +$('swing-amt').value || 0)) / 100,
                  grid: $('swing-grid').value };
  });
  $('swing-amt').onchange = grooveChange;
  $('swing-grid').onchange = grooveChange;
  $('bpm').onchange = (e) => store.mutate('tempo', (pp) => { pp.tempoMap = [{ tick: 0, bpm: Math.max(20, Math.min(400, +e.target.value || 120)) }]; });
  const sigChange = () => store.mutate('time signature', (pp) => { pp.timeSigs = [{ tick: 0, num: +$('sig-num').value || 4, den: +$('sig-den').value || 4 }]; });
  $('sig-num').onchange = sigChange;
  $('sig-den').onchange = sigChange;
  $('proj-name').onchange = (e) => { store.p.name = e.target.value; store.dirty = true; };

  // Phone layout: header and log start collapsed to one line each and expand
  // on tap. Desktop ignores both classes -- the CSS only acts under 700px --
  // so the same markup serves both without a second code path.
  const PHONE = window.matchMedia('(max-width: 700px), (max-height: 520px)');
  const setCollapsed = (el, on) => el.classList.toggle('collapsed', on);
  const applyPhoneDefaults = () => {
    setCollapsed(document.querySelector('header'), PHONE.matches && store.p.view.hdrOpen !== true);
    setCollapsed($('log-wrap'), PHONE.matches && store.p.view.logOpen !== true);
  };
  $('hdr-toggle').onclick = () => {
    const h = document.querySelector('header');
    const open = h.classList.contains('collapsed');
    setCollapsed(h, !open);
    store.p.view.hdrOpen = open;
  };
  $('log-toggle').onclick = () => {
    const w = $('log-wrap');
    const open = w.classList.contains('collapsed');
    setCollapsed(w, !open);
    store.p.view.logOpen = open;
    $('log').scrollTop = $('log').scrollHeight;
  };
  PHONE.addEventListener('change', applyPhoneDefaults);
  applyPhoneDefaults();

  // ---------------- UI helpers ----------------
  let selectedChannel = store.p.tracks[0].channel;
  let currentTab = 'arrange';
  const ui = ctx.ui = {
    get selectedChannel() { return selectedChannel; },
    snapTicks: () => ($('snap').value === 'off' ? 0 : gridTicks(store.p, $('snap').value)),
    selectTrack(ch) {
      selectedChannel = ch;
      arrange.refreshTracks();
      roll.setTrack(ch);
      refreshSynthList();
    },
    editTrackInstrument(ch) {
      const t = store.p.tracks.find((x) => x.channel === ch);
      if (!t) return;
      ui.showTab('synth');
      syncEditorToTrack(true);
    },
    async showTab(name) {
      if (name === currentTab) return;
      const prev = currentTab;
      currentTab = name;
      document.querySelectorAll('#tabs button').forEach((b) => b.classList.toggle('active', b.dataset.tab === name));
      document.querySelectorAll('.tab').forEach((t) => t.classList.toggle('active', t.id === 'tab-' + name));
      // Leaving a tab.
      if (prev === 'synth') editor.deactivate();
      if (prev === 'track') roll.deactivate();
      if (prev === 'arrange') arrange.stop();
      if (prev === 'cut') cutter.stop();
      if (prev === 'fit') fitPanel.deactivate();
      if ((prev === 'cut' || prev === 'cues') && mode === 'pieces' && name !== 'cut' && name !== 'cues') {
        client.stop();
        await restoreContent();
      }
      // Entering.
      if (name === 'synth') { editor.activate(); syncEditorToTrack(false); refreshSynthList(); }
      if (name === 'track') { roll.refreshTrackList(); roll.setTrack(selectedChannel); roll.activate(); }
      if (name === 'arrange') { arrange.refreshTracks(); arrange.start(); }
      if (name === 'cut') cutter.start();
      if (name === 'fit') fitPanel.activate();
      if (name === 'cues') cues.refresh();
      if (name === 'master') master.syncUI();
      refreshHistoryUI();     // the Synth tab's history is not the project's
    },
  };
  document.querySelectorAll('#tabs button').forEach((b) => { b.onclick = () => ui.showTab(b.dataset.tab); });

  // ---------------- tabs ----------------
  createScope($('scope'), client.analyser).start();
  const arrange = initArrange($('tab-arrange'), ctx);
  const roll = initPianoRoll($('tab-track'), ctx);
  const master = initMaster($('tab-master'), ctx);

  $('patch-editor-host').innerHTML = PATCH_EDITOR_HTML;
  const editor = ctx.editor = initPatchEditor($('tab-synth'), client, editorStore, {
    log,
    getAuditionChannel: () => (editor?.mode === 'kit' ? 9 : (selectedChannel === 9 ? 0 : selectedChannel)),
    onChange: () => { store.dirty = true; },
    onSelect: ({ mode: m, key }) => {
      // Picking a program in the editor makes the selected track play it, so
      // what you edit is what you hear.
      if (m === 'bank' && key !== 'fallback' && currentTab === 'synth') {
        const t = store.p.tracks.find((x) => x.channel === selectedChannel);
        if (t && t.channel !== 9 && t.program !== +key) {
          store.mutate('program', (pp) => { pp.tracks.find((x) => x.channel === selectedChannel).program = +key; });
          client.program(t.channel, +key);
        }
      }
      if (currentTab === 'synth') refreshSynthList();
    },
  });
  // The editor edits what the selected track plays: a noise layer you just
  // dialled in must be the one you hear in the song.
  function syncEditorToTrack(force) {
    const t = store.p.tracks.find((x) => x.channel === selectedChannel);
    if (!t) return;
    if (t.channel === 9) { if (force || editor.mode !== 'kit') editor.selectDrum(36); }
    else if (force || editor.mode !== 'bank' || +editor.key !== t.program) editor.selectProgram(t.program);
  }
  function refreshSynthList() {
    const box = $('synth-instruments');
    box.innerHTML = '';
    // What is being edited, and who plays it.
    const mode = editor.mode, key = editor.key;
    const players = store.p.tracks.filter((x) => (mode === 'kit' ? x.channel === 9 : x.channel !== 9 && String(x.program) === key));
    const what = mode === 'kit' ? `drum note ${key}` : key === 'fallback' ? 'the fallback patch' : `program ${key} ${GM_NAMES[+key] || ''}`;
    const hint = $('synth-hint');
    if (players.length) {
      hint.textContent = `editing ${what} — played by ${players.map((x) => `${x.name} (ch ${x.channel + 1})`).join(', ')}; edits apply live to the song`;
      hint.style.color = '';
    } else {
      hint.textContent = `editing ${what} — no track plays it, so you will not hear this edit in the song (the keyboard auditions it on ch ${(editor.mode === 'kit' ? 9 : selectedChannel) + 1}); click a track below to edit its instrument`;
      hint.style.color = 'var(--warn)';
    }
    for (const t of store.p.tracks) {
      const el = document.createElement('div');
      const editing = mode === 'kit' ? t.channel === 9 : (t.channel !== 9 && String(t.program) === key);
      el.className = 'inst' + (editing ? ' lit' : '');
      el.dataset.ch = t.channel;
      el.innerHTML = `<span class="now"></span><span class="prog">${t.channel === 9 ? 'kit' : t.program}</span>
        <span>${t.name}</span><span class="chs">ch ${t.channel + 1}</span>
        <span class="flag">${t.channel === 9 ? 'drum kit' : GM_NAMES[t.program]}</span>`;
      el.onclick = () => { selectedChannel = t.channel; syncEditorToTrack(true); refreshSynthList(); };
      box.appendChild(el);
    }
  }

  $('fit-host').innerHTML = FIT_PANEL_HTML;
  const fitPanel = initFitPanel($('tab-fit'), ctx);
  $('cutter-host').innerHTML = CUTTER_HTML;
  const cutter = ctx.cutter = initCutterPanel($('tab-cut'), client, {
    log, autoStart: false,
    onSlicesChanged: (slices) => {
      store.mutate('slices', (pp) => { pp.slices = slices.map((s) => ({ name: s.name, startTick: s.startTick, endTick: s.endTick })); }, { audio: false });
      for (const [name, pc] of pieces) if (pc.source === 'slice' && !slices.some((s) => s.name === name)) pieces.delete(name);
      for (const s of slices) pieces.set(s.name, { bytes: s.bytes, source: 'slice', ms: s.ms });
      cues.refresh();
    },
  });
  $('cut-bake').onclick = async () => {
    const bytes = await files.bakeProject(ctx);
    if (!bytes) return;
    mode = 'pieces';
    await cutter.loadTrackBytes(bytes, store.p.name);
    // Re-cut slices the project remembers (from a saved session).
    for (const s of store.p.slices) if (!cutter.slices.some((x) => x.name === s.name)) await cutter.addSlice({ startTick: s.startTick, endTick: s.endTick }, s.name);
  };
  $('cut-import-btn').onclick = () => $('cut-import').click();
  $('cut-import').onchange = async (e) => {
    const f = e.target.files[0];
    if (!f) return;
    mode = 'pieces';
    await cutter.loadTrackBytes(await f.arrayBuffer(), f.name);
  };

  const cues = ctx.cues = initCues($('tab-cues'), ctx);
  $('cue-add-project').onclick = async () => {
    const bytes = await files.bakeProject(ctx);
    if (!bytes) return;
    pieces.set(store.p.name || 'project', { bytes, source: 'project', ms: tickToMs(store.p, projectEndTick(store.p)) });
    cues.refresh();
  };
  $('cue-import-btn').onclick = () => $('cue-import').click();
  $('cue-import').onchange = async (e) => {
    for (const f of e.target.files) pieces.set(f.name.replace(/\.mstk$/i, ''), { bytes: await f.arrayBuffer(), source: 'import', ms: 0 });
    cues.refresh();
  };

  // ---------------- MIDI ----------------
  // Declared before setupMidi: a successful requestMIDIAccess() calls
  // onDevicesChanged straight away, and `recorder?.` would still be a TDZ
  // error while the const below is uninitialised. (A browser that denies or
  // lacks Web MIDI returns early and hides this; the desktop build grants it.)
  let recorder = null;
  const midi = await setupMidi({
    statusEl: $('midi-state'),
    onMessage: (d, ts) => {
      if (currentTab === 'synth') editor.midiMessage(d);
      else recorder.midiMessage(d, ts);
    },
    onDevicesChanged: () => recorder?.refreshDevices(),
  });
  recorder = initRecorder($('tab-track'), ctx, midi);

  // ---------------- project loading / store events ----------------
  ctx.loadProject = async (p, { keepContent = false } = {}) => {
    if (keepContent) { p.bank = store.p.bank; p.kit = store.p.kit; }
    store.replace(p);
  };
  const syncContent = async (pushToEngine) => {
    const p = store.p;
    if (!p.bank) p.bank = structuredClone(defaultBank);
    if (!p.kit) p.kit = structuredClone(defaultKit);
    editorStore.bankDoc = p.bank;
    editorStore.kitDoc = p.kit;
    if (pushToEngine) {
      await Promise.all([client.loadBank(ctx.bankJson()), client.loadDrums(ctx.kitJson())]).catch((err) => log(err.message, 'err'));
      editor.refreshTargets();
    }
  };
  store.addEventListener('change', async (e) => {
    const d = e.detail;
    const p = store.p;
    // The host hears of an edit at once (its edited dot, the studio's
    // "changed" and drafts): not only from the audio status loop, which never
    // ticks before audio has started (no click yet, a muted or headless page).
    syncDocument();
    // Header widgets.
    $('proj-name').value = p.name;
    $('bpm').value = p.tempoMap[0].bpm;
    $('sig-num').value = p.timeSigs[0].num;
    $('sig-den').value = p.timeSigs[0].den;
    $('chk-loop').checked = p.loop.enabled;
    $('swing-amt').value = Math.round((p.groove?.amount || 0) * 100);
    $('swing-grid').value = p.groove?.grid || '1/16';
    if (!p.tracks.some((t) => t.channel === selectedChannel)) selectedChannel = p.tracks[0]?.channel ?? 0;
    if (d.structural || d.loaded || d.content) await syncContent(true);
    else { editorStore.bankDoc = p.bank; editorStore.kitDoc = p.kit; }
    if (d.loaded || d.fx || d.structural) master.pushAll();
    if (d.loaded) {
      pieces.clear();
      cues.refresh();
      master.syncUI();
      await playback.reload({ keepPosition: false });
    } else if (d.audio) playback.scheduleReload();
    arrange.refreshTracks();
    roll.refreshTrackList();
    if (currentTab === 'synth') refreshSynthList();
    if (currentTab === 'cues') cues.refresh();
    refreshHistoryUI();
  });

  // ---------------- undo / redo ----------------
  // History belongs to whatever the tab is editing. The Synth tab edits a
  // patch, which keeps its own per-target history; every other tab edits the
  // project. Undo used to respect that and redo did not, so on the Synth tab
  // ⌘Z took back a patch edit and ⇧⌘Z put back an unrelated project change.
  const historyOwner = () => (currentTab === 'synth' ? editor : store);
  const doUndo = () => { if (historyOwner().undo()) refreshHistoryUI(); };
  const doRedo = () => { if (historyOwner().redo()) refreshHistoryUI(); };

  function refreshHistoryUI() {
    const h = historyOwner();
    const canU = h.canUndo(), canR = h.canRedo();
    const inSynth = currentTab === 'synth';
    const past = inSynth ? [] : store.history().past;
    const future = inSynth ? [] : store.history().future;
    const last = past[past.length - 1], next = future[future.length - 1];
    $('btn-undo').disabled = !canU;
    $('btn-redo').disabled = !canR;
    $('btn-undo').title = canU ? `undo ${inSynth ? 'patch edit' : last} (⌘Z)` : 'nothing to undo';
    $('btn-redo').title = canR ? `redo ${inSynth ? 'patch edit' : next} (⇧⌘Z)` : 'nothing to redo';
    const mu = $('mi-undo'), mr = $('mi-redo');
    mu.disabled = !canU;
    mr.disabled = !canR;
    mu.textContent = canU ? `Undo ${inSynth ? 'patch edit' : last}` : 'Undo';
    mr.textContent = canR ? `Redo ${inSynth ? 'patch edit' : next}` : 'Redo';
    // The list runs oldest first; clicking an entry steps to just after it.
    const list = $('hist-list');
    list.innerHTML = '';
    if (inSynth) {
      list.innerHTML = '<div class="empty">the Synth tab keeps its own history, per patch</div>';
      return;
    }
    if (!past.length && !future.length) {
      list.innerHTML = '<div class="empty">nothing yet</div>';
      return;
    }
    past.forEach((label, i) => {
      const b = document.createElement('button');
      b.textContent = `${i + 1}. ${label}`;
      b.onclick = () => { store.gotoHistory(i); refreshHistoryUI(); };
      list.appendChild(b);
    });
    const now = document.createElement('button');
    now.textContent = `${past.length + 1}. ← now`;
    now.onclick = () => { store.gotoHistory(past.length); refreshHistoryUI(); };
    list.appendChild(now);
    // Redo entries are stored next-first; show them in the order they happened.
    [...future].reverse().forEach((label, i) => {
      const b = document.createElement('button');
      b.className = 'future';
      b.textContent = `${past.length + 2 + i}. ${label}`;
      b.onclick = () => { store.gotoHistory(past.length + 1 + i); refreshHistoryUI(); };
      list.appendChild(b);
    });
  }
  $('btn-undo').onclick = doUndo;
  $('btn-redo').onclick = doRedo;

  // ---------------- commands ----------------
  // One table behind the in-page File menu, the desktop app's native menu
  // and the keyboard: every entry point runs the same action.
  async function runAction(act) {
    switch (act) {
      case 'new':
        if (!store.dirty || await desktop.confirmAction('Discard unsaved changes?',
              'The current project has edits that were never saved.', 'Discard')) {
          ctx.loadProject(newProject());
          ctx.setProjectPath(null);
        }
        break;
      case 'open': await files.openProject(ctx); break;
      case 'save': await files.saveProject(ctx); break;
      case 'save-as': await files.saveProject(ctx, { saveAs: true }); break;
      case 'import-song': await files.importSong(ctx); break;
      case 'import-bank': await files.importJson(ctx, 'bank'); break;
      case 'import-kit': await files.importJson(ctx, 'kit'); break;
      case 'import-fx': await files.importJson(ctx, 'fx'); break;
      case 'import-master': await files.importJson(ctx, 'master'); break;
      case 'export-mid': files.exportMid(ctx); break;
      case 'export-mstk': await files.exportMstk(ctx); break;
      case 'export-wav': files.renderWav(ctx); break;
      case 'export-slices': await files.exportSlices(ctx); break;
      case 'export-cues': files.exportJson(ctx, 'cues'); break;
      case 'export-bank': files.exportJson(ctx, 'bank'); break;
      case 'export-kit': files.exportJson(ctx, 'kit'); break;
      case 'export-fx': files.exportJson(ctx, 'fx'); break;
      case 'export-master': files.exportJson(ctx, 'master'); break;
      // Native-menu equivalents of the in-page controls.
      case 'undo': doUndo(); break;
      case 'redo': doRedo(); break;
      case 'play-stop': if (status?.playing) playback.stop(); else playback.play(); break;
      case 'record': recorder.toggle(); break;
      case 'rewind': playback.seekTick(0); break;
      case 'loop': $('chk-loop').checked = !$('chk-loop').checked; $('chk-loop').onchange({ target: $('chk-loop') }); break;
      case 'click': $('chk-click').checked = !$('chk-click').checked; playback.scheduleReload(); break;
      default:
        if (act.startsWith('tab-')) await ui.showTab(act.slice(4));
        break;
    }
  }
  ctx.runAction = runAction;

  // ---------------- File menu ----------------
  const menu = $('file-menu'), editMenu = $('edit-menu');
  const closeMenus = () => { menu.classList.remove('open'); editMenu.classList.remove('open'); };
  $('file-btn').onclick = (e) => { e.stopPropagation(); editMenu.classList.remove('open'); menu.classList.toggle('open'); };
  $('edit-btn').onclick = (e) => {
    e.stopPropagation();
    menu.classList.remove('open');
    refreshHistoryUI();                       // the list is built when opened
    editMenu.classList.toggle('open');
  };
  document.addEventListener('click', closeMenus);
  // The history list rebuilds itself on every click, so it must not close the
  // menu from under the pointer the way a one-shot command does.
  $('hist-list').addEventListener('click', (e) => e.stopPropagation());
  menu.querySelectorAll('[data-act]').forEach((b) => {
    b.onclick = () => { closeMenus(); runAction(b.dataset.act); };
  });
  editMenu.querySelectorAll('[data-act]').forEach((b) => {
    b.onclick = () => { closeMenus(); runAction(b.dataset.act); };
  });

  // ---------------- desktop shell ----------------
  if (desktop.isDesktop) {
    // The native File menu owns these actions; a second in-page copy of it
    // would just be a second place to look. The Edit menu stays: its two
    // commands are duplicated natively, but the history list under them is
    // not something a native menu can show.
    $('file-menu').style.display = 'none';
    desktop.onMenu((act) => runAction(act));
    // Files handed over by the shell: double-clicked in Finder, dropped on
    // the dock, or named on the command line.
    const openHandedFile = async (f) => {
      const file = desktop.wrapFile(f);
      if (/\.msproj$/i.test(file.name)) await files.openProject(ctx, file);
      else await files.importSong(ctx, file);
    };
    desktop.onOpenPath(openHandedFile);
    const launched = await desktop.launchFile();
    if (launched) await openHandedFile(launched);
    syncDocument(true);
  }
  // Drag & drop anything onto the page.
  document.addEventListener('dragover', (e) => e.preventDefault());
  document.addEventListener('drop', async (e) => {
    e.preventDefault();
    for (const f of e.dataTransfer.files) {
      if (/\.msproj$/i.test(f.name)) files.openProject(ctx, f);
      else if (/\.(mid|midi|mstk)$/i.test(f.name)) files.importSong(ctx, f);
      else if (/\.json$/i.test(f.name)) {
        const doc = JSON.parse(await f.text());
        files.importJson(ctx, doc.programs || doc.fallback ? 'bank' : doc.notes ? 'kit' : doc.master && !doc.reverb ? 'master' : 'fx', f);
      }
    }
  });

  // ---------------- global keys ----------------
  document.addEventListener('keydown', (e) => {
    if (['TEXTAREA', 'INPUT', 'SELECT'].includes(e.target.tagName)) return;
    if (e.code === 'Space') { e.preventDefault(); if (status?.playing) playback.stop(); else playback.play(); return; }
    if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === 'z' && currentTab !== 'synth') {
      e.preventDefault();
      if (e.shiftKey) doRedo(); else doUndo();
    } else if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === 'y' && currentTab !== 'synth') {
      e.preventDefault();
      doRedo();
    }
    else if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === 's') {
      e.preventDefault();
      runAction(e.shiftKey ? 'save-as' : 'save');
    }
    else if (e.key.toLowerCase() === 'r' && !e.metaKey && !e.ctrlKey && currentTab !== 'synth') recorder.toggle();
  });
  window.addEventListener('beforeunload', (e) => { if (store.dirty) { e.preventDefault(); e.returnValue = ''; } });

  // ---------------- go ----------------
  setStage('building wavetables…');
  await syncContent(true);
  master.pushAll();
  setStage('preparing the session…');
  await playback.reload({ keepPosition: false });
  arrange.start();
  arrange.refreshTracks();
  refreshHistoryUI();          // both start out with nothing to take back
  log('DAW ready — draw notes in Track, record with ● Rec, bake in File › Bake .mstk', 'ok');
  // Exposed for the smoke test.
  window.__daw = { store, client, ctx, playback, ui, pieces, recorder, cues, cutter, master, editor,
                   fitPanel, roll, undo: doUndo, redo: doRedo };
}
