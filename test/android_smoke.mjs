#!/usr/bin/env node
//
// Smoke test for the Android build, driven through the debug WebView's
// DevTools endpoint over adb (same idea as test/electron_smoke.mjs).
//
// Proves the things a build alone does not: the payload really is served
// from https://localhost (secure context), the AudioWorklet + WASM engine
// runs inside the WebView with the clock advancing, saving goes to the
// filesystem (a WebView has no download manager), and the phone layout is
// actually applied.
//
//   ./scripts/package_android.sh      # install: adb install -r <apk>
//   node test/android_smoke.mjs       # from a dir with puppeteer-core
//
import { createRequire } from 'node:module';
import { execFileSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import path from 'node:path';

const APP = 'com.digidreamsynth.daw';
const ADB = [process.env.ANDROID_HOME, process.env.ANDROID_SDK_ROOT, `${process.env.HOME}/Library/Android/sdk`]
  .filter(Boolean).map((r) => path.join(r, 'platform-tools', 'adb')).find((p) => existsSync(p));

let puppeteer;
try {
  const require = createRequire(process.cwd() + '/package.json');
  puppeteer = require('puppeteer-core');
} catch {
  try { puppeteer = (await import('puppeteer-core')).default; }
  catch { console.log('skip: puppeteer-core not installed (run from a dir that has it)'); process.exit(0); }
}
if (!ADB) { console.log('skip: adb not found (set ANDROID_HOME)'); process.exit(0); }

const adb = (...a) => execFileSync(ADB, a, { encoding: 'utf8' }).trim();
// pidof exits non-zero until the process exists; that is a poll, not an error.
const adbTry = (...a) => { try { return adb(...a); } catch { return ''; } };
if (!adb('devices').split('\n').slice(1).some((l) => l.endsWith('device'))) {
  console.log('skip: no android device/emulator attached');
  process.exit(0);
}
if (!adb('shell', 'pm', 'list', 'packages', APP).includes(APP)) {
  console.log(`skip: ${APP} not installed (adb install -r mobile/android/.../app-debug.apk)`);
  process.exit(0);
}

let failures = 0;
const check = (cond, what) => { if (cond) console.log('ok', what); else { console.error('FAIL', what); failures++; } };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

adb('shell', 'am', 'force-stop', APP);
adb('shell', 'am', 'start', '-n', `${APP}/.MainActivity`);

let browser = null;
try {
  let pid = '';
  for (let i = 0; i < 40 && !pid; i++) { await sleep(500); pid = adbTry('shell', 'pidof', APP).split(/\s+/)[0] || ''; }
  check(!!pid, 'app launches');
  adb('forward', 'tcp:9222', `localabstract:webview_devtools_remote_${pid}`);

  for (let i = 0; i < 40 && !browser; i++) {
    await sleep(500);
    try { browser = await puppeteer.connect({ browserURL: 'http://127.0.0.1:9222', defaultViewport: null }); } catch {}
  }
  if (!browser) throw new Error('WebView devtools never came up');

  let page = null;
  for (let i = 0; i < 40 && !page; i++) {
    page = (await browser.pages()).find((p) => p.url().startsWith('https://localhost')) || null;
    if (!page) await sleep(500);
  }
  check(!!page, 'DAW loads from https://localhost (secure context)');
  if (!page) throw new Error('no page');

  const errors = [];
  page.on('pageerror', (e) => errors.push(e.message));
  // Resource failures come through the response listener below, with a URL;
  // the console version of the same event carries no URL to judge by.
  page.on('console', (m) => {
    if (m.type() === 'error' && !/Failed to load resource/i.test(m.text())) errors.push('console: ' + m.text());
  });
  page.on('response', (r) => {
    if (r.status() >= 400 && !/favicon/i.test(r.url())) errors.push(`${r.status()} ${r.url()}`);
  });

  await page.waitForSelector('#start-btn', { timeout: 20000 });
  await page.click('#start-btn');
  await page.waitForFunction(() => window.__daw && !document.getElementById('overlay'), { timeout: 90000 });
  check(true, 'engine boots in the WebView (worklet + wasm)');

  // The real test of an AudioWorklet on a phone: does its clock advance?
  await page.evaluate(() => window.__daw.store.mutate('notes', (p) => {
    p.tracks[0].notes = [0, 1, 2, 3].map((i) => ({ tick: i * 480, dur: 440, note: 60 + i * 3, vel: 100 }));
  }));
  // The edit above schedules a DEBOUNCED reload, and a reload landing after
  // play() cancels it. Force the reload through first, then play.
  await page.evaluate(() => window.__daw.playback.reload({ keepPosition: false }));
  await page.evaluate(() => window.__daw.playback.play());
  let t1 = 0;
  for (let i = 0; i < 40 && t1 <= 50; i++) {
    await sleep(500);
    t1 = await page.evaluate(() => window.__daw.playback.msNow());
  }
  await sleep(600);
  const t2 = await page.evaluate(() => window.__daw.playback.msNow());
  // On failure, say which half is dead: the audio device (ctx.currentTime
  // frozen) or the worklet's status feed.
  const why = t1 > 50 ? '' : ' ' + JSON.stringify(await page.evaluate(() => ({
    ctx: window.__daw.client.ctx.state,
    ctxTime: +window.__daw.client.ctx.currentTime.toFixed(2),
    status: !!window.__daw.playback.status(),
    playing: !!window.__daw.playback.status()?.playing,
  })));
  check(t1 > 50 && t2 > t1 + 200, `transport clock runs (${t1 | 0}ms -> ${t2 | 0}ms)${why}`);
  await page.evaluate(() => window.__daw.playback.stop());

  // Saving: no download manager in a WebView, so this must reach the
  // Filesystem plugin and come back readable.
  const saved = await page.evaluate(async () => {
    const m = await import('./lib/desktop.js');
    const r = await m.saveBytes('smoke.json', JSON.stringify({ ok: 1 }), 'application/json');
    const back = await window.Capacitor.Plugins.Filesystem.readFile({ path: 'smoke.json', directory: 'DOCUMENTS' });
    await window.Capacitor.Plugins.Filesystem.deleteFile({ path: 'smoke.json', directory: 'DOCUMENTS' });
    return { uri: r?.path || '', text: atob(back.data) };
  }).catch((e) => ({ uri: '', text: 'threw: ' + e.message }));
  check(saved.uri.startsWith('file://') && saved.text === '{"ok":1}', `save writes a real file (${saved.uri || saved.text})`);

  // Phone layout, not the desktop one squeezed onto a phone.
  const layout = await page.evaluate(() => ({
    title: getComputedStyle(document.querySelector('header h1')).display,
    tabs: getComputedStyle(document.getElementById('tabs')).flexWrap,
    overflow: document.documentElement.scrollWidth - document.documentElement.clientWidth,
  }));
  check(layout.title === 'none' && layout.tabs === 'nowrap' && layout.overflow === 0,
        `mobile layout applied (${JSON.stringify(layout)})`);

  check(errors.length === 0, 'no page errors' + (errors.length ? ': ' + errors.join(' | ') : ''));
  await browser.disconnect();
} catch (err) {
  console.error('FAIL', err.message);
  failures++;
} finally {
  try { adb('forward', '--remove', 'tcp:9222'); } catch {}
  try { adb('shell', 'am', 'force-stop', APP); } catch {}
}

console.log(failures ? `${failures} failure(s)` : 'android smoke: all ok');
process.exit(failures ? 1 : 0);
