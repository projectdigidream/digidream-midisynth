//
// Track cutter page: a thin shell over tool/lib/cutter-panel.js (the DAW
// hosts the same panel as its "Cut & Loop" tab).
//
import { SynthClient } from './lib/synth-client.js';
import { makeLogger, logReport, bootStatus } from './lib/ui.js';
import { initCutterPanel } from './lib/cutter-panel.js';

const $ = (id) => document.getElementById(id);
const log = makeLogger($('log'), 200);

$('start-btn').onclick = async () => {
  const setStage = bootStatus($('overlay'));
  let client;
  try {
    setStage('compiling the engine (WASM)…');
    client = await SynthClient.create();
  } catch (err) {
    setStage('failed to start: ' + err + ' — reload the page to retry', true);
    return;
  }
  $('overlay').remove();
  log('engine ready — load a .mstk', 'ok');
  client.addEventListener('fatal', (e) => log('worklet failed: ' + e.detail.error, 'err'));
  client.addEventListener('report', (e) => {
    const m = e.detail;
    if (m.what === 'track' || m.what === 'cut') return;  // the panel logs these itself
    logReport(log, m.what, m.report, null);
  });
  initCutterPanel(document, client, { log });
};
