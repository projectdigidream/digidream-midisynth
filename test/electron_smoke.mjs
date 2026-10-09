#!/usr/bin/env node
//
// Smoke test for the desktop build (Electron + puppeteer-core over the
// remote debugging port; skipped when either is missing).
//
// What it is really checking: the DAW is served from the private app://
// protocol rather than a web server, so this proves ES modules, the
// AudioWorklet, the WASM engine and fetch() all still work there, that the
// preload bridge reached the page, and that the audio clock actually runs.
//
//   cd midisynth/electron && npm install     (once)
//   node test/electron_smoke.mjs             (from a dir with puppeteer-core)
//   node test/electron_smoke.mjs --packaged  the built app in electron/dist
//
import { createRequire } from 'node:module';
import { existsSync, unlinkSync } from 'node:fs';
import os from 'node:os';
import { spawn } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const ELECTRON_DIR = path.join(ROOT, 'electron');

let puppeteer;
try {
  const require = createRequire(process.cwd() + '/package.json');
  puppeteer = require('puppeteer-core');
} catch {
  try { puppeteer = (await import('puppeteer-core')).default; }
  catch { console.log('skip: puppeteer-core not installed (run from a dir that has it)'); process.exit(0); }
}

// Either the dev shell (electron .) or the built application -- the packaged
// app reads its payload out of app.asar, which is a different code path.
const PACKAGED = process.argv.includes('--packaged');
let launchBin, launchArgs, launchCwd = ELECTRON_DIR;
if (PACKAGED) {
  const candidates = [
    'dist/mac-arm64/DigidreamSynth.app/Contents/MacOS/DigidreamSynth',
    'dist/mac/DigidreamSynth.app/Contents/MacOS/DigidreamSynth',
    'dist/linux-unpacked/digidreamsynth',
    'dist/win-unpacked/DigidreamSynth.exe',
  ].map((p) => path.join(ELECTRON_DIR, p));
  launchBin = candidates.find((p) => existsSync(p));
  launchArgs = [];
  if (!launchBin) { console.log('skip: no packaged app (scripts/package_electron.sh --dir)'); process.exit(0); }
} else {
  try {
    const require = createRequire(path.join(ELECTRON_DIR, 'package.json'));
    launchBin = require('electron');
  } catch {
    console.log('skip: electron not installed (cd electron && npm install)');
    process.exit(0);
  }
  launchArgs = ['.'];
}
if (!existsSync(path.join(ELECTRON_DIR, 'payload', 'tool', 'daw.html'))
    && !existsSync(path.join(ROOT, 'tool', 'dist', 'midisynth.mjs'))) {
  console.log('skip: no engine build (scripts/build_wasm.sh)');
  process.exit(0);
}

let failures = 0;
const check = (cond, what) => { if (cond) console.log('ok', what); else { console.error('FAIL', what); failures++; } };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// A port of our own: a stray instance from an earlier run must never be
// mistaken for the one under test.
const PORT = 9300 + Math.floor(Math.random() * 600);
const app = spawn(launchBin, [...launchArgs, `--remote-debugging-port=${PORT}`], {
  cwd: launchCwd,
  // ELECTRON_RUN_AS_NODE (exported by VS Code terminals) would boot the
  // binary as plain Node; clear it the way electron/run.js does.
  env: (() => { const e = { ...process.env, DGS_SMOKE: '1' }; delete e.ELECTRON_RUN_AS_NODE; return e; })(),
  stdio: ['ignore', 'pipe', 'pipe'],
});
const mainLog = [];
app.stdout.on('data', (d) => mainLog.push(String(d)));
app.stderr.on('data', (d) => mainLog.push(String(d)));

const stop = () => { try { app.kill('SIGTERM'); } catch {} };
process.on('exit', stop);

let browser = null;
try {
  // Wait for the debugging endpoint.
  for (let i = 0; i < 100 && !browser; i++) {
    await sleep(200);
    try { browser = await puppeteer.connect({ browserURL: `http://127.0.0.1:${PORT}`, defaultViewport: null }); }
    catch { /* not up yet */ }
  }
  if (!browser) throw new Error('electron never opened the debugging port:\n' + mainLog.join(''));
  check(true, `app launches (${PACKAGED ? 'packaged' : 'dev'})`);

  // A previous instance still shutting down can delay the window: give it
  // room rather than failing the run on a startup race.
  let page = null;
  for (let i = 0; i < 150 && !page; i++) {
    const pages = await browser.pages();
    page = pages.find((p) => p.url().includes('daw.html')) || null;
    if (!page) await sleep(200);
  }
  check(!!page, 'DAW window loads over app://');
  if (!page) throw new Error('no DAW page');

  const pageErrors = [];
  page.on('pageerror', (e) => pageErrors.push(e.message));
  page.on('console', (m) => { if (m.type() === 'error') pageErrors.push('console: ' + m.text()); });

  check(page.url().startsWith('app://digidream/'), 'page origin is the private protocol');

  // The engine boots by itself here: no Start-audio click, because the shell
  // runs with autoplay allowed. This only resolves if the worklet module,
  // the WASM engine and the content fetches all loaded over app://.
  await page.waitForFunction(() => window.__daw && !document.getElementById('overlay'), { timeout: 45000 });
  check(true, 'engine boots without a user gesture (worklet + wasm over app://)');

  check(await page.evaluate(() => window.digidream?.isDesktop === true), 'preload bridge is exposed');
  check(await page.evaluate(() => typeof window.digidream.save === 'function'
                                && typeof window.digidream.open === 'function'), 'file bridge present');
  check(await page.evaluate(() => document.getElementById('file-menu').style.display === 'none'),
        'in-page File menu yields to the native menu');

  // The audio clock has to actually advance: this is the real AudioWorklet
  // test (a stalled worklet still boots fine).
  await page.evaluate(() => window.__daw.store.mutate('notes', (p) => {
    p.tracks[0].notes = [0, 1, 2, 3].map((i) => ({ tick: i * 480, dur: 440, note: 60 + i * 3, vel: 100 }));
  }));
  await page.evaluate(() => window.__daw.playback.play());
  await sleep(1200);
  const t1 = await page.evaluate(() => window.__daw.playback.msNow());
  await sleep(600);
  const t2 = await page.evaluate(() => window.__daw.playback.msNow());
  check(t1 > 50 && t2 > t1 + 200, `transport clock runs (${t1 | 0}ms -> ${t2 | 0}ms)`);
  check(await page.evaluate(() => (window.__daw.playback.status()?.activeVoices ?? 0) >= 0), 'engine reports status');
  await page.evaluate(() => window.__daw.playback.stop());

  // The file bridge, end to end -- with an explicit path, so no modal dialog
  // (a Save dialog would hang a headless run forever).
  const tmp = path.join(os.tmpdir(), `dgs_smoke_${Date.now()}.json`);
  const saved = await page.evaluate(async (dest) => {
    const r = await window.digidream.save('smoke.json', JSON.stringify({ ok: 1 }), { toPath: dest });
    return r?.path || null;
  }, tmp);
  check(saved === tmp && existsSync(tmp), 'save bridge writes real files');
  if (existsSync(tmp)) unlinkSync(tmp);

  check(pageErrors.length === 0, 'no page errors' + (pageErrors.length ? ': ' + pageErrors.join(' | ') : ''));

  await browser.disconnect();
} catch (err) {
  console.error('FAIL', err.message);
  failures++;
} finally {
  stop();
}

console.log(failures ? `${failures} failure(s)` : 'electron smoke: all ok');
process.exit(failures ? 1 : 0);
