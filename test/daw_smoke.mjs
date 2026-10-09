#!/usr/bin/env node
//
// Browser smoke test for the DAW (Chrome + puppeteer-core; skipped when
// missing). Boots the page, builds a project through the store, plays it,
// edits while playing (hot swap keeps position), drives the recorder with a
// synthetic MIDI stream, enables mastering and reads meters, bakes + cuts in
// Cut & Loop, queues a cue change at loop end and checks the switch, and
// exercises the exports.
//
//   node tool/serve.js &  &&  node test/daw_smoke.mjs   (from a dir with puppeteer-core)
//
import { createRequire } from 'node:module';
import { existsSync } from 'node:fs';

const CHROME = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
let puppeteer;
try {
  const require = createRequire(process.cwd() + '/package.json');
  puppeteer = require('puppeteer-core');
} catch {
  try { puppeteer = (await import('puppeteer-core')).default; }
  catch { console.log('skip: puppeteer-core not installed (run from a dir that has it)'); process.exit(0); }
}
if (!existsSync(CHROME)) { console.log('skip: Chrome not found'); process.exit(0); }

let failures = 0;
const check = (cond, what) => { if (cond) console.log('ok', what); else { console.error('FAIL', what); failures++; } };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const browser = await puppeteer.launch({
  executablePath: CHROME, headless: 'new',
  args: ['--autoplay-policy=no-user-gesture-required', '--mute-audio'],
});
const page = await browser.newPage();
const pageErrors = [];
page.on('pageerror', (e) => pageErrors.push(e.message));
page.on('console', (m) => { if (m.type() === 'error') pageErrors.push('console: ' + m.text()); });
page.on('response', (r) => { if (r.status() >= 400 && !r.url().includes('favicon')) pageErrors.push(`${r.status()} ${r.url()}`); });
const cdp = await page.createCDPSession();
await cdp.send('Browser.setDownloadBehavior', { behavior: 'allow', downloadPath: '/tmp/midisynth_daw_dl' });

await page.goto('http://localhost:8930/tool/daw.html', { waitUntil: 'networkidle0' });
await page.click('#start-btn');
await page.waitForFunction(() => window.__daw && !document.getElementById('overlay'), { timeout: 20000 });
check(true, 'DAW boots');

// Build a 2-bar project: bass line on ch1, kick on ch10.
await page.evaluate(() => {
  const { store } = window.__daw;
  store.mutate('smoke notes', (p) => {
    p.loop = { startTick: 0, endTick: 1920 * 2, enabled: true };
    const t = p.tracks.find((x) => x.channel === 0);
    t.program = 33;
    for (let b = 0; b < 8; b++) t.notes.push({ tick: b * 480, dur: 400, note: 40 + (b % 4) * 3, vel: 100 });
    const d = p.tracks.find((x) => x.channel === 9);
    for (let b = 0; b < 8; b++) d.notes.push({ tick: b * 480, dur: 60, note: b % 2 ? 42 : 36, vel: 110 });
  });
});
await sleep(400);
await page.evaluate(() => window.__daw.playback.play());
await sleep(1200);
let st = await page.evaluate(() => ({ s: window.__daw.playback.status(), tick: window.__daw.playback.positionTick() }));
check(st.s.playing && st.tick > 0, `plays (tick ${Math.round(st.tick)})`);
check(st.s.activeNotes && String(st.s.activeNotes[0]).length > 0, `activeNotes highlights ch1 (${st.s.activeNotes[0]})`);

// Edit while playing: position keeps advancing, no page errors.
const before = await page.evaluate(() => window.__daw.playback.positionTick());
await page.evaluate(() => {
  const { store } = window.__daw;
  store.mutate('edit while playing', (p) => { p.tracks[0].notes.push({ tick: 240, dur: 200, note: 52, vel: 90 }); });
});
await sleep(600);
const after = await page.evaluate(() => window.__daw.playback.positionTick());
check(after !== before, `hot swap keeps the transport running (${Math.round(before)} -> ${Math.round(after)})`);
const posEl = await page.evaluate(() => document.getElementById('pos').textContent);
check(/^\d+\.\d+\.\d{3}$/.test(posEl), `position display ${posEl}`);

// Piano roll tab draws a note via pointer events.
await page.click('[data-tab="track"]');
await sleep(300);
const rollBox = await (await page.$('#roll')).boundingBox();
const notesBefore = await page.evaluate(() => window.__daw.store.p.tracks[0].notes.length);
await page.mouse.move(rollBox.x + 200, rollBox.y + 120);
await page.mouse.down();
await page.mouse.move(rollBox.x + 260, rollBox.y + 120, { steps: 4 });
await page.mouse.up();
await sleep(300);
const notesAfter = await page.evaluate(() => window.__daw.store.p.tracks[0].notes.length);
check(notesAfter === notesBefore + 1, `piano roll draw adds a note (${notesBefore} -> ${notesAfter})`);
await page.keyboard.down('Meta'); await page.keyboard.press('z'); await page.keyboard.up('Meta');
await sleep(300);
const notesUndo = await page.evaluate(() => window.__daw.store.p.tracks[0].notes.length);
check(notesUndo === notesBefore, `undo removes it (${notesUndo})`);

// Recording: synthetic MIDI stream onto the armed track.
await page.evaluate(async () => {
  const { store, recorder } = window.__daw;
  store.mutate('arm', (p) => { for (const t of p.tracks) t.arm = t.channel === 0; }, { audio: false });
  document.getElementById('count-in').value = '0';
  document.getElementById('rec-quant').value = '1/8';
  await recorder.toggle();
});
await sleep(700);
await page.evaluate(async () => {
  const { recorder } = window.__daw;
  const now = performance.now();
  for (let i = 0; i < 4; i++) {
    recorder.midiMessage(new Uint8Array([0x90, 60 + i, 100]), now - 500 + i * 120);
    recorder.midiMessage(new Uint8Array([0x80, 60 + i, 0]), now - 500 + i * 120 + 90);
  }
});
await sleep(300);
const recNotes = await page.evaluate(async () => {
  const { recorder, store } = window.__daw;
  const n0 = store.p.tracks[0].notes.length;
  await recorder.stop();
  return store.p.tracks[0].notes.length - n0;
});
check(recNotes === 4, `recorder commits 4 notes (${recNotes})`);
check((await page.evaluate(() => document.getElementById('rec-state').textContent)).includes('recorded 4'),
      'record state text');

// Synth tab edits the instrument the selected track plays (program 33 above).
await page.click('[data-tab="synth"]');
await sleep(300);
const synth = await page.evaluate(() => ({
  target: document.querySelector('#tab-synth #edit-target').value,
  hint: document.getElementById('synth-hint').textContent,
}));
check(synth.target === '33' && synth.hint.includes('played by'), `Synth tab targets the track's program (${synth.target}; ${synth.hint.slice(0, 60)}…)`);

// Master tab: enable, meters arrive.
await page.click('[data-tab="master"]');
await sleep(200);
await page.evaluate(() => { window.__daw.playback.play(); });
await page.click('#mst-enabled');
await sleep(900);
const meters = await page.evaluate(() => ({
  en: window.__daw.store.p.master.enabled,
  m: window.__daw.playback.status()?.meters,
  lat: document.getElementById('mst-latency').textContent,
  pl: document.getElementById('mt-pl-v').textContent,
}));
check(meters.en && meters.m && meters.m.enabled === true, 'mastering enabled in project + engine');
check(meters.m.peakL > 0 && meters.pl !== '', `meters read (peak L ${meters.pl} dBFS)`);
check(meters.lat.includes('latency'), `latency shown: ${meters.lat}`);
await page.evaluate(() => { const s = document.querySelector('[data-mp="gainDb"]'); s.value = -6; s.dispatchEvent(new Event('input')); });
await sleep(100);
check((await page.evaluate(() => window.__daw.store.p.master.gainDb)) === -6, 'EQ/gain slider writes the project');

// Cut & Loop: bake project, slice two bars, both become cue pieces.
await page.click('[data-tab="cut"]');
await page.click('#cut-bake');
await page.waitForFunction(() => document.getElementById('track-desc').textContent.includes('bars'), { timeout: 20000 });
check(true, 'project baked and loaded in the cutter: ' + await page.evaluate(() => document.getElementById('track-desc').textContent));
await page.evaluate(async () => {
  const { cutter } = window.__daw;
  await cutter.addSlice({ startTick: 0, endTick: 1920 }, 'A');
  await cutter.addSlice({ startTick: 1920, endTick: 3840 }, 'B');
});
await sleep(300);
const pieceNames = await page.evaluate(() => [...window.__daw.pieces.keys()]);
check(pieceNames.includes('A') && pieceNames.includes('B'), `slices are cue pieces (${pieceNames})`);
check((await page.evaluate(() => window.__daw.store.p.slices.length)) === 2, 'project remembers the slices');

// Cues: play A, queue B at loop end, expect the switch at the boundary.
await page.click('[data-tab="cues"]');
await sleep(300);
await page.evaluate(() => window.__daw.cues.playNow('A'));
await sleep(600);
const sw0 = await page.evaluate(() => window.__daw.playback.status()?.pieceSwitches);
await page.evaluate(() => window.__daw.cues.queue('B', 'loopEnd'));
await sleep(300);
const q = await page.evaluate(() => ({ s: window.__daw.playback.status(), cur: window.__daw.cues.current,
                                         log: document.getElementById('log').textContent.slice(-300) }));
check(q.s.queued === true && q.s.msToBoundary > 0, `queued: ${(q.s.msToBoundary / 1000).toFixed(2)}s to the boundary (queued=${q.s.queued}, cur=${q.cur})\n  log: ${q.log}`);
await sleep(Math.min(6000, q.s.msToBoundary + 800));
const afterSw = await page.evaluate(() => ({ s: window.__daw.playback.status(), cur: window.__daw.cues.current,
                                              log: document.getElementById('cue-log').textContent }));
check(afterSw.s.pieceSwitches === sw0 + 1 && afterSw.cur === 'B', `switched to B at the boundary (switches ${sw0} -> ${afterSw.s.pieceSwitches})`);
check(afterSw.log.includes('at the boundary'), 'transition logged');
check(afterSw.s.playing, 'B keeps playing');
const cuesJson = await page.evaluate(() => JSON.parse(window.__daw.cues.exportCuesJson()));
check(cuesJson.pieces.length === 2 && cuesJson.entry, 'cues.json export');

// Back to Arrange restores the project content and song.
await page.click('[data-tab="arrange"]');
await sleep(800);
const restored = await page.evaluate(() => window.__daw.ctx.inPiecesMode());
check(restored === false, 'leaving the piece tabs restores the project');

// Exports: .mid bytes + project JSON round trip + .mstk bake.
const exp = await page.evaluate(async () => {
  const { store, ctx } = window.__daw;
  const mod = await import('./daw/smf.js');
  const bytes = mod.projectToSmf(store.p);
  const back = mod.smfToProject(bytes);
  const json = store.toJSON();
  const files = await import('./daw/export.js');
  const baked = await files.bakeProject(ctx);
  return { smfLen: bytes.length, tracks: back.tracks.length, jsonLen: json.length, baked: baked ? baked.byteLength : 0 };
});
check(exp.smfLen > 100 && exp.tracks === 2, `.mid export (${exp.smfLen} bytes, ${exp.tracks} tracks)`);
check(exp.jsonLen > 1000, `.msproj serializes (${exp.jsonLen} chars)`);
check(exp.baked > 1000, `.mstk bakes with mastering (${exp.baked} bytes)`);

const realErrors = pageErrors.filter((e) => !e.includes('favicon') && !e.startsWith('console: Failed to load resource'));
check(realErrors.length === 0, 'no page errors' + (realErrors.length ? ': ' + realErrors.join(' | ') : ''));

await browser.close();
process.exit(failures ? 1 : 0);
