//
// Patch & kit authoring page: a thin shell over tool/lib/*. The same panels
// are hosted as tabs by the DAW (daw.html); this page keeps the classic
// single-screen layout.
//
import { SynthClient } from './lib/synth-client.js';
import { makeLogger, logReport, bootStatus } from './lib/ui.js';
import { createScope } from './lib/scope.js';
import { initPatchEditor } from './lib/patch-editor.js';
import { initSongPanel } from './lib/song-panel.js';
import { setupMidi } from './lib/midi-input.js';

const $ = (id) => document.getElementById(id);
const log = makeLogger($('log'), 150);
const store = { bankDoc: null, kitDoc: null };

$('start-btn').onclick = async () => {
  const setStage = bootStatus($('overlay'));
  let client;
  try {
    setStage('compiling the engine (WASM)…');
    client = await SynthClient.create({ analyser: true });
  } catch (err) {
    setStage('failed to start: ' + err + ' — reload the page to retry', true);
    return;
  }
  client.addEventListener('fatal', (e) => log('worklet failed: ' + e.detail.error, 'err'));
  // Every report is logged once here; panels only add their own state.
  client.addEventListener('report', (e) => {
    const m = e.detail;
    if (m.what === 'patch') return;  // the editor shows apply state itself
    logReport(log, m.what, m.report, m.what === 'song' || m.what === 'track' ? null : 'loaded ok');
  });
  createScope($('scope'), client.analyser).start();

  setStage('loading instruments (bank + kit)…');
  const [bankText, kitText] = await Promise.all([
    fetch(new URL('../content/patches/gm_bank.json', import.meta.url)).then((r) => r.text()),
    fetch(new URL('../content/drums/gm_kit.json', import.meta.url)).then((r) => r.text()),
  ]);
  store.bankDoc = JSON.parse(bankText);
  store.kitDoc = JSON.parse(kitText);
  client.loadBank(bankText).catch(() => {});
  client.loadDrums(kitText).catch(() => {});

  setStage('building wavetables…');
  const editor = initPatchEditor(document, client, store, { log });
  editor.activate();
  initSongPanel(document, client, store, { log, editor });
  setupMidi({ onMessage: (d) => editor.midiMessage(d), statusEl: $('midi-state') });
  $('overlay').remove();
};
