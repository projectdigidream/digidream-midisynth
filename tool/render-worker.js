//
// Offline render worker: a second, independent WASM engine instance renders
// the song faster than realtime with the tool's current (edited) bank/kit,
// while live playback continues untouched. Same code path as the golden
// tests -- what you export is what the game will play, bit for bit.
//
import createMidiSynth from './dist/midisynth.mjs';

const SAMPLE_RATE = 48000;
const BUFFER = 512;

function wavHeader(dataBytes) {
  const h = new ArrayBuffer(44);
  const v = new DataView(h);
  const str = (o, s) => { for (let i = 0; i < s.length; i++) v.setUint8(o + i, s.charCodeAt(i)); };
  str(0, 'RIFF'); v.setUint32(4, 36 + dataBytes, true); str(8, 'WAVE');
  str(12, 'fmt '); v.setUint32(16, 16, true); v.setUint16(20, 1, true);
  v.setUint16(22, 2, true); v.setUint32(24, SAMPLE_RATE, true);
  v.setUint32(28, SAMPLE_RATE * 4, true); v.setUint16(32, 4, true); v.setUint16(34, 16, true);
  str(36, 'data'); v.setUint32(40, dataBytes, true);
  return new Uint8Array(h);
}

onmessage = async (e) => {
  const { bankJson, kitJson, songBytes, seconds, dither, loop } = e.data;
  try {
    const mod = await createMidiSynth();
    const s = new mod.WebSynth();
    const reports = [];
    for (const [fn, json, what] of [[s.loadBank.bind(s), bankJson, 'bank'],
                                    [s.loadDrums.bind(s), kitJson, 'drums']]) {
      const r = JSON.parse(fn(json));
      reports.push([what, r]);
      if (!r.ok) throw new Error(`${what}: ${r.errors.join('; ')}`);
    }
    const bytes = new Uint8Array(songBytes);
    const ptr = s.songBufferPrepare(bytes.length);
    mod.HEAPU8.set(bytes, ptr);
    const sr = JSON.parse(s.loadSongFromBuffer());
    reports.push(['song', sr]);
    if (!sr.ok) throw new Error('song: ' + sr.errors.join('; '));
    postMessage({ type: 'reports', reports });

    s.setDither(!!dither);
    s.setLoop(!!loop);
    s.play();

    // Default: song length + 2s of release/reverb tail (the CLI's rule).
    const totalSec = seconds > 0 ? seconds : s.durationMs() / 1000 + 2;
    const totalFrames = Math.floor((totalSec * SAMPLE_RATE) / BUFFER) * BUFFER;
    const pcm = new Int16Array(totalFrames * 2);
    let done = 0;
    let lastPct = -1;
    while (done < totalFrames) {
      const p = s.render(BUFFER);
      pcm.set(new Int16Array(mod.HEAP16.buffer, p, BUFFER * 2), done * 2);
      done += BUFFER;
      const pct = Math.floor((done / totalFrames) * 100);
      if (pct !== lastPct && pct % 2 === 0) {
        lastPct = pct;
        postMessage({ type: 'progress', pct });
      }
    }
    const body = new Uint8Array(pcm.buffer);
    const wav = new Uint8Array(44 + body.length);
    wav.set(wavHeader(body.length), 0);
    wav.set(body, 44);
    postMessage({ type: 'done', wav: wav.buffer, frames: totalFrames,
                  peakVoices: s.peakVoices(), steals: s.stealCount(),
                  drops: s.droppedNotes() }, [wav.buffer]);
  } catch (err) {
    postMessage({ type: 'error', error: String(err) });
  }
};
