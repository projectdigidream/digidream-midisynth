#!/usr/bin/env node
//
// WASM leg of the determinism gate: renders a reference MIDI through the
// built ES6 module and prints the PCM sha256 in the same format as
// midisynth_render --hash. run_goldens.sh compares the two.
//
//   node test/determinism_wasm.mjs <song.mid> <seconds>
//
import { createHash } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const { default: createMidiSynth } = await import(join(root, 'tool/dist/midisynth.mjs'));

const [song, seconds] = [process.argv[2], Number(process.argv[3] || 10)];

const mod = await createMidiSynth();
const synth = new mod.WebSynth();

for (const [fn, path] of [
  ['loadBank', 'content/patches/gm_bank.json'],
  ['loadDrums', 'content/drums/gm_kit.json'],
]) {
  const r = JSON.parse(synth[fn](readFileSync(join(root, path), 'utf8')));
  if (!r.ok) throw new Error(`${fn}: ${r.errors}`);
}

const smf = readFileSync(song);
const ptr = synth.songBufferPrepare(smf.length);
mod.HEAPU8.set(smf, ptr);
const r = JSON.parse(synth.loadSongFromBuffer());
if (!r.ok) throw new Error(`song: ${r.errors}`);
synth.play();

const BUFFER = 512;
const totalFrames = Math.floor((seconds * 48000) / BUFFER) * BUFFER;
const hash = createHash('sha256');
for (let done = 0; done < totalFrames; done += BUFFER) {
  const p = synth.render(BUFFER);
  hash.update(Buffer.from(mod.HEAP16.buffer, p, BUFFER * 2 * 2));
}
console.log(`sha256 ${hash.digest('hex')}`);
