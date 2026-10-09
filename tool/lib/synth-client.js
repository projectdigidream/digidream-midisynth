//
// SynthClient: the one AudioWorklet engine of a page, wrapped as promises.
//
//   const client = await SynthClient.create({ analyser: true });
//   await client.loadBank(json);                 // rejects if the report says !ok
//   const info = await client.trackInfo();
//   const bytes = await client.cutTrack(track, 0, 1920, 'intro');
//   client.play();                               // fire-and-forget transport
//   client.addEventListener('status', (e) => ... e.detail ...);
//
// Every request carries a requestId the worklet echoes on its replies
// (synth-worklet.js), so several panels can share the engine without racing
// on "whose report is this". Byte buffers are COPIED before transfer: the
// caller keeps its ArrayBuffer.
//
export class SynthClient extends EventTarget {
  static async create({ analyser = false, sampleRate = 48000, workletUrl = null } = {}) {
    const ctx = new AudioContext({ sampleRate });
    await ctx.audioWorklet.addModule(workletUrl || new URL('../synth-worklet.js', import.meta.url).href);
    const node = new AudioWorkletNode(ctx, 'midisynth', {
      numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2],
    });
    let an = null;
    if (analyser) {
      an = ctx.createAnalyser();
      an.fftSize = 4096;
      an.smoothingTimeConstant = 0.6;
      node.connect(an);
      an.connect(ctx.destination);
    } else {
      node.connect(ctx.destination);
    }
    const client = new SynthClient(ctx, node, an);
    await new Promise((resolve, reject) => {
      client._readyResolve = resolve;
      client._readyReject = reject;
    });
    await ctx.resume();
    return client;
  }

  constructor(ctx, node, analyser) {
    super();
    this.ctx = ctx;
    this.node = node;
    this.analyser = analyser;
    this.port = node.port;
    this.ready = false;
    this.lastStatus = null;
    this._pending = new Map();
    this._nextId = 1;
    this.port.onmessage = (e) => this._onMessage(e.data);
  }

  _onMessage(m) {
    switch (m.type) {
      case 'ready':
        this.ready = true;
        this._readyResolve?.();
        this.dispatchEvent(new CustomEvent('ready'));
        return;
      case 'fatal':
        if (!this.ready) this._readyReject?.(new Error(m.error));
        for (const p of this._pending.values()) p.reject(new Error('worklet failed: ' + m.error));
        this._pending.clear();
        this.dispatchEvent(new CustomEvent('fatal', { detail: m }));
        return;
      case 'status':
        this.lastStatus = m;
        this.dispatchEvent(new CustomEvent('status', { detail: m }));
        return;
      default: break;
    }
    const p = m.requestId !== undefined ? this._pending.get(m.requestId) : null;
    if (m.type === 'report') {
      this.dispatchEvent(new CustomEvent('report', { detail: m }));
      if (p) {
        if (!m.report.ok) {
          this._pending.delete(m.requestId);
          const err = new Error((m.report.errors || []).join('; ') || `${m.what} rejected`);
          err.report = m.report;
          p.reject(err);
        } else if (p.expect === 'report') {
          this._pending.delete(m.requestId);
          p.resolve(m.report);
        } else {
          p.report = m.report;  // data message follows
        }
      }
      return;
    }
    this.dispatchEvent(new CustomEvent(m.type, { detail: m }));
    if (p && m.type === p.expect) {
      this._pending.delete(m.requestId);
      m.report = p.report;
      p.resolve(m);
    }
  }

  // ---- raw ----
  post(msg, transfer = []) { this.port.postMessage(msg, transfer); }
  request(msg, expect = 'report', transfer = []) {
    return new Promise((resolve, reject) => {
      const requestId = this._nextId++;
      this._pending.set(requestId, { expect, resolve, reject });
      this.port.postMessage({ ...msg, requestId }, transfer);
    });
  }
  static _copy(bytes) {
    if (bytes instanceof ArrayBuffer) return bytes.slice(0);
    if (ArrayBuffer.isView(bytes)) return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
    throw new TypeError('bytes must be an ArrayBuffer or a typed array');
  }
  _withBytes(msg, bytes, expect = 'report', key = 'bytes') {
    const buf = SynthClient._copy(bytes);
    return this.request({ ...msg, [key]: buf }, expect, [buf]);
  }

  // ---- content ----
  loadBank(json) { return this.request({ type: 'loadBank', json }); }
  loadDrums(json) { return this.request({ type: 'loadDrums', json }); }
  loadProgram(prog, json) { return this.request({ type: 'loadProgram', prog, json }); }
  loadDrumNote(note, json) { return this.request({ type: 'loadDrumNote', note, json }); }
  loadSongConfig(json) { return this.request({ type: 'loadSongConfig', json }); }
  loadMaster(json) { return this.request({ type: 'loadMaster', json }); }
  masterJson() { return this.request({ type: 'masterJson' }, 'masterJson').then((m) => m.json); }
  masterParam(id, q16) { this.post({ type: 'masterParam', id, q16: Math.round(q16) }); }

  // ---- songs / tracks ----
  loadSong(bytes) { return this._withBytes({ type: 'loadSong' }, bytes); }
  loadTrack(bytes) { return this._withBytes({ type: 'loadTrack' }, bytes); }
  // Either kind: dispatches on the MSTK magic.
  loadAny(bytes) {
    const u8 = new Uint8Array(SynthClient._copy(bytes));
    const track = u8.length >= 4 && u8[0] === 0x4D && u8[1] === 0x53 && u8[2] === 0x54 && u8[3] === 0x4B;
    return track ? this.loadTrack(u8) : this.loadSong(u8);
  }
  crossfade(bytes, fadeMs) { return this._withBytes({ type: 'crossfade', fadeMs }, bytes); }
  crossfadeTrack(bytes, fadeMs) { return this._withBytes({ type: 'crossfadeTrack', fadeMs }, bytes); }
  // Live edit: swap the song keeping the musical position.
  replaceSong(bytes) { return this._withBytes({ type: 'replaceSong' }, bytes); }
  // Cue change: mode 'loopEnd' | 'nextBar' | 'now' (fadeMs applies to 'now').
  queueTrack(bytes, mode = 'loopEnd', fadeMs = 0) {
    return this._withBytes({ type: 'queueTrack', mode, fadeMs }, bytes);
  }
  cancelQueued() { this.post({ type: 'cancelQueued' }); }
  trackInfo() { return this.request({ type: 'trackInfo' }, 'trackInfo').then((m) => m.info); }
  // Embedded content of the last loaded .mstk: {name, bankJson, kitJson, fx}.
  trackContent() { return this.request({ type: 'trackContent' }, 'trackContent').then((m) => m.content); }
  cutTrack(bytes, startTick, endTick, name) {
    return this._withBytes({ type: 'cutTrack', startTick, endTick, name }, bytes, 'cutBaked')
      .then((m) => m.bytes);
  }
  bakeTrack({ smfBytes, bankJson, kitJson, fxJson = '', name = 'track', verifySeconds = 0 }) {
    return this._withBytes({ type: 'bakeTrack', bankJson, kitJson, fxJson, name, verifySeconds },
                           smfBytes, 'bakedTrack', 'smfBytes').then((m) => m.bytes);
  }
  exportSmf(name = 'song') {
    return this.request({ type: 'exportSmf', name }, 'smf').then((m) => m.bytes);
  }

  // ---- transport ----
  play() { this.post({ type: 'play' }); }
  pause() { this.post({ type: 'pause' }); }
  stop() { this.post({ type: 'stop' }); }
  seekMs(ms) { this.post({ type: 'seekMs', ms: Math.round(ms) }); }
  setLoop(on) { this.post({ type: 'setLoop', on: !!on }); }
  loopTicks(startTick, endTick) { this.post({ type: 'loopTicks', startTick, endTick }); }
  tempoScale(milli) { this.post({ type: 'tempoScale', milli }); }
  masterVolume(milli) { this.post({ type: 'masterVolume', milli }); }
  channelMute(ch, muted, fadeMs = 300) { this.post({ type: 'channelMute', ch, muted, fadeMs }); }
  duck(milli, rampMs) { this.post({ type: 'duck', milli, rampMs }); }
  dither(on) { this.post({ type: 'dither', on: !!on }); }

  // ---- live MIDI ----
  noteOn(ch, note, vel) { this.post({ type: 'noteOn', ch, note, vel }); }
  noteOff(ch, note) { this.post({ type: 'noteOff', ch, note }); }
  cc(ch, num, val) { this.post({ type: 'cc', ch, num, val }); }
  program(ch, prog) { this.post({ type: 'program', ch, prog }); }
  bend(ch, value) { this.post({ type: 'bend', ch, value }); }
  aftertouch(ch, pressure) { this.post({ type: 'aftertouch', ch, pressure }); }
  reverbParam(id, milli) { this.post({ type: 'reverbParam', id, milli }); }
  chorusParam(id, milli) { this.post({ type: 'chorusParam', id, milli }); }
}
