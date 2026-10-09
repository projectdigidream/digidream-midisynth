#!/usr/bin/env node
//
// Browser smoke test for the authoring tool (needs Chrome + puppeteer-core;
// skipped by CI when either is missing). Catches whole-flow breakage the
// unit suite can't see: worklet boot, song load, instruments-panel
// navigation, patch editor state, and any page error thrown along the way.
//
//   node tool/serve.js &  &&  node test/tool_smoke.mjs
//
import { createRequire } from 'node:module';
import { existsSync } from 'node:fs';

const CHROME = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
let puppeteer;
try {
  const require = createRequire(process.cwd() + '/package.json');
  puppeteer = require('puppeteer-core');
} catch {
  try {
    puppeteer = (await import('puppeteer-core')).default;
  } catch {
    console.log('skip: puppeteer-core not installed (run from a dir that has it)');
    process.exit(0);
  }
}
if (!existsSync(CHROME)) {
  console.log('skip: Chrome not found');
  process.exit(0);
}

let failures = 0;
const check = (cond, what) => {
  if (cond) console.log('ok', what);
  else { console.error('FAIL', what); failures++; }
};

const browser = await puppeteer.launch({
  executablePath: CHROME,
  headless: 'new',
  args: ['--autoplay-policy=no-user-gesture-required', '--mute-audio'],
});
const page = await browser.newPage();
const pageErrors = [];
page.on('pageerror', (e) => pageErrors.push(e.message));

await page.goto('http://localhost:8930/tool/', { waitUntil: 'networkidle0' });
await page.click('#start-btn');
await page.waitForFunction(() => !document.getElementById('overlay'), { timeout: 15000 });
check(true, 'worklet ready');

// Drums song -> instruments panel -> drum row navigates the editor.
await page.click('[data-song="test/golden/drums.mid"]');
await page.waitForFunction(
  () => document.querySelectorAll('#instruments .inst').length > 0, { timeout: 10000 });
const clickRowStarting = async (prefix) => {
  const handles = await page.$$('#instruments .inst');
  for (const h of handles) {
    const t = await h.evaluate((e) => e.textContent.trim());
    if (t.startsWith(prefix)) { await h.click(); return true; }
  }
  return false;
};
check(await clickRowStarting('49'), 'crash row present');
await new Promise((r) => setTimeout(r, 250));
let st = await page.evaluate(() => ({
  mode: document.getElementById('edit-mode').value,
  target: document.getElementById('edit-target').value,
  name: document.getElementById('p-name').value,
}));
check(st.mode === 'kit' && st.target === '49' && st.name === 'crash_1',
      `drum row click selects the clicked drum (got ${st.target} ${st.name})`);

// Melody song -> melodic instrument row.
await page.click('[data-song="test/golden/melody.mid"]');
await page.waitForFunction(
  () => [...document.querySelectorAll('#instruments .inst')].some((e) =>
      e.textContent.includes('String Ensemble 1')), { timeout: 10000 });
check(await clickRowStarting('48'), 'strings row present');
await new Promise((r) => setTimeout(r, 250));
st = await page.evaluate(() => ({
  mode: document.getElementById('edit-mode').value,
  target: document.getElementById('edit-target').value,
  name: document.getElementById('p-name').value,
}));
check(st.mode === 'bank' && st.target === '48' && st.name === 'string_ensemble_1',
      `melodic row click selects the clicked program (got ${st.target} ${st.name})`);

// Kit mode shows drum pads; clicking a pad selects that drum.
const padCount = await page.evaluate(() => {
  document.getElementById('edit-mode').value = 'kit';
  document.getElementById('edit-mode').dispatchEvent(new Event('change'));
  return document.querySelectorAll('#kbd .pad').length;
});
check(padCount > 20, `kit mode shows drum pads (${padCount})`);
await page.evaluate(() => {
  document.querySelector('#kbd .pad[data-note="42"]')
      .dispatchEvent(new PointerEvent('pointerdown', { bubbles: true }));
});
await new Promise((r) => setTimeout(r, 250));
st = await page.evaluate(() => ({
  target: document.getElementById('edit-target').value,
  name: document.getElementById('p-name').value,
}));
check(st.target === '42' && st.name === 'hihat_closed',
      `pad click selects that drum (got ${st.target} ${st.name})`);

// Layers panel exists with legend chips; generator UI switches and applies.
await page.evaluate(() => {
  document.getElementById('edit-mode').value = 'bank';
  document.getElementById('edit-mode').dispatchEvent(new Event('change'));
});
await new Promise((r) => setTimeout(r, 200));
const legendChips = await page.$$eval('#layers-legend .lchip', (els) => els.length);
check(legendChips >= 1, `layers legend chips (${legendChips})`);
await page.select('#wt-gen', 'fm2op');
await new Promise((r) => setTimeout(r, 600));
let apState = await page.evaluate(() => document.getElementById('apply-state').textContent);
check(apState.includes('applied'), `generator switch to fm2op applies (${apState})`);
const genFields = await page.$$eval('#gen-params input, #gen-params select', (els) => els.length);
check(genFields === 3, `fm2op shows ratio/index fields (${genFields})`);

// A/B store + swap round-trips.
await page.click('#btn-ab-store');
await page.select('#wt-gen', 'pulse');
await new Promise((r) => setTimeout(r, 500));
await page.click('#btn-ab-swap');
await new Promise((r) => setTimeout(r, 500));
let genNow = await page.evaluate(() => document.getElementById('wt-gen').value);
check(genNow === 'fm2op', `A/B swap restores snapshot (${genNow})`);

// Undo returns to the pre-swap state.
await page.click('#btn-undo');
await new Promise((r) => setTimeout(r, 400));
genNow = await page.evaluate(() => document.getElementById('wt-gen').value);
check(genNow === 'pulse', `undo steps back (${genNow})`);

// ... and redo steps forward again, which the editor could not do at all.
await page.click('#btn-redo');
await new Promise((r) => setTimeout(r, 400));
genNow = await page.evaluate(() => document.getElementById('wt-gen').value);
check(genNow === 'fm2op', `redo steps forward (${genNow})`);
await page.click('#btn-undo');
await new Promise((r) => setTimeout(r, 400));

// History is per patch. One shared stack meant undoing here scanned past the
// other programs' entries and threw them away on the way, so switching back
// found nothing to undo.
const selectTarget = async (v) => {
  await page.evaluate((val) => {
    document.getElementById('edit-target').value = val;
    document.getElementById('edit-target').dispatchEvent(new Event('change'));
  }, v);
  await new Promise((r) => setTimeout(r, 400));
};
const here = await page.evaluate(() => document.getElementById('edit-target').value);
await selectTarget('48');
await page.select('#wt-gen', 'saw');
await new Promise((r) => setTimeout(r, 500));
await page.click('#btn-undo');                    // spends 48's history
await new Promise((r) => setTimeout(r, 400));
await selectTarget(here);
const undoOff = await page.evaluate(() => document.getElementById('btn-undo').disabled);
check(!undoOff, 'the other patch still has its own history to undo');
await page.click('#btn-undo');
await new Promise((r) => setTimeout(r, 400));
genNow = await page.evaluate(() => document.getElementById('wt-gen').value);
check(genNow !== 'pulse', `and undoing there still works (${genNow})`);

// Filter response canvas painted (non-blank for a lowpass layer).
await page.evaluate(() => {
  document.getElementById('edit-target').value = '48';
  document.getElementById('edit-target').dispatchEvent(new Event('change'));
});
await new Promise((r) => setTimeout(r, 300));
const respPainted = await page.evaluate(() => {
  const c = document.getElementById('filter-resp');
  const g = c.getContext('2d');
  const d = g.getImageData(0, 0, c.width, c.height).data;
  let lit = 0;
  for (let i = 0; i < d.length; i += 4) if (d[i] > 40 || d[i + 2] > 60) lit++;
  return lit;
});
check(respPainted > 50, `filter response curve painted (${respPainted} px)`);

// Render button produces a WAV download.
const cdp = await page.createCDPSession();
await cdp.send('Browser.setDownloadBehavior',
               { behavior: 'allow', downloadPath: '/tmp/midisynth_dl' });
await page.click('#btn-render');
await page.waitForFunction(
  () => document.getElementById('render-state').textContent.startsWith('done'),
  { timeout: 60000 });
check(true, 'in-editor render completes: ' +
      await page.evaluate(() => document.getElementById('render-state').textContent));

// ---------------- track cutter flow ----------------
import('node:child_process').then(() => {});
const { execFileSync } = await import('node:child_process');
const trackPath = '/tmp/smoke_melody.mstk';
execFileSync('build/midisynth_bake',
  ['test/golden/melody.mid', '-o', trackPath,
   '--bank', 'content/patches/gm_bank.json', '--drums', 'content/drums/gm_kit.json'],
  { cwd: new URL('..', import.meta.url).pathname });

const cutter = await browser.newPage();
const cutterErrors = [];
cutter.on('pageerror', (e) => cutterErrors.push(e.message));
await cutter.goto('http://localhost:8930/tool/cutter.html', { waitUntil: 'networkidle0' });
await cutter.click('#start-btn');
await cutter.waitForFunction(() => !document.getElementById('overlay'), { timeout: 15000 });
const fileInput = await cutter.$('#track-file');
await fileInput.uploadFile(trackPath);
await cutter.waitForFunction(
  () => document.getElementById('track-desc').textContent.includes('bars'), { timeout: 10000 });
check(true, 'cutter loads .mstk: ' +
      await cutter.evaluate(() => document.getElementById('track-desc').textContent));

// Drag a region across the timeline (20% -> 60%).
const box = await (await cutter.$('#timeline')).boundingBox();
await cutter.mouse.move(box.x + box.width * 0.2, box.y + box.height / 2);
await cutter.mouse.down();
await cutter.mouse.move(box.x + box.width * 0.6, box.y + box.height / 2, { steps: 5 });
await cutter.mouse.up();
await new Promise((r) => setTimeout(r, 200));
const regionLabel = await cutter.evaluate(() => document.getElementById('region-label').textContent);
check(regionLabel.startsWith('bars'), `region selected (${regionLabel})`);

// Loop-preview the region, then add it as a slice.
await cutter.click('#btn-preview');
await new Promise((r) => setTimeout(r, 400));
await cutter.click('#btn-add-slice');
await cutter.waitForFunction(
  () => document.querySelectorAll('#slice-rows tr').length === 1, { timeout: 10000 });
check(true, 'slice cut and added');

// Crossfade into the slice.
await cutter.evaluate(() => document.querySelector('[data-a=xfade]').click());
await new Promise((r) => setTimeout(r, 600));
const cutterLog = await cutter.evaluate(() => document.getElementById('log').textContent);
check(cutterLog.includes('crossfading into slice'), 'crossfade into slice triggered');
check(!cutterLog.includes('crossfade] error') && !cutterLog.includes('cut failed'),
      'no cutter errors in log');
const cutterReal = cutterErrors.filter((e) => !e.includes('favicon'));
check(cutterReal.length === 0, 'cutter: no page errors' +
      (cutterReal.length ? ': ' + cutterReal.join(' | ') : ''));

const realErrors = pageErrors.filter((e) => !e.includes('favicon'));
check(realErrors.length === 0, 'no page errors' +
      (realErrors.length ? ': ' + realErrors.join(' | ') : ''));

await browser.close();
process.exit(failures ? 1 : 0);
