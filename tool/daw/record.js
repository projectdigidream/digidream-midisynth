//
// Recording from a MIDI instrument onto the armed track: count-in with the
// metronome click, overdub/replace, quantize-on-record, latency offset.
//
// Timing: the worklet's 10 Hz status feed anchors song time to
// performance.now(); Web MIDI timestamps share that clock, so each event maps
// to a song millisecond, then to a tick through the project tempo map. The
// count-in is real song time: the playback SMF is built with every event
// shifted by the count-in and clicks in front, so ticks recorded during the
// count-in are simply negative and dropped.
//
import { scoped } from '../lib/ui.js';
import { msToTick, barTicksAt, gridTicks, sortEvents } from './project.js';

export function initRecorder(root, ctx, midi) {
  const $ = scoped(root);
  const { store, client, log } = ctx;
  let recording = false;
  let armed = null;          // channel
  let offsetTicks = 0;
  let captured = [], capturedCC = [], capturedBend = [];
  const open = new Map();    // note -> {tick, vel}
  const state = (t) => { $('rec-state').textContent = t; };

  function refreshDevices() {
    const sel = $('rec-device');
    const cur = sel.value;
    sel.innerHTML = '<option value="">all inputs</option>';
    for (const d of midi?.inputs?.() || []) {
      const o = document.createElement('option');
      o.value = d.id;
      o.textContent = d.name;
      sel.appendChild(o);
    }
    sel.value = cur;
  }
  $('rec-device').onchange = (e) => midi?.select(e.target.value || null);

  // Shifted tempo map for the recording session (count-in in front).
  function songTickAt(ms) {
    const p = store.p;
    const shifted = { ...p, tempoMap: p.tempoMap.map((t) => ({ tick: t.tick ? t.tick + offsetTicks : 0, bpm: t.bpm })) };
    return msToTick(shifted, ms) - offsetTicks;
  }
  function tickNow(ts) {
    const ms = ctx.playback.msAt(ts) - (+$('rec-latency').value || 0);
    let tick = Math.round(songTickAt(ms));
    const p = store.p;
    if (p.loop.enabled && tick >= p.loop.endTick) tick -= p.loop.endTick - p.loop.startTick;  // wrap not yet seen in status
    return tick;
  }

  function armedTrack() {
    const p = store.p;
    return p.tracks.find((t) => t.arm) || p.tracks.find((t) => t.channel === ctx.ui.selectedChannel) || null;
  }

  async function start() {
    const t = armedTrack();
    if (!t) { log('record: no track to record on', 'err'); return; }
    if (!t.arm) store.mutate('arm', (pp) => { for (const tr of pp.tracks) tr.arm = tr.channel === t.channel; }, { audio: false });
    armed = t.channel;
    const p = store.p;
    const bars = +$('count-in')?.value || 0;
    offsetTicks = bars * barTicksAt(p, 0);
    captured = []; capturedCC = []; capturedBend = [];
    open.clear();
    recording = true;
    $('btn-rec')?.classList.add('active');
    await ctx.playback.reload({ keepPosition: false, click: true, offsetTicks, replaceRecordedRegion: false });
    client.seekMs(0);
    client.play();
    state(`recording on '${t.name}' (ch ${t.channel + 1})${bars ? ` — ${bars} bar count-in` : ''}…`);
    log(`● recording on ${t.name}`, 'ok');
  }

  function midiMessage(d, ts) {
    const t = armedTrack();
    if (!t) return;
    const ch = t.channel;
    const kind = d[0] & 0xF0;
    // Always monitor through the track's channel.
    switch (kind) {
      case 0x90: if (d[2] > 0) client.noteOn(ch, d[1], d[2]); else client.noteOff(ch, d[1]); break;
      case 0x80: client.noteOff(ch, d[1]); break;
      case 0xB0: client.cc(ch, d[1], d[2]); break;
      case 0xE0: client.bend(ch, ((d[2] << 7) | d[1]) - 8192); break;
      case 0xD0: client.aftertouch(ch, d[1]); break;
      default: return;
    }
    if (!recording || !ctx.playback.isPlaying()) return;
    const tick = tickNow(ts ?? performance.now());
    if (kind === 0x90 && d[2] > 0) open.set(d[1], { tick, vel: d[2] });
    else if (kind === 0x80 || (kind === 0x90 && d[2] === 0)) {
      const on = open.get(d[1]);
      if (on) { open.delete(d[1]); if (on.tick >= 0) captured.push({ tick: on.tick, dur: Math.max(1, tick - on.tick), note: d[1], vel: on.vel }); }
    } else if (kind === 0xB0 && tick >= 0) capturedCC.push({ tick, cc: d[1], val: d[2] });
    else if (kind === 0xE0 && tick >= 0) capturedBend.push({ tick, value: ((d[2] << 7) | d[1]) - 8192 });
  }

  async function stop() {
    if (!recording) return;
    recording = false;
    $('btn-rec')?.classList.remove('active');
    const now = tickNow(performance.now());
    for (const [note, on] of open) if (on.tick >= 0) captured.push({ tick: on.tick, dur: Math.max(1, now - on.tick), note, vel: on.vel });
    open.clear();
    client.stop();
    const p = store.p;
    const q = $('rec-quant').value;
    const grid = q === 'off' ? 0 : gridTicks(p, q);
    const snap = (tick) => (grid ? Math.round(tick / grid) * grid : tick);
    const notes = captured.map((n) => ({ ...n, tick: snap(n.tick), dur: grid ? Math.max(grid, Math.round(n.dur / grid) * grid) : n.dur }))
                          .filter((n) => n.tick >= 0);
    const ccs = capturedCC.filter((c) => c.tick >= 0), bends = capturedBend.filter((b) => b.tick >= 0);
    const ch = armed;
    const replace = $('rec-mode').value === 'replace';
    if (notes.length || ccs.length || bends.length) {
      store.mutate('record', (pp) => {
        const tr = pp.tracks.find((x) => x.channel === ch);
        if (!tr) return;
        if (replace) {
          const inRegion = (tick) => !pp.loop.enabled || (tick >= pp.loop.startTick && tick < pp.loop.endTick);
          tr.notes = tr.notes.filter((n) => !inRegion(n.tick));
          tr.ccs = tr.ccs.filter((c) => !inRegion(c.tick));
          tr.bends = tr.bends.filter((b) => !inRegion(b.tick));
        }
        tr.notes.push(...notes);
        tr.ccs.push(...ccs);
        tr.bends.push(...bends);
        sortEvents(tr);
      });
      log(`recorded ${notes.length} notes${ccs.length ? `, ${ccs.length} CC` : ''}${bends.length ? `, ${bends.length} bends` : ''}`, 'ok');
      state(`recorded ${notes.length} notes onto ch ${ch + 1}${grid ? ' (quantized ' + q + ')' : ''}`);
    } else {
      state('nothing recorded');
    }
    offsetTicks = 0;
    await ctx.playback.reload({ keepPosition: false });
  }

  refreshDevices();
  return {
    midiMessage,
    refreshDevices,
    toggle() { return recording ? stop() : start(); },
    stop,
    get recording() { return recording; },
    get offsetTicks() { return offsetTicks; },
  };
}
