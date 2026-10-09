//
// .msproj project model: tracks (1 track = 1 MIDI channel), tempo map, time
// signatures, loop region, embedded bank/kit/fx/master, slices and cue rules.
// Plus undo/redo and the tick <-> ms math the UI needs (the engine's baked
// sample positions stay the ground truth for playback; this is for display,
// snapping and recording).
//
export const PROJECT_VERSION = 1;
export const TRACK_COLORS = ['#64b5f6', '#e0b060', '#7bc67b', '#d678d6', '#f28b82', '#80cbc4',
                             '#ffd54f', '#b39ddb', '#a5d6a7', '#ffab91', '#90caf9', '#ce93d8',
                             '#c5e1a5', '#ffe082', '#80deea', '#ef9a9a'];

export function defaultFx() {
  return {
    version: 1,
    reverb: { roomSize: 0.8, damping: 0.4, width: 1.0, wetGain: 0.3 },
    chorus: { rateHz: 0.8, depthMs: 3.5, wetGain: 0.2 },
    delay: { timeMs: 320, feedback: 0.35, wetGain: 0, pingPong: false },
  };
}
export function defaultMaster() {
  return {
    enabled: false,
    eq: { low: { hz: 120, db: 0 }, mid: { hz: 1000, db: 0, q: 1 }, high: { hz: 8000, db: 0 } },
    comp: { thresholdDb: -12, ratio: 2, attackMs: 10, releaseMs: 150, makeupDb: 0, kneeDb: 6 },
    limiter: { ceilingDb: -1, lookaheadBlocks: 2, releaseMs: 80 },
    satDrive: 1,
    width: 1.0,
    gainDb: 0,
  };
}

export function newTrack(channel, { name = null, program = 0, color = null } = {}) {
  return {
    name: name || (channel === 9 ? 'Drums' : `Track ${channel + 1}`),
    channel,
    program: channel === 9 ? 0 : program,
    color: color || TRACK_COLORS[channel % TRACK_COLORS.length],
    mute: false,
    solo: false,
    arm: false,
    notes: [],   // {tick, dur, note, vel}
    ccs: [],     // {tick, cc, val}
    bends: [],   // {tick, value}  value -8192..8191
  };
}

export function newProject({ name = 'untitled', bpm = 120, bars = 4 } = {}) {
  return {
    version: PROJECT_VERSION,
    name,
    ppq: 480,
    tempoMap: [{ tick: 0, bpm }],
    timeSigs: [{ tick: 0, num: 4, den: 4 }],
    tracks: [newTrack(0, { program: 0 }), newTrack(9)],
    loop: { startTick: 0, endTick: bars * 4 * 480, enabled: true },
    // Swing is applied when the project is serialised to SMF, never to the
    // model: the notes on the grid stay where you put them, and the shuffle
    // rides along into the .mstk because the bake consumes that SMF.
    groove: { amount: 0, grid: '1/16' },
    bank: null,      // patch bank document (null = engine default GM bank)
    kit: null,       // drum kit document
    fx: defaultFx(),
    master: defaultMaster(),
    slices: [],      // {name, startTick, endTick}
    cues: { entry: null, transitions: {} },  // name -> {then, after, mode}
    view: {},
  };
}

// ---------------- validation / migration ----------------
export function normalizeProject(p) {
  const out = { ...newProject(), ...p };
  out.version = PROJECT_VERSION;
  out.ppq = Math.max(24, Math.min(960, +out.ppq || 480));
  if (!Array.isArray(out.tempoMap) || !out.tempoMap.length) out.tempoMap = [{ tick: 0, bpm: 120 }];
  out.tempoMap = out.tempoMap.map((t) => ({ tick: Math.max(0, t.tick | 0), bpm: clampBpm(+t.bpm || 120) }))
                             .sort((a, b) => a.tick - b.tick);
  if (out.tempoMap[0].tick !== 0) out.tempoMap.unshift({ tick: 0, bpm: out.tempoMap[0].bpm });
  if (!Array.isArray(out.timeSigs) || !out.timeSigs.length) out.timeSigs = [{ tick: 0, num: 4, den: 4 }];
  out.timeSigs = out.timeSigs.map((t) => ({ tick: Math.max(0, t.tick | 0), num: clampInt(t.num, 1, 32, 4),
                                            den: [1, 2, 4, 8, 16, 32].includes(+t.den) ? +t.den : 4 }))
                             .sort((a, b) => a.tick - b.tick);
  if (out.timeSigs[0].tick !== 0) out.timeSigs.unshift({ tick: 0, num: 4, den: 4 });
  const g = out.groove || {};
  out.groove = {
    amount: Math.max(0, Math.min(1, +g.amount || 0)),
    grid: /^1\/(8|16|32)$/.test(g.grid) ? g.grid : '1/16',
  };
  const seen = new Set();
  out.tracks = (Array.isArray(out.tracks) ? out.tracks : []).map((t, i) => {
    const ch = clampInt(t.channel, 0, 15, i % 16);
    const tr = { ...newTrack(ch), ...t, channel: ch };
    tr.program = ch === 9 ? 0 : clampInt(tr.program, 0, 127, 0);
    tr.notes = (tr.notes || []).map((n) => ({ tick: Math.max(0, n.tick | 0), dur: Math.max(1, n.dur | 0),
                                              note: clampInt(n.note, 0, 127, 60), vel: clampInt(n.vel, 1, 127, 100) }));
    tr.ccs = (tr.ccs || []).map((c) => ({ tick: Math.max(0, c.tick | 0), cc: clampInt(c.cc, 0, 127, 7), val: clampInt(c.val, 0, 127, 0) }));
    tr.bends = (tr.bends || []).map((b) => ({ tick: Math.max(0, b.tick | 0), value: clampInt(b.value, -8192, 8191, 0) }));
    sortEvents(tr);
    return tr;
  }).filter((t) => { if (seen.has(t.channel)) return false; seen.add(t.channel); return true; })
    .sort((a, b) => a.channel - b.channel);  // canonical order: 1 track = 1 channel
  out.loop = { startTick: Math.max(0, out.loop?.startTick | 0), endTick: Math.max(1, out.loop?.endTick | 0),
               enabled: out.loop?.enabled !== false };
  if (out.loop.endTick <= out.loop.startTick) out.loop.endTick = out.loop.startTick + barTicksAt(out, out.loop.startTick);
  out.fx = { ...defaultFx(), ...(out.fx || {}) };
  out.master = mergeDeep(defaultMaster(), out.master || {});
  out.slices = (out.slices || []).filter((s) => s && s.endTick > s.startTick)
      .map((s) => ({ name: String(s.name || 'slice'), startTick: s.startTick | 0, endTick: s.endTick | 0 }));
  out.cues = { entry: out.cues?.entry ?? null, transitions: out.cues?.transitions || {} };
  out.view = out.view || {};
  return out;
}
const clampBpm = (b) => Math.max(20, Math.min(400, b));
const clampInt = (v, lo, hi, def) => (Number.isFinite(+v) ? Math.max(lo, Math.min(hi, Math.round(+v))) : def);
function mergeDeep(base, over) {
  const out = { ...base };
  for (const k of Object.keys(over || {})) {
    if (over[k] && typeof over[k] === 'object' && !Array.isArray(over[k]) && base[k] && typeof base[k] === 'object')
      out[k] = mergeDeep(base[k], over[k]);
    else out[k] = over[k];
  }
  return out;
}
export function sortEvents(tr) {
  // Full key so two imports of the same music compare equal regardless of
  // MIDI-track interleaving.
  tr.notes.sort((a, b) => a.tick - b.tick || a.note - b.note || a.dur - b.dur || a.vel - b.vel);
  tr.ccs.sort((a, b) => a.tick - b.tick || a.cc - b.cc);
  tr.bends.sort((a, b) => a.tick - b.tick);
}

// ---------------- tempo / meter math ----------------
export function msPerTick(bpm, ppq) { return 60000 / (bpm * ppq); }
export function tickToMs(p, tick) {
  let ms = 0;
  const tm = p.tempoMap;
  for (let i = 0; i < tm.length; i++) {
    const segStart = tm[i].tick;
    const segEnd = i + 1 < tm.length ? tm[i + 1].tick : Infinity;
    if (tick <= segStart) break;
    const t1 = Math.min(tick, segEnd);
    ms += (t1 - segStart) * msPerTick(tm[i].bpm, p.ppq);
    if (tick <= segEnd) break;
  }
  return ms;
}
export function msToTick(p, ms) {
  let acc = 0;
  const tm = p.tempoMap;
  for (let i = 0; i < tm.length; i++) {
    const segStart = tm[i].tick;
    const segEnd = i + 1 < tm.length ? tm[i + 1].tick : Infinity;
    const mpt = msPerTick(tm[i].bpm, p.ppq);
    const segMs = (segEnd - segStart) * mpt;
    if (ms <= acc + segMs || segEnd === Infinity) return segStart + (ms - acc) / mpt;
    acc += segMs;
  }
  return 0;
}
export function bpmAt(p, tick) {
  let bpm = p.tempoMap[0].bpm;
  for (const t of p.tempoMap) if (t.tick <= tick) bpm = t.bpm;
  return bpm;
}
export function timeSigAt(p, tick) {
  let ts = p.timeSigs[0];
  for (const t of p.timeSigs) if (t.tick <= tick) ts = t;
  return ts;
}
export function barTicksAt(p, tick) {
  const ts = timeSigAt(p, tick);
  return Math.round((ts.num * 4 * p.ppq) / ts.den);
}
export function beatTicksAt(p, tick) {
  const ts = timeSigAt(p, tick);
  return Math.round((4 * p.ppq) / ts.den);
}
// Bar start ticks up to (and including one past) `endTick`.
export function barStarts(p, endTick) {
  const bars = [];
  let tick = 0;
  let guard = 0;
  while (tick <= endTick && guard++ < 100000) {
    bars.push(tick);
    // Advance one bar; a time signature change inside the bar re-anchors it.
    const next = tick + barTicksAt(p, tick);
    const ts = p.timeSigs.find((s) => s.tick > tick && s.tick < next);
    tick = ts ? ts.tick : next;
  }
  return bars;
}
export function barBeatAt(p, tick) {
  const bars = barStarts(p, tick);
  const bar = bars.length - 1;
  const inBar = tick - bars[bar];
  const bt = beatTicksAt(p, tick);
  return { bar, beat: Math.floor(inBar / bt), ticks: inBar % bt };
}
// Musical end of the project: last note end rounded up to a bar, at least 1 bar
// and never before the loop end.
export function contentEndTick(p) {
  let end = 0;
  for (const t of p.tracks) {
    for (const n of t.notes) end = Math.max(end, n.tick + n.dur);
    for (const c of t.ccs) end = Math.max(end, c.tick);
    for (const b of t.bends) end = Math.max(end, b.tick);
  }
  return end;
}
export function projectEndTick(p) {
  const end = Math.max(contentEndTick(p), p.loop.endTick, 1);
  const bars = barStarts(p, end);
  const last = bars[bars.length - 1];
  return last >= end && last > 0 ? last : last + barTicksAt(p, last);
}
export function snapTick(p, tick, div /* ticks per grid step */) {
  if (!div) return Math.round(tick);
  return Math.round(tick / div) * div;
}
// Grid divisions: '1/4', '1/8T', ... -> ticks.
export function gridTicks(p, spec) {
  const m = /^1\/(\d+)(T?)$/.exec(spec);
  if (!m) return p.ppq;
  const den = +m[1];
  let t = (4 * p.ppq) / den;
  if (m[2]) t = (t * 2) / 3;
  return Math.max(1, Math.round(t));
}

// Swing: delay every second subdivision of the groove grid. At amount 1 the
// pair lands 2:1, i.e. the triplet feel; at 0 nothing moves. Only note starts
// swing -- CC and bend stay put, and a note's length rides with its start.
export function swingTicks(p, tick) {
  const amount = p.groove?.amount || 0;
  if (amount <= 0) return tick;
  const grid = gridTicks(p, p.groove.grid || '1/16');
  if (!grid) return tick;
  const index = Math.floor(tick / grid);
  if (index % 2 === 0) return tick;           // on-beat subdivisions stay
  return tick + Math.round((grid / 3) * amount);
}

// ---------------- store with undo/redo ----------------
export class ProjectStore extends EventTarget {
  constructor(project = newProject()) {
    super();
    this.p = normalizeProject(project);
    this.undoStack = [];
    this.redoStack = [];
    this.dirty = false;
  }
  // fn mutates this.p in place; label for the undo history. opts.audio=false
  // means the change does not affect the playback SMF (e.g. master params).
  mutate(label, fn, opts = {}) {
    if (!opts.coalesce || this._lastLabel !== label) {
      this.undoStack.push({ label, json: JSON.stringify(this.p) });
      if (this.undoStack.length > 200) this.undoStack.shift();
      this.redoStack.length = 0;
    }
    this._lastLabel = opts.coalesce ? label : null;
    fn(this.p);
    this.dirty = true;
    this.dispatchEvent(new CustomEvent('change', { detail: { label, audio: opts.audio !== false, ...opts } }));
  }
  undo() {
    const u = this.undoStack.pop();
    if (!u) return false;
    this.redoStack.push({ label: u.label, json: JSON.stringify(this.p) });
    this.p = normalizeProject(JSON.parse(u.json));
    this._lastLabel = null;
    this.dispatchEvent(new CustomEvent('change', { detail: { label: 'undo ' + u.label, audio: true, structural: true } }));
    return true;
  }
  redo() {
    const r = this.redoStack.pop();
    if (!r) return false;
    this.undoStack.push({ label: r.label, json: JSON.stringify(this.p) });
    this.p = normalizeProject(JSON.parse(r.json));
    this._lastLabel = null;
    this.dispatchEvent(new CustomEvent('change', { detail: { label: 'redo ' + r.label, audio: true, structural: true } }));
    return true;
  }
  replace(project, label = 'load') {
    this.p = normalizeProject(project);
    this.undoStack.length = 0;
    this.redoStack.length = 0;
    this.dirty = false;
    this.dispatchEvent(new CustomEvent('change', { detail: { label, audio: true, structural: true, loaded: true } }));
  }
  toJSON() { return JSON.stringify(this.p, null, 1); }
  static fromJSON(text) {
    const doc = JSON.parse(text);
    if (!doc || typeof doc !== 'object' || !Array.isArray(doc.tracks)) throw new Error('not a .msproj project');
    return normalizeProject(doc);
  }
  track(channel) { return this.p.tracks.find((t) => t.channel === channel) || null; }
}
