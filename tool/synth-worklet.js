//
// AudioWorkletProcessor hosting the whole synth. The WASM module (single-file
// ES6) is imported straight into the worklet scope; the main thread drives it
// exclusively through this port -- no SharedArrayBuffer, no COOP/COEP.
//
import createMidiSynth from './dist/midisynth.mjs';

class MidiSynthProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.ready = false;
    this.pending = [];
    this.sinceStatus = 0;
    this.port.onmessage = (e) => {
      if (!this.ready) this.pending.push(e.data);
      else this.handle(e.data);
    };
    createMidiSynth().then((mod) => {
      this.mod = mod;
      this.synth = new mod.WebSynth();
      this.ready = true;
      for (const m of this.pending) this.handle(m);
      this.pending = [];
      this.port.postMessage({ type: 'ready' });
    }).catch((err) => {
      this.port.postMessage({ type: 'fatal', error: String(err) });
    });
  }

  handle(m) {
    const s = this.synth;
    switch (m.type) {
      case 'loadBank':
        this.report('bank', s.loadBank(m.json));
        break;
      case 'loadSongConfig':
        this.report('fx', s.loadSongConfig(m.json));
        break;
      case 'loadDrums':
        this.report('drums', s.loadDrums(m.json));
        break;
      case 'loadProgram':
        this.report('patch', s.loadProgram(m.prog | 0, m.json));
        break;
      case 'loadDrumNote':
        this.report('patch', s.loadDrumNote(m.note | 0, m.json));
        break;
      case 'loadSong': {
        const bytes = new Uint8Array(m.bytes);
        const ptr = s.songBufferPrepare(bytes.length);
        this.mod.HEAPU8.set(bytes, ptr);
        this.report('song', s.loadSongFromBuffer());
        break;
      }
      case 'loadTrack': {
        const bytes = new Uint8Array(m.bytes);
        const ptr = s.songBufferPrepare(bytes.length);
        this.mod.HEAPU8.set(bytes, ptr);
        this.report('track', s.loadTrackFromBuffer());
        break;
      }
      case 'trackInfo':
        this.port.postMessage({ type: 'trackInfo', info: JSON.parse(s.trackInfoJson()) });
        break;
      case 'cutTrack': {
        const bytes = new Uint8Array(m.bytes);
        const ptr = s.songBufferPrepare(bytes.length);
        this.mod.HEAPU8.set(bytes, ptr);
        const rep = JSON.parse(s.cutTrackFromBuffer(m.startTick | 0, m.endTick | 0,
                                                    m.name || 'slice'));
        this.port.postMessage({ type: 'report', what: 'cut', report: rep });
        if (rep.ok) {
          const size = s.bakedTrackSize();
          const out = new Uint8Array(size);
          out.set(new Uint8Array(this.mod.HEAPU8.buffer, s.bakedTrackPtr(), size));
          this.port.postMessage({ type: 'cutBaked', bytes: out.buffer, name: m.name,
                                  requestId: m.requestId }, [out.buffer]);
        }
        break;
      }
      case 'crossfadeTrack': {
        const bytes = new Uint8Array(m.bytes);
        const ptr = s.songBufferPrepare(bytes.length);
        this.mod.HEAPU8.set(bytes, ptr);
        this.report('crossfade', s.crossfadeTrackFromBuffer(m.fadeMs | 0));
        break;
      }
      case 'bakeTrack': {
        const bytes = new Uint8Array(m.smfBytes);
        const ptr = s.songBufferPrepare(bytes.length);
        this.mod.HEAPU8.set(bytes, ptr);
        const rep = JSON.parse(s.bakeTrackFromBuffer(
            m.bankJson, m.kitJson, m.fxJson || '', m.name || 'track', m.verifySeconds | 0));
        this.port.postMessage({ type: 'report', what: 'bake', report: rep });
        if (rep.ok) {
          const size = s.bakedTrackSize();
          const out = new Uint8Array(size);
          out.set(new Uint8Array(this.mod.HEAPU8.buffer, s.bakedTrackPtr(), size));
          this.port.postMessage({ type: 'bakedTrack', bytes: out.buffer, name: m.name },
                                [out.buffer]);
        }
        break;
      }
      case 'crossfade': {
        const bytes = new Uint8Array(m.bytes);
        const ptr = s.songBufferPrepare(bytes.length);
        this.mod.HEAPU8.set(bytes, ptr);
        this.report('song', s.crossfadeFromBuffer(m.fadeMs | 0));
        break;
      }
      case 'play': s.play(); break;
      case 'pause': s.pause(); break;
      case 'stop': s.stop(); break;
      case 'seekMs': s.seekMs(m.ms | 0); break;
      case 'setLoop': s.setLoop(!!m.on); break;
      case 'loopTicks': s.setLoopTicks(m.startTick | 0, m.endTick | 0); break;
      case 'tempoScale': s.setTempoScaleMilli(m.milli | 0); break;
      case 'masterVolume': s.setMasterVolumeMilli(m.milli | 0); break;
      case 'channelMute': s.channelMute(m.ch | 0, !!m.muted, m.fadeMs | 0); break;
      case 'duck': s.duckMilli(m.milli | 0, m.rampMs | 0); break;
      case 'dither': s.setDither(!!m.on); break;
      case 'noteOn': s.noteOn(m.ch | 0, m.note | 0, m.vel | 0); break;
      case 'noteOff': s.noteOff(m.ch | 0, m.note | 0); break;
      case 'cc': s.cc(m.ch | 0, m.num | 0, m.val | 0); break;
      case 'program': s.programChange(m.ch | 0, m.prog | 0); break;
      case 'bend': s.pitchBend(m.ch | 0, m.value | 0); break;
      case 'aftertouch': s.aftertouch(m.ch | 0, m.pressure | 0); break;
      case 'reverbParam': s.reverbParam(m.id | 0, m.milli | 0); break;
      case 'chorusParam': s.chorusParam(m.id | 0, m.milli | 0); break;
      default: break;
    }
  }

  report(what, jsonText) {
    this.port.postMessage({ type: 'report', what, report: JSON.parse(jsonText) });
  }

  process(inputs, outputs) {
    const out = outputs[0];
    if (!out || out.length === 0) return true;
    const L = out[0];
    const R = out.length > 1 ? out[1] : out[0];
    const n = L.length; // 128, a multiple of the engine's 64-sample block
    if (!this.ready) {
      L.fill(0); if (R !== L) R.fill(0);
      return true;
    }
    const p = this.synth.render(n);
    const heap = this.mod.HEAP16;
    const base = p >> 1;
    for (let i = 0; i < n; i++) {
      L[i] = heap[base + 2 * i] / 32768;
      R[i] = heap[base + 2 * i + 1] / 32768;
    }
    this.sinceStatus += n;
    if (this.sinceStatus >= 4800) {  // ~10 Hz status feed
      this.sinceStatus = 0;
      const s = this.synth;
      const channelNotes = [], channelPrograms = [];
      for (let ch = 0; ch < 16; ch++) {
        channelNotes.push(s.channelNotes(ch));
        channelPrograms.push(s.channelProgram(ch));
      }
      this.port.postMessage({
        type: 'status',
        playing: s.playing(),
        timeMs: s.timeMs(),
        durationMs: s.durationMs(),
        bar: s.bar(),
        beat: s.beat(),
        activeVoices: s.activeVoices(),
        peakVoices: s.peakVoices(),
        steals: s.stealCount(),
        drops: s.droppedNotes(),
        queueDepth: s.queueDepth(),
        channelNotes,
        channelPrograms,
        drumNotes: s.activeNoteList(9),
      });
    }
    return true;
  }
}

registerProcessor('midisynth', MidiSynthProcessor);
