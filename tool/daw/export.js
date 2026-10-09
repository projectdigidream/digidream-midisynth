//
// File menu: project save/open, imports (.mid/.mstk, bank/kit/fx/master
// JSON), exports (.mid, baked .mstk, .wav render, slices, cues.json, the
// standalone JSON documents).
//
import { download, isMstk } from '../lib/ui.js';
import { pickFiles, saveAllBytes } from '../lib/desktop.js';
import { ProjectStore, normalizeProject } from './project.js';
import { projectToSmf, smfToProject } from './smf.js';

const pickFile = async (accept) => (await pickFiles(accept, false))[0] || null;
const safe = (s) => (s || 'untitled').replace(/[^\w.-]+/g, '_');

// Desktop: Save writes over the open file and Save As always asks. Browser:
// both are the same download (there is no path to write back to).
export async function saveProject(ctx, { saveAs = false } = {}) {
  const { store } = ctx;
  store.p.name = document.getElementById('proj-name')?.value || store.p.name;
  const name = safe(store.p.name) + '.msproj';
  const r = await download(name, store.toJSON(), 'application/json',
                           { toPath: saveAs ? null : ctx.projectPath });
  if (!r) { ctx.log('save cancelled', 'warn'); return; }
  ctx.setProjectPath?.(r.path);
  store.dirty = false;
  ctx.log(`saved ${r.path || name}`, 'ok');
}

export async function openProject(ctx, file = null) {
  file = file || await pickFile('.msproj,.json');
  if (!file) return;
  try {
    const p = ProjectStore.fromJSON(await file.text());
    ctx.loadProject(p);
    ctx.setProjectPath?.(file.path || null);
    ctx.log(`opened ${file.path || file.name}`, 'ok');
  } catch (err) {
    ctx.log('open failed: ' + err.message, 'err');
  }
}

// .mid -> project (replaces tracks/tempo/loop, keeps bank/kit/fx/master);
// .mstk -> engine exports its SONG as SMF, then the same path; the track's
// embedded bank/kit/fx/master come along.
export async function importSong(ctx, file = null) {
  file = file || await pickFile('.mid,.midi,.mstk');
  if (!file) return;
  const { client, log } = ctx;
  try {
    const bytes = new Uint8Array(await file.arrayBuffer());
    let smf = bytes, extra = null;
    if (isMstk(bytes)) {
      await client.loadTrack(bytes);
      smf = new Uint8Array(await client.exportSmf(file.name));
      extra = await client.trackContent();
      log(`imported .mstk '${extra.name || file.name}' — song, bank, kit, fx and mastering extracted through the engine`, 'ok');
    }
    const imported = smfToProject(smf, { name: file.name });
    const cur = ctx.store.p;
    const merged = normalizeProject({
      ...cur,
      name: imported.name,
      ppq: imported.ppq,
      tempoMap: imported.tempoMap,
      timeSigs: imported.timeSigs,
      tracks: imported.tracks,
      loop: imported.loop,
    });
    if (extra?.bankJson) merged.bank = JSON.parse(extra.bankJson);
    if (extra?.kitJson) merged.kit = JSON.parse(extra.kitJson);
    if (extra?.fx) {
      merged.fx = { version: 1, reverb: extra.fx.reverb, chorus: extra.fx.chorus };
      merged.master = normalizeProject({ ...merged, master: extra.fx.master }).master;
    }
    ctx.loadProject(merged, { keepContent: !extra?.bankJson });
    log(`imported ${file.name}: ${imported.tracks.length} tracks, ${imported.tracks.reduce((n, t) => n + t.notes.length, 0)} notes`, 'ok');
  } catch (err) {
    log('import failed: ' + err.message, 'err');
  }
}

export async function importJson(ctx, kind, file = null) {
  file = file || await pickFile('.json');
  if (!file) return;
  const { store, log } = ctx;
  let doc;
  try { doc = JSON.parse(await file.text()); }
  catch (err) { log('import failed: ' + err.message, 'err'); return; }
  switch (kind) {
    case 'bank':
      if (!doc.programs && !doc.fallback) { log('not a bank file', 'err'); return; }
      store.mutate('import bank', (pp) => { pp.bank = doc; }, { audio: false, content: true });
      break;
    case 'kit':
      if (!doc.notes) { log('not a kit file', 'err'); return; }
      store.mutate('import kit', (pp) => { pp.kit = doc; }, { audio: false, content: true });
      break;
    case 'fx':
      store.mutate('import fx', (pp) => {
        pp.fx = { version: 1, reverb: { ...pp.fx.reverb, ...(doc.reverb || {}) }, chorus: { ...pp.fx.chorus, ...(doc.chorus || {}) } };
        if (doc.master) pp.master = { ...pp.master, ...doc.master };
      }, { audio: false, fx: true });
      break;
    case 'master': {
      const m = doc.master || doc;
      if (!m || typeof m !== 'object') { log('not a master file', 'err'); return; }
      store.mutate('import master', (pp) => { pp.master = normalizeProject({ ...pp, master: m }).master; }, { audio: false, fx: true });
      break;
    }
    default: return;
  }
  log(`imported ${kind}: ${file.name}`, 'ok');
}

export function exportMid(ctx) {
  const p = ctx.store.p;
  download(safe(p.name) + '.mid', projectToSmf(p), 'audio/midi');
  ctx.log(`exported ${safe(p.name)}.mid`, 'ok');
}

// Bake the whole project (bank/kit/fx/master embedded) to .mstk bytes.
export async function bakeProject(ctx, { verifySeconds = 0, name = null } = {}) {
  const { store, client, log } = ctx;
  const p = store.p;
  ctx.editor?.applyNow();
  // The bake runs inside the audio-thread worklet with no progress feed:
  // show an indeterminate bar so long bakes are visibly alive.
  ctx.progress?.begin(`baking ${name || safe(p.name)}.mstk`, { indeterminate: true });
  try {
    const bytes = await client.bakeTrack({
      smfBytes: projectToSmf(p),
      bankJson: ctx.bankJson(),
      kitJson: ctx.kitJson(),
      fxJson: JSON.stringify(ctx.fxDoc()),
      name: name || safe(p.name),
      verifySeconds,
    });
    return bytes;
  } catch (err) {
    log('[bake] ' + err.message, 'err');
    return null;
  } finally {
    ctx.progress?.done();
  }
}
export async function exportMstk(ctx) {
  const bytes = await bakeProject(ctx);
  if (!bytes) return;
  const r = await download(safe(ctx.store.p.name) + '.mstk', bytes);
  if (!r) { ctx.log('bake discarded (save cancelled)', 'warn'); return; }
  ctx.log(`baked ${r.path || r.name} (${(bytes.byteLength / 1024).toFixed(1)} KB, mastering ${ctx.store.p.master.enabled ? 'on' : 'off'})`, 'ok');
}

export function renderWav(ctx, { seconds = 0 } = {}) {
  const { store, log } = ctx;
  const p = store.p;
  ctx.editor?.applyNow();
  const worker = new Worker(new URL('../render-worker.js', import.meta.url), { type: 'module' });
  log('rendering .wav …');
  ctx.progress?.begin(`rendering ${safe(p.name)}.wav`, {
    cancel: () => { worker.terminate(); log('render cancelled', 'warn'); },
  });
  worker.onmessage = (e) => {
    const m = e.data;
    if (m.type === 'progress') { ctx.progress?.update(m.pct); if (m.pct % 20 === 0) log(`render ${m.pct}%`); }
    else if (m.type === 'reports') { for (const [what, r] of m.reports) for (const w of r.warnings || []) log(`[render:${what}] ${w}`, 'warn'); }
    else if (m.type === 'done') {
      download(safe(p.name) + '.wav', m.wav, 'audio/wav');
      log(`render done (${(m.frames / 48000).toFixed(1)}s, peak ${m.peakVoices} voices${m.drops ? `, ${m.drops} drops!` : ''})`, 'ok');
      worker.terminate();
      ctx.progress?.done();
    } else if (m.type === 'error') { log('render failed: ' + m.error, 'err'); worker.terminate(); ctx.progress?.done(); }
  };
  const buf = projectToSmf(p, { mutes: true }).buffer;
  worker.postMessage({
    bankJson: ctx.bankJson(), kitJson: ctx.kitJson(), songBytes: buf, seconds,
    dither: !!p.view.dither, loop: false,
    fxJson: JSON.stringify(ctx.fxDoc()), masterJson: JSON.stringify({ version: 1, master: p.master }),
  }, [buf]);
}

export async function exportSlices(ctx) {
  const slices = ctx.cutter?.slices || [];
  if (!slices.length) { ctx.log('no slices to export — cut some in Cut & Loop', 'warn'); return; }
  const r = await saveAllBytes(slices.map((s) => ({ name: s.name + '.mstk', data: s.bytes })));
  if (!r) { ctx.log('slice export cancelled', 'warn'); return; }
  ctx.log(`exported ${slices.length} slice(s)${r.dir ? ' to ' + r.dir : ''}`, 'ok');
}

export function exportJson(ctx, kind) {
  const p = ctx.store.p;
  ctx.editor?.applyNow();
  switch (kind) {
    case 'bank': download('bank.json', ctx.bankJson(true), 'application/json'); break;
    case 'kit': download('kit.json', ctx.kitJson(true), 'application/json'); break;
    case 'fx': download('fx.json', JSON.stringify(ctx.fxDoc(), null, 2), 'application/json'); break;
    case 'master': download('master.json', JSON.stringify({ version: 1, master: p.master }, null, 2), 'application/json'); break;
    case 'cues': download(safe(p.name) + '.cues.json', ctx.cues.exportCuesJson(), 'application/json'); break;
    default: return;
  }
  ctx.log(`exported ${kind}.json`, 'ok');
}
