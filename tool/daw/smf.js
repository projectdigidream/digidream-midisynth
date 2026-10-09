//
// Standard MIDI File <-> project. Writer: format 1, track 0 = conductor
// (tempo map, time signatures, loop markers), track ch+1 = channel ch.
// Reader: format 0/1, merges every MIDI track into one project track per
// channel (1 track = 1 channel), pairs note-ons with note-offs (FIFO per
// channel/note, vel-0 note-on = off), keeps CC/bend, tempo map, time
// signatures, loopStart/loopEnd markers and track names.
//
import { newTrack, normalizeProject, projectEndTick, barStarts, beatTicksAt, barTicksAt, swingTicks } from './project.js';

// ---------------- write ----------------
function vlq(out, v) {
  const buf = [v & 0x7F];
  while ((v >>= 7) > 0) buf.push(0x80 | (v & 0x7F));
  for (let i = buf.length - 1; i >= 0; i--) out.push(buf[i]);
}
function be32(out, v) { out.push((v >>> 24) & 255, (v >>> 16) & 255, (v >>> 8) & 255, v & 255); }
function be16(out, v) { out.push((v >>> 8) & 255, v & 255); }
function strBytes(s) { return [...new TextEncoder().encode(s)]; }

function trackChunk(events, eotTick) {
  // events: [{tick, order, bytes}] ; stable sort by tick then order.
  events.sort((a, b) => a.tick - b.tick || a.order - b.order);
  const out = [];
  let tick = 0;
  for (const e of events) {
    vlq(out, Math.max(0, e.tick - tick));
    out.push(...e.bytes);
    tick = Math.max(tick, e.tick);
  }
  vlq(out, Math.max(0, eotTick - tick));
  out.push(0xFF, 0x2F, 0x00);
  return out;
}

// Options:
//   click:        add a metronome track on channel 10 (playback only)
//   offsetTicks:  shift every project event later by N ticks (count-in)
//   loopMarkers:  write loopStart/loopEnd metas (default: project.loop.enabled)
//   mutes:        honour track mute/solo (playback) — muted tracks are dropped
//   endTick:      override the song end
export function projectToSmf(p, opts = {}) {
  const off = opts.offsetTicks | 0;
  const anySolo = p.tracks.some((t) => t.solo);
  const audible = (t) => !opts.mutes || (!t.mute && (!anySolo || t.solo));
  const end = (opts.endTick ?? projectEndTick(p)) + off;
  const chunks = [];

  // Conductor.
  const cond = [];
  cond.push({ tick: 0, order: -10, bytes: [0xFF, 0x03, ...lenPrefix(strBytes(p.name || 'song'))] });
  for (const t of p.tempoMap) {
    const us = Math.max(1, Math.min(0xFFFFFF, Math.round(60000000 / t.bpm)));
    cond.push({ tick: t.tick + (t.tick > 0 ? off : 0), order: -3,
                bytes: [0xFF, 0x51, 0x03, (us >> 16) & 255, (us >> 8) & 255, us & 255] });
  }
  for (const s of p.timeSigs) {
    const den = Math.round(Math.log2(s.den));
    cond.push({ tick: s.tick + (s.tick > 0 ? off : 0), order: -2, bytes: [0xFF, 0x58, 0x04, s.num, den, 24, 8] });
  }
  const loopMarkers = opts.loopMarkers ?? p.loop.enabled;
  if (loopMarkers) {
    cond.push({ tick: p.loop.startTick + off, order: -1, bytes: [0xFF, 0x06, ...lenPrefix(strBytes('loopStart'))] });
    cond.push({ tick: p.loop.endTick + off, order: -1, bytes: [0xFF, 0x06, ...lenPrefix(strBytes('loopEnd'))] });
  }
  chunks.push(trackChunk(cond, end));

  // One MTrk per project track, in channel order.
  const tracks = [...p.tracks].sort((a, b) => a.channel - b.channel);
  for (const t of tracks) {
    if (!audible(t)) continue;
    const ch = t.channel & 15;
    const ev = [];
    ev.push({ tick: 0, order: -10, bytes: [0xFF, 0x03, ...lenPrefix(strBytes(t.name || `ch${ch + 1}`))] });
    if (ch !== 9) ev.push({ tick: 0, order: -5, bytes: [0xC0 | ch, t.program & 127] });
    let seq = 0;
    for (const c of t.ccs) ev.push({ tick: c.tick + off, order: seq++, bytes: [0xB0 | ch, c.cc & 127, c.val & 127] });
    for (const b of t.bends) {
      const v = Math.max(0, Math.min(16383, (b.value | 0) + 8192));
      ev.push({ tick: b.tick + off, order: seq++, bytes: [0xE0 | ch, v & 127, (v >> 7) & 127] });
    }
    // Same tick + same pitch: emit the shorter note first so a FIFO reader
    // pairs each note-on with its own note-off (stable re-import).
    const notes = [...t.notes].sort((a, b) => a.tick - b.tick || a.note - b.note || a.dur - b.dur);
    for (const n of notes) {
      // Groove: the start shifts, the length rides along with it. Note-offs
      // before note-ons at the same tick (order 1e6+ vs 2e6+) so a repeated
      // note re-strikes cleanly.
      const start = swingTicks(p, n.tick) + off;
      ev.push({ tick: start, order: 2e6 + seq, bytes: [0x90 | ch, n.note & 127, Math.max(1, n.vel & 127)] });
      ev.push({ tick: start + Math.max(1, n.dur), order: 1e6 + seq, bytes: [0x80 | ch, n.note & 127, 0] });
      seq++;
    }
    chunks.push(trackChunk(ev, end));
  }

  // Metronome click (playback only): claves on beats, hi wood block on the
  // downbeat, on channel 10 as its own track.
  if (opts.click) {
    const ev = [];
    ev.push({ tick: 0, order: -10, bytes: [0xFF, 0x03, ...lenPrefix(strBytes('click'))] });
    const bars = barStarts(p, Math.max(0, end - off));
    const clickVel = opts.clickVel ?? 90;
    let seq = 0;
    const emit = (tick, note, vel) => {
      ev.push({ tick, order: 2e6 + seq, bytes: [0x99, note, vel] });
      ev.push({ tick: tick + 30, order: 1e6 + seq, bytes: [0x89, note, 0] });
      seq++;
    };
    // Count-in bars live in [0, off): same grid, extended backwards.
    if (off > 0) {
      const bt = barTicksAt(p, 0), beat = beatTicksAt(p, 0);
      for (let tick = 0; tick < off; tick += beat) emit(tick, (tick % bt) === 0 ? 76 : 75, clickVel);
    }
    for (let i = 0; i < bars.length; i++) {
      const b0 = bars[i] + off;
      if (b0 >= end) break;
      const beat = beatTicksAt(p, bars[i]);
      const b1 = (i + 1 < bars.length ? bars[i + 1] : bars[i] + barTicksAt(p, bars[i])) + off;
      for (let tick = b0, k = 0; tick < b1 && tick < end; tick += beat, k++) emit(tick, k === 0 ? 76 : 75, clickVel);
    }
    chunks.push(trackChunk(ev, end));
  }

  const out = [];
  out.push(...strBytes('MThd'));
  be32(out, 6);
  be16(out, 1);
  be16(out, chunks.length);
  be16(out, p.ppq);
  for (const c of chunks) {
    out.push(...strBytes('MTrk'));
    be32(out, c.length);
    out.push(...c);
  }
  return new Uint8Array(out);
}
function lenPrefix(bytes) { const out = []; vlq(out, bytes.length); return [...out, ...bytes]; }

// ---------------- read ----------------
export function smfToProject(bytes, { name = 'imported' } = {}) {
  const d = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  if (d.length < 14 || String.fromCharCode(d[0], d[1], d[2], d[3]) !== 'MThd') throw new Error('not a MIDI file');
  const hlen = (d[4] << 24) | (d[5] << 16) | (d[6] << 8) | d[7];
  const ntracks = (d[10] << 8) | d[11];
  const division = (d[12] << 8) | d[13];
  if (division & 0x8000) throw new Error('SMPTE time division is not supported');
  const ppq = division || 480;
  let pos = 8 + hlen;

  const tempoMap = [];
  const timeSigs = [];
  const byChannel = new Map();   // ch -> {track, open: Map<note, [{tick, vel}]>}
  const trackNames = new Map();  // ch -> name (first named track that plays it)
  let loopStart = null, loopEnd = null, songName = null;
  let maxTick = 0;

  const chan = (ch) => {
    if (!byChannel.has(ch)) byChannel.set(ch, { track: newTrack(ch), open: new Map() });
    return byChannel.get(ch);
  };

  for (let ti = 0; ti < ntracks && pos + 8 <= d.length; ti++) {
    const kind = String.fromCharCode(d[pos], d[pos + 1], d[pos + 2], d[pos + 3]);
    const clen = ((d[pos + 4] << 24) | (d[pos + 5] << 16) | (d[pos + 6] << 8) | d[pos + 7]) >>> 0;
    let p = pos + 8;
    const end = Math.min(p + clen, d.length);
    pos = p + clen;
    if (kind !== 'MTrk') continue;
    let tick = 0, running = 0, trackName = null;
    const chansInTrack = new Set();
    while (p < end) {
      let delta = 0;
      for (let i = 0; i < 4; i++) { const b = d[p++]; delta = (delta << 7) | (b & 0x7F); if (!(b & 0x80)) break; }
      tick += delta;
      if (p >= end) break;
      let st = d[p];
      if (st < 0x80) st = running; else { p++; if (st < 0xF0) running = st; }
      if (st === 0xFF) {
        const type = d[p++];
        let ml = 0;
        for (let i = 0; i < 4; i++) { const b = d[p++]; ml = (ml << 7) | (b & 0x7F); if (!(b & 0x80)) break; }
        const data = d.subarray(p, p + ml);
        p += ml;
        if (type === 0x51 && ml >= 3) {
          const us = (data[0] << 16) | (data[1] << 8) | data[2];
          if (us > 0) tempoMap.push({ tick, bpm: Math.round((60000000 / us) * 1000) / 1000 });
        } else if (type === 0x58 && ml >= 2) {
          timeSigs.push({ tick, num: data[0] || 4, den: 1 << data[1] });
        } else if (type === 0x06 || type === 0x01) {
          const text = new TextDecoder().decode(data).trim();
          if (/^loop_?start$/i.test(text)) loopStart = tick;
          else if (/^loop_?end$/i.test(text)) loopEnd = tick;
        } else if (type === 0x03) {
          trackName = new TextDecoder().decode(data).trim();
          if (ti === 0 && !songName) songName = trackName;
        } else if (type === 0x2F) {
          maxTick = Math.max(maxTick, tick);
          break;
        }
        continue;
      }
      if (st === 0xF0 || st === 0xF7) {
        let sl = 0;
        for (let i = 0; i < 4; i++) { const b = d[p++]; sl = (sl << 7) | (b & 0x7F); if (!(b & 0x80)) break; }
        p += sl;
        continue;
      }
      if (st < 0x80) break;
      const hi = st & 0xF0, ch = st & 0x0F;
      const nbytes = (hi === 0xC0 || hi === 0xD0) ? 1 : 2;
      const d1 = d[p] & 0x7F, d2 = (d[p + 1] ?? 0) & 0x7F;
      p += nbytes;
      maxTick = Math.max(maxTick, tick);
      const c = chan(ch);
      chansInTrack.add(ch);
      if (hi === 0x90 && d2 > 0) {
        if (!c.open.has(d1)) c.open.set(d1, []);
        c.open.get(d1).push({ tick, vel: d2 });
      } else if (hi === 0x80 || (hi === 0x90 && d2 === 0)) {
        const q = c.open.get(d1);
        if (q && q.length) {
          const on = q.shift();
          c.track.notes.push({ tick: on.tick, dur: Math.max(1, tick - on.tick), note: d1, vel: on.vel });
        }
      } else if (hi === 0xB0) {
        c.track.ccs.push({ tick, cc: d1, val: d2 });
      } else if (hi === 0xE0) {
        c.track.bends.push({ tick, value: ((d2 << 7) | d1) - 8192 });
      } else if (hi === 0xC0) {
        // First program wins as the track's program; later ones stay as
        // events? The model is one program per track: keep the earliest.
        if (ch !== 9 && !c.track._progSet) { c.track.program = d1; c.track._progSet = true; }
      }
    }
    if (trackName) for (const ch of chansInTrack) if (!trackNames.has(ch)) trackNames.set(ch, trackName);
  }
  // Close notes still open at the end.
  for (const [, c] of byChannel) {
    for (const [note, q] of c.open) for (const on of q)
      c.track.notes.push({ tick: on.tick, dur: Math.max(1, maxTick - on.tick), note, vel: on.vel });
    c.open.clear();
    delete c.track._progSet;
  }

  const tracks = [...byChannel.values()].map((c) => c.track).filter((t) => t.notes.length || t.ccs.length || t.bends.length)
      .sort((a, b) => a.channel - b.channel);
  for (const t of tracks) if (trackNames.has(t.channel)) t.name = trackNames.get(t.channel);
  const tm = dedupeByTick(tempoMap.length ? tempoMap : [{ tick: 0, bpm: 120 }]);
  const ts = dedupeByTick(timeSigs.length ? timeSigs : [{ tick: 0, num: 4, den: 4 }]);
  const proj = normalizeProject({
    name: (songName || name).replace(/\.(midi?|mstk)$/i, ''),
    ppq, tempoMap: tm, timeSigs: ts, tracks,
    loop: { startTick: loopStart ?? 0, endTick: loopEnd ?? Math.max(1, maxTick), enabled: true },
  });
  if (loopEnd === null) proj.loop.endTick = projectEndTick(proj);
  return proj;
}
function dedupeByTick(list) {
  const m = new Map();
  for (const e of list) m.set(e.tick, e);  // last at a tick wins
  return [...m.values()].sort((a, b) => a.tick - b.tick);
}
