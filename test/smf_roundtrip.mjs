#!/usr/bin/env node
//
// DAW model round trips (node, no browser): project -> SMF -> project keeps
// every note/CC/bend, tempo map, time signatures and loop; golden MIDIs
// import and re-export with identical note sets; the engine's own SMF
// parser (via midisynth_bake --to-mid on a bake) agrees on event counts.
//
//   node test/smf_roundtrip.mjs
//
import { readFileSync, writeFileSync, existsSync, mkdtempSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { newProject, newTrack, normalizeProject, tickToMs, msToTick, barStarts, barBeatAt, projectEndTick,
         ProjectStore, gridTicks } from '../tool/daw/project.js';
import { projectToSmf, smfToProject } from '../tool/daw/smf.js';

const root = new URL('..', import.meta.url).pathname;
let failures = 0;
const check = (cond, what) => { if (cond) console.log('ok', what); else { console.error('FAIL', what); failures++; } };
const notesKey = (p) => p.tracks.map((t) => `${t.channel}:` + t.notes.map((n) => `${n.tick},${n.dur},${n.note},${n.vel}`).join(';')).join('|');
const ccKey = (p) => p.tracks.map((t) => `${t.channel}:` + t.ccs.map((c) => `${c.tick},${c.cc},${c.val}`).join(';') +
                                          '/' + t.bends.map((b) => `${b.tick},${b.value}`).join(';')).join('|');

// ---- hand-built project ----
let p = newProject({ bpm: 100, bars: 2 });
p.name = 'rt';
p.tempoMap.push({ tick: 960, bpm: 140 });
p.timeSigs.push({ tick: 1920, num: 3, den: 4 });
const t0 = p.tracks[0];
t0.program = 48;
t0.notes.push({ tick: 0, dur: 480, note: 60, vel: 100 }, { tick: 480, dur: 240, note: 64, vel: 90 },
              { tick: 480, dur: 240, note: 67, vel: 80 }, { tick: 720, dur: 1, note: 64, vel: 127 });
t0.ccs.push({ tick: 0, cc: 7, val: 100 }, { tick: 240, cc: 1, val: 64 });
t0.bends.push({ tick: 100, value: -8192 }, { tick: 200, value: 8191 }, { tick: 300, value: 0 });
p.tracks[1].notes.push({ tick: 0, dur: 30, note: 36, vel: 110 }, { tick: 480, dur: 30, note: 42, vel: 70 });
p.tracks.push(newTrack(3, { name: 'Lead', program: 81 }));
p.tracks[2].notes.push({ tick: 1920, dur: 960, note: 72, vel: 99 });
p.loop = { startTick: 480, endTick: 2880, enabled: true };
p = normalizeProject(p);

const smf = projectToSmf(p);
check(smf[0] === 0x4D && smf[9] === 1, 'writes a format-1 SMF');
const back = smfToProject(smf, { name: 'x' });
const detail = (ok, a, b) => (ok ? '' : `\n  ${a}\n  ${b}`);
check(notesKey(back) === notesKey(p), 'notes survive the round trip' + detail(notesKey(back) === notesKey(p), notesKey(p), notesKey(back)));
check(ccKey(back) === ccKey(p), 'CC + bends survive the round trip' + detail(ccKey(back) === ccKey(p), ccKey(p), ccKey(back)));
check(JSON.stringify(back.tempoMap) === JSON.stringify(p.tempoMap), `tempo map survives (${JSON.stringify(back.tempoMap)})`);
check(JSON.stringify(back.timeSigs) === JSON.stringify(p.timeSigs), 'time signatures survive');
check(back.loop.startTick === 480 && back.loop.endTick === 2880, 'loop markers survive');
check(back.tracks.find((t) => t.channel === 3)?.name === 'Lead' && back.tracks.find((t) => t.channel === 3)?.program === 81,
      'track name + program survive');
check(back.name === 'rt', 'song name from the conductor track');

// ---- tempo math ----
check(Math.abs(tickToMs(p, 480) - 600) < 1e-9, 'tickToMs at 100 BPM');
check(Math.abs(tickToMs(p, 1440) - (1200 + 480 * 60000 / (140 * 480))) < 1e-6, 'tickToMs across a tempo change');
check(Math.abs(msToTick(p, tickToMs(p, 2000)) - 2000) < 1e-6, 'msToTick inverts tickToMs');
const bars = barStarts(p, 5000);
check(bars[0] === 0 && bars[1] === 1920 && bars[2] === 3360, `bar starts follow the 3/4 change (${bars.slice(0, 4)})`);
const bb = barBeatAt(p, 1920 + 960 + 10);
check(bb.bar === 1 && bb.beat === 2, `bar/beat at a 3/4 position (${JSON.stringify(bb)})`);
check(gridTicks(p, '1/16') === 120 && gridTicks(p, '1/8T') === 160, 'grid ticks');
check(projectEndTick(p) === 3360 || projectEndTick(p) === 4800, `project end rounds to a bar (${projectEndTick(p)})`);

// ---- click / count-in / mutes ----
const withClick = projectToSmf(p, { click: true, offsetTicks: 1920, mutes: true });
const clickBack = smfToProject(withClick);
const drums = clickBack.tracks.find((t) => t.channel === 9);
check(drums.notes.some((n) => n.tick < 1920 && (n.note === 75 || n.note === 76)), 'count-in clicks precede the song');
check(clickBack.tracks.find((t) => t.channel === 0).notes[0].tick === 1920, 'offset shifts the music');
p.tracks[0].mute = true;
const muted = smfToProject(projectToSmf(p, { mutes: true }));
check(!muted.tracks.some((t) => t.channel === 0), 'muted track dropped from the playback SMF');
p.tracks[0].mute = false;

// ---- undo/redo store ----
const store = new ProjectStore(p);
let changes = 0;
store.addEventListener('change', () => changes++);
store.mutate('add note', (pp) => pp.tracks[0].notes.push({ tick: 3000, dur: 10, note: 50, vel: 50 }));
check(store.p.tracks[0].notes.length === 5 && changes === 1, 'mutate applies + notifies');
store.undo();
check(store.p.tracks[0].notes.length === 4, 'undo restores');
store.redo();
check(store.p.tracks[0].notes.length === 5, 'redo re-applies');
check(ProjectStore.fromJSON(store.toJSON()).tracks.length === 3, 'project JSON round trip');

// ---- golden corpus: import -> export -> import is stable ----
for (const f of ['melody.mid', 'drums.mid', 'tempos.mid', 'poly.mid']) {
  const path = join(root, 'test/golden', f);
  if (!existsSync(path)) { console.log('skip', f); continue; }
  const a = smfToProject(readFileSync(path), { name: f });
  const b = smfToProject(projectToSmf(a), { name: f });
  // Multi-track originals can pair overlapping same-pitch notes differently
  // than the merged one-track-per-channel export (SMF has no note identity),
  // so compare the note-on / note-off EVENT sets, which must be identical.
  const evKey = (p) => p.tracks.map((t) => `${t.channel}:` +
      [...t.notes.map((n) => `on${n.tick},${n.note},${n.vel}`), ...t.notes.map((n) => `off${n.tick + n.dur},${n.note}`)]
        .sort().join(';')).join('|');
  const same = evKey(a) === evKey(b) && ccKey(a) === ccKey(b);
  check(same, `${f}: import/export/import stable ` +
        `(${a.tracks.reduce((n, t) => n + t.notes.length, 0)} notes, ${a.tracks.length} tracks)`);
  if (!same) {
    for (let i = 0; i < a.tracks.length; i++) {
      const ka = notesKey({ tracks: [a.tracks[i]] }), kb = notesKey({ tracks: [b.tracks[i] || a.tracks[i]] });
      if (ka !== kb) {
        const na = ka.split(';'), nb = kb.split(';');
        const j = na.findIndex((x, k) => x !== nb[k]);
        console.error(`  ch ${a.tracks[i].channel}: ${na.length} vs ${nb.length} notes; first diff #${j}: ${na[j]} vs ${nb[j]}`);
        break;
      }
      const ca = ccKey({ tracks: [a.tracks[i]] }), cb = ccKey({ tracks: [b.tracks[i] || a.tracks[i]] });
      if (ca !== cb) { console.error(`  ch ${a.tracks[i].channel}: cc/bend differ (${ca.length} vs ${cb.length} chars)`); break; }
    }
  }
}

// ---- cross-check with the engine's parser through midisynth_bake --to-mid ----
const bake = join(root, 'build/midisynth_bake');
if (existsSync(bake)) {
  const dir = mkdtempSync(join(tmpdir(), 'msproj-'));
  const midPath = join(dir, 'rt.mid'), mstk = join(dir, 'rt.mstk'), back2 = join(dir, 'rt_back.mid');
  writeFileSync(midPath, projectToSmf(p));
  execFileSync(bake, [midPath, '-o', mstk, '--bank', join(root, 'content/patches/gm_bank.json'),
                      '--drums', join(root, 'content/drums/gm_kit.json')], { stdio: 'pipe' });
  execFileSync(bake, ['--to-mid', mstk, back2], { stdio: 'pipe' });
  const viaEngine = smfToProject(readFileSync(back2));
  check(notesKey(viaEngine) === notesKey(p), 'engine bake -> --to-mid -> import reproduces the notes' + detail(notesKey(viaEngine) === notesKey(p), notesKey(p), notesKey(viaEngine)));
  check(viaEngine.loop.startTick === 480 && viaEngine.loop.endTick === 2880, 'engine keeps the loop markers');
} else {
  console.log('skip: build/midisynth_bake not built');
}

// ---- groove -------------------------------------------------------------
// Swing is an export-time transform: the model never moves, the SMF does.
{
  const p = normalizeProject(newProject({ bpm: 174 }));
  const grid = 480 / 4;                      // 1/16 at ppq 480
  p.tracks[0].notes = [0, 1, 2, 3].map((i) => ({ tick: i * grid, dur: grid, note: 60, vel: 100 }));

  const straight = smfToProject(projectToSmf(p), { name: 'x' });
  check(straight.tracks[0].notes.map((n) => n.tick).join() === [0, 120, 240, 360].join(),
        'no groove leaves every note on the grid');

  p.groove = { amount: 1, grid: '1/16' };
  const swung = smfToProject(projectToSmf(p), { name: 'x' });
  const ticks = swung.tracks[0].notes.map((n) => n.tick);
  // Full swing pushes every second sixteenth a third of a grid step late.
  check(ticks.join() === [0, 160, 240, 400].join(), `full swing shuffles the off-beats (${ticks})`);
  check(p.tracks[0].notes.map((n) => n.tick).join() === [0, 120, 240, 360].join(),
        'the project model itself is untouched');

  // Half the amount is half the shift, and a note keeps its length.
  p.groove = { amount: 0.5, grid: '1/16' };
  const half = smfToProject(projectToSmf(p), { name: 'x' });
  check(half.tracks[0].notes[1].tick === 140, 'swing amount scales the shift');
  check(half.tracks[0].notes[1].dur === grid, 'a swung note keeps its length');

  // Exporting an already-exported project must not swing twice.
  const twice = smfToProject(projectToSmf(normalizeProject(swung)), { name: 'x' });
  check(twice.tracks[0].notes.map((n) => n.tick).join() === [0, 160, 240, 400].join(),
        'swing is not applied twice on a re-export');
}

process.exit(failures ? 1 : 0);
