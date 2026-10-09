//
// Patch-fitting worker: an independent WASM engine instance searches for
// synth parameters whose render matches a target sample, while the page (and
// live playback) stay responsive. Same optimiser and loss as midisynth_fit,
// so a fit done here equals a fit done on the CLI.
//
// One restart per message round, because the restart is the natural unit of
// progress -- and because restarts, not iterations, are what actually buy a
// better fit on this landscape.
//
import createMidiSynth from './dist/midisynth.mjs';

let mod = null, fitter = null, heap = 0, heapLen = 0;

// A scratch buffer in the wasm heap for passing audio both ways.
function scratch(len) {
  if (heap && heapLen >= len) return heap;
  if (heap) mod._free(heap);
  heap = mod._malloc(len * 4);
  heapLen = len;
  return heap;
}

function writeAudio(samples) {
  const p = scratch(samples.length);
  mod.HEAPF32.set(samples, p >> 2);
  return p;
}

function readAudio(ptr, n) {
  return mod.HEAPF32.slice(ptr >> 2, (ptr >> 2) + n);
}

onmessage = async (e) => {
  const m = e.data;
  try {
    if (!mod) {
      mod = await createMidiSynth();
      fitter = new mod.PatchFitter();
    }
    if (m.type === 'archs') {
      postMessage({ type: 'archs', archs: JSON.parse(fitter.archList()) });
      return;
    }
    if (m.type === 'fit') {
      const { target, arch, weights, windowMs, iters, restarts, seed } = m;
      fitter.setTarget(writeAudio(target), target.length);
      if (!fitter.configure(arch, weights.stft, weights.env, weights.bands, windowMs)) {
        postMessage({ type: 'error', error: `unknown architecture '${arch}'` });
        return;
      }
      let best = null;
      for (let r = 0; r < restarts; r++) {
        // seed 0 starts from the centre of every range; the rest are random.
        const res = JSON.parse(fitter.fitOnce(iters, r === 0 ? 0 : seed + r));
        if (res.error) { postMessage({ type: 'error', error: res.error }); return; }
        if (!best || res.loss < best.loss) best = res;
        postMessage({ type: 'progress', restart: r + 1, of: restarts, loss: res.loss,
                      best: best.loss });
      }
      // Preview data for the panel: band profiles and both waveforms.
      const detail = JSON.parse(fitter.analyze(JSON.stringify(best.patch)));
      const n = fitter.frames();
      const p = scratch(n);
      const got = (fitter.renderTo(JSON.stringify(best.patch), p, n), readAudio(p, n));
      const tgt = (fitter.targetTo(p, n), readAudio(p, n));
      postMessage({ type: 'done', best, detail, fit: got, target: tgt, frames: n },
                  [got.buffer, tgt.buffer]);
      return;
    }
    if (m.type === 'analyze') {
      // Re-score an edited patch against the same target and weights.
      const detail = JSON.parse(fitter.analyze(m.patch));
      const n = fitter.frames();
      const p = scratch(n);
      const got = (fitter.renderTo(m.patch, p, n), readAudio(p, n));
      postMessage({ type: 'analyzed', detail, fit: got, frames: n }, [got.buffer]);
      return;
    }
  } catch (err) {
    postMessage({ type: 'error', error: String(err && err.message ? err.message : err) });
  }
};
