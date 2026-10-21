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
  // Tall enough that the whole piano roll, scroll bar and lane included, is
  // inside the viewport -- page.mouse works in viewport coordinates, so
  // anything below the fold is silently clicked somewhere else. Touch events
  // are enabled at launch because turning them on later reloads the page,
  // and the unload guard blocks that.
  defaultViewport: { width: 1400, height: 1100 },
  args: ['--autoplay-policy=no-user-gesture-required', '--mute-audio', '--touch-events=enabled'],
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

// Undo/redo as controls, not just key chords -- on a phone there is no
// keyboard, so the buttons are the only way to reach either.
const notes = () => page.evaluate(() => window.__daw.store.p.tracks[0].notes.length);
const hist = () => page.evaluate(() => {
  const h = window.__daw.store.history();
  return { past: h.past, future: h.future,
           undoOff: document.getElementById('btn-undo').disabled,
           redoOff: document.getElementById('btn-redo').disabled };
});
let h = await hist();
check(h.future.length === 1 && !h.redoOff, `undo leaves something to redo (${h.future[0]})`);
await page.click('#btn-redo');
await sleep(300);
check(await notes() === notesBefore + 1, 'the redo button puts the note back');
await page.click('#btn-undo');
await sleep(300);
check(await notes() === notesBefore, 'the undo button takes it away again');

// A fresh edit must drop the redo branch rather than leave it danging.
await page.evaluate(() => window.__daw.store.mutate('new branch', (p) => {
  p.tracks[0].notes.push({ tick: 0, dur: 120, note: 48, vel: 80 });
}));
await sleep(300);
h = await hist();
check(h.future.length === 0 && h.redoOff, 'a new edit drops the redo branch');
check(h.past[h.past.length - 1] === 'new branch', `history is labelled (${h.past.slice(-2).join(', ')})`);

// Stepping to a point in the history, which is what the Edit menu list does.
const depth = (await hist()).past.length;
await page.evaluate(() => window.__daw.store.mutate('step a', (p) => { p.tracks[0].notes.push({ tick: 60, dur: 60, note: 50, vel: 80 }); }));
await page.evaluate(() => window.__daw.store.mutate('step b', (p) => { p.tracks[0].notes.push({ tick: 120, dur: 60, note: 51, vel: 80 }); }));
await sleep(300);
const afterTwo = await notes();
await page.evaluate((d) => window.__daw.store.gotoHistory(d), depth);
await sleep(300);
check(await notes() === afterTwo - 2, `gotoHistory walks back two edits (${afterTwo} -> ${await notes()})`);
await page.evaluate((d) => window.__daw.store.gotoHistory(d + 2), depth);
await sleep(300);
check(await notes() === afterTwo, 'and forward again');

// The Synth tab edits a patch, which keeps its own history; undo and redo
// must both follow the tab. Redo used to ignore it and hit the project.
await page.click('[data-tab="synth"]');
await sleep(600);
const projNotes = await notes();
await page.evaluate(() => {
  const ed = window.__daw.editor;
  ed.patch.layers[0].mixGain = 0.42;
  ed.applyNow();
});
await sleep(400);
await page.evaluate(() => window.__daw.undo());
await sleep(400);
check(await notes() === projNotes, 'undo on the Synth tab leaves the project alone');
await page.evaluate(() => window.__daw.redo());
await sleep(400);
check(await notes() === projNotes, 'and so does redo');
const gain = await page.evaluate(() => window.__daw.editor.patch.layers[0].mixGain);
check(Math.abs(gain - 0.42) < 1e-6, `redo put the patch edit back (mixGain ${gain})`);
await page.click('[data-tab="track"]');
await sleep(400);

// Scrolling the roll. Direction is the whole point here: the notes must move
// WITH the finger on both axes, and the vertical half used to do the
// opposite, which no state-only pass/fail would have caught.
const view = () => page.evaluate(() => window.__daw.roll.view());
const rollEl = async () => {
  await page.$eval('#roll', (el) => el.scrollIntoView({ block: 'center' }));
  await sleep(60);
  return (await page.$('#roll')).boundingBox();
};

// Stop first: while the transport runs the view follows the playhead, which
// quietly undoes every scroll under test.
await page.evaluate(() => window.__daw.playback.stop());
await sleep(200);
await page.evaluate(() => {
  window.__daw.store.mutate('scroll fixture', (p) => {
    p.tracks[0].notes = [];
    // Long enough that the song does not fit the view -- with nothing to
    // scroll to, every scroll test passes or fails for the wrong reason.
    for (let i = 0; i < 96; i++)
      p.tracks[0].notes.push({ tick: 480 + i * 480, dur: 300, note: 60 + (i % 5), vel: 100 });
  });
});
await sleep(400);

// Sliding along the key column scrolls pitch; a tap there still auditions.
let v0 = await view();
let b = await rollEl();
await page.mouse.move(b.x + 20, b.y + 150);
await page.mouse.down();
await page.mouse.move(b.x + 20, b.y + 250, { steps: 10 });
await page.mouse.up();
await sleep(200);
let v1 = await view();
const rows = Math.round(100 / v0.noteH);
check(v1.topNote === v0.topNote + rows,
      `dragging the keys down scrolls pitch down ${rows} rows (${v0.topNote} -> ${v1.topNote})`);

// The scroll bar between the grid and the lane.
b = await rollEl();
v0 = await view();
const sbY = b.y + v0.sbCss.y + v0.sbCss.h / 2;   // the middle of the strip, wherever it is
await page.mouse.move(b.x + 120, sbY);
await page.mouse.down();
await page.mouse.move(b.x + 340, sbY, { steps: 10 });
await page.mouse.up();
await sleep(200);
v1 = await view();
check(v1.scrollTick > v0.scrollTick, `the scroll bar moves along the song (${Math.round(v0.scrollTick)} -> ${Math.round(v1.scrollTick)} ticks)`);
await page.mouse.move(b.x + 340, sbY);
await page.mouse.down();
await page.mouse.move(b.x + 60, sbY, { steps: 10 });
await page.mouse.up();
await sleep(200);
check((await view()).scrollTick < v1.scrollTick, 'and back');

// Two fingers: pan both ways, and a pan that holds its spread must not zoom.
const twoFinger = async (dx, dy, spread = 0) => {
  const r = await rollEl();
  const cdp = await page.createCDPSession();
  const at = (f) => [
    { x: r.x + r.width / 2 - 60 - spread * f + dx * f, y: r.y + r.height / 2 - 40 + dy * f, id: 1 },
    { x: r.x + r.width / 2 + 60 + spread * f + dx * f, y: r.y + r.height / 2 + 40 + dy * f, id: 2 },
  ];
  await cdp.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: at(0) });
  for (let i = 1; i <= 8; i++)
    await cdp.send('Input.dispatchTouchEvent', { type: 'touchMove', touchPoints: at(i / 8) });
  await cdp.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
  await cdp.detach();
  await sleep(200);
};
v0 = await view();
await twoFinger(0, 120);
v1 = await view();
check(v1.topNote > v0.topNote, `two fingers down brings the notes down with them (${v0.topNote} -> ${v1.topNote})`);
await twoFinger(0, -120);
check((await view()).topNote < v1.topNote, 'two fingers up take them back');

v0 = await view();
await twoFinger(-180, 0);
v1 = await view();
check(v1.scrollTick > v0.scrollTick, `two fingers left carry the song along (${Math.round(v0.scrollTick)} -> ${Math.round(v1.scrollTick)} ticks)`);
await twoFinger(180, 0);
check((await view()).scrollTick < v1.scrollTick, 'two fingers right bring it back');

// The deadzone: a pan holding its spread leaves both zooms exactly alone.
v0 = await view();
await twoFinger(0, 100);
v1 = await view();
check(v1.noteH === v0.noteH && v1.pxPerBeat === v0.pxPerBeat,
      `panning does not zoom (noteH ${v0.noteH} -> ${v1.noteH}, pxPerBeat ${v0.pxPerBeat} -> ${v1.pxPerBeat})`);
// ... but a real pinch still does, on the axis it is spread along.
await twoFinger(0, 0, 120);
check((await view()).pxPerBeat > v1.pxPerBeat * 1.2, 'a pinch still zooms the time axis');

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

// Autosave: an edit lands in localStorage, and a reload comes back to it.
await page.evaluate(() => window.__daw.store.mutate('autosave fixture', (p) => {
  p.name = 'recovered-me';
  p.tracks[0].notes.push({ tick: 960, dur: 240, note: 71, vel: 99 });
}));
await page.evaluate(() => window.__daw.autosave.now());
const saved = await page.evaluate(() => {
  const raw = localStorage.getItem(window.__daw.autosave.key);
  return raw ? JSON.parse(raw).name : null;
});
check(saved === 'recovered-me', `the project is in localStorage (${saved})`);

const wantNotes = await page.evaluate(() => window.__daw.store.p.tracks[0].notes.length);
// The unload guard would block a reload, so drop the flag the way a save does.
await page.evaluate(() => { window.__daw.store.dirty = false; });
await page.reload({ waitUntil: 'networkidle0' });
await page.click('#start-btn');
await page.waitForFunction(() => window.__daw && !document.getElementById('overlay'), { timeout: 20000 });
await sleep(600);
const back = await page.evaluate(() => ({
  name: window.__daw.store.p.name,
  notes: window.__daw.store.p.tracks[0].notes.length,
  restored: window.__daw.autosave.restored,
  path: window.__daw.ctx.projectPath,
}));
check(back.restored && back.name === 'recovered-me' && back.notes === wantNotes,
      `reload recovers the session (${back.name}, ${back.notes} notes)`);
check(!back.path, 'but not the file path, so a save cannot overwrite the wrong file');

// New project drops the snapshot rather than leaving it to come back later.
await page.evaluate(() => { window.__daw.store.dirty = false; });
await page.evaluate(() => window.__daw.ctx.runAction('new'));
await sleep(500);
const cleared = await page.evaluate(() => localStorage.getItem(window.__daw.autosave.key));
check(cleared === null, 'New project clears the snapshot');

const realErrors = pageErrors.filter((e) => !e.includes('favicon') && !e.startsWith('console: Failed to load resource'));
check(realErrors.length === 0, 'no page errors' + (realErrors.length ? ': ' + realErrors.join(' | ') : ''));

await browser.close();
process.exit(failures ? 1 : 0);
