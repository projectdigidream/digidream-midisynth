# Developing DigidreamSynth

How the engine works, how it is tested, and its command-line tools. To build it
at all, see "Build from source" in [README.md](README.md): the sibling `engine`
checkout and the miniaudio header are needed.

*(Code identifiers keep the `msynth` / `midisynth_` prefixes.)*

## The engine

Wavetable and subtractive synthesis in fixed-point integer math, native and in
the browser (WebAssembly), with game-facing music direction: seamless loops,
crossfades, layer muting, ducking and beat sync. It reuses the Digidream
Engine's fixed-point primitives (`core/include/dnp/math/fixed.hpp`: the
`asr`/`asl` shift helpers and type aliases) and its determinism discipline:
every constant derived from a transcendental function is baked by
`scripts/gen_tables.py` (Python `decimal` at 60 digits) into integer literals in
`src/synth/tables_baked.inc`, so no platform's libm ever touches the audio path.

The result is **bit-identical** output: the same MIDI rendered by the native
binary and by the WebAssembly build under Node hashes the same.

## Tests

```sh
cmake -S . -B build -G Ninja && ninja -C build   # native lib + CLIs + tests
./build/midisynth_tests                          # the unit suite
./scripts/build_wasm.sh                          # tool/dist/midisynth.mjs
./scripts/run_goldens.sh                         # determinism gate (+ wasm cross-check)
node test/smf_roundtrip.mjs                      # DAW model <-> SMF round trips
./scripts/ci.sh                                  # all of the above
```

`scripts/ci.sh` skips the WebAssembly build when it finds no `em++`, and the
goldens then skip the native-vs-wasm comparison.

The golden harness renders a reference corpus and compares SHA-256 hashes of
the PCM, checks that a baked track renders bit-identically to its sources, and
compares native against WebAssembly.

Browser and shell smoke tests, each skipped when what it needs is missing:

- `node test/tool_smoke.mjs` and `node test/daw_smoke.mjs`: headless Chrome
  against a running `node tool/serve.js`. They need Chrome and
  `puppeteer-core` installed in the working directory.
- `node test/electron_smoke.mjs` (`--packaged` for the built app): the desktop
  shell. Needs `electron/node_modules`.
- `node test/android_smoke.mjs`: the installed Android app, through the debug
  WebView's DevTools over adb.

[docs/DAW.md](docs/DAW.md) lists what each one covers.

### Regenerating the goldens

Intentional DSP changes invalidate the hashes. `scripts/run_goldens.sh --regen`
re-renders everything, writes new hashes and drops review WAVs into
`test/golden/review/`. **Listen to them** before committing. Regenerating never
weakens the cross-platform check; it only moves the reference point.

## Layout

```
src/synth/       oscillator, envelopes, SVF filter, LFO/mod matrix, voices,
                 patches, effects, engine   (audio path: integer-only)
src/midi/        SMF parser and writer, tempo map (cumulative sample anchors)
src/track/       .mstk baked-track format (bake + load, tree-shaking)
src/sequencer/   song, sequencer (virtual clock), music director, SPSC queue
src/output/      offline WAV renderer, render/play/bake/fit CLIs, sha256
src/bindings/    the C API and the embind API (WebSynth) for the WASM build
content/         instrument banks, drum kits, effect configs, the demo track
tool/            the web pages: DAW, patch editor, track cutter
tool/lib/        shared panels (synth client, patch editor, cutter, scope, MIDI input)
tool/daw/        the DAW: project model, JS SMF, arrange, piano roll, recorder, cues, master
electron/        desktop shell (app:// protocol, native menu, file dialogs)
mobile/          Android shell (Capacitor)
deploy/          nginx, Caddy, Docker and systemd recipes for the web build
test/unit/       unit tests
test/golden/     reference MIDI + pinned PCM hashes
```

## Command-line tools

Render a song offline, or play it live through the OS audio device (miniaudio).
`--fx` loads a song-level effect config (examples in `content/fx/`):

```sh
./build/midisynth_render song.mid out.wav \
    --bank content/patches/gm_bank.json --drums content/drums/gm_kit.json \
    [--fx content/fx/cathedral.json] [--master content/fx/master_default.json] \
    [--seconds N] [--loop] [--dither] [--hash]

./build/midisynth_play song.mid --bank ... --drums ... [--fx ...] [--master ...] [--loop] \
    [--tempo milli] [--volume milli] [--seek ms]
# prints live position, voices, steals/drops, and render-callback CPU
```

`midisynth_bake` compiles a song into a `.mstk` (below), and
`midisynth_bake --to-mid track.mstk out.mid` writes a track's song back out as
a Standard MIDI File. `midisynth_fit` fits a patch to a recorded sample; the
DAW's Fit tab uses the same code.

Native integration goes through the plain C API (`src/bindings/c_api.h`).
`midisynth_play` is the reference embedding, and a unit test pins the C path
bit for bit against the C++ path.

## Baked tracks (.mstk)

The shipping format. `midisynth_bake` compiles MIDI, bank, kit, effects and
mastering into one self-contained, integrity-sealed binary: pre-parsed events
(no SMF parsing or tick math at load), only the instruments the song actually
plays, baked loop points, and an optional embedded reference-render SHA-256
that `midisynth_bake --check` re-verifies on any machine. Every consumer (the
CLIs, the C API's `msynth_load_track`, WASM, the tools) accepts a `.mstk`
wherever it accepts a `.mid`. Corrupt tracks are rejected outright. Format:
[docs/TRACK_FORMAT.md](docs/TRACK_FORMAT.md).

**The track cutter** slices baked tracks into gameplay pieces. Slices are
state-chased at their start (they open sounding exactly as the parent did
there), close notes that cross the boundary, loop whole by default, and inherit
the parent's content verbatim, so sibling slices can always crossfade
(`msynth_crossfade_track` is the runtime counterpart).

**Mastering** is part of the deliverable: a fixed-point chain (EQ, compressor,
bus saturation, width, gain, lookahead limiter) inside the engine, `--master`
on the CLIs, baked into the `.mstk` (`MSTR` section) so the game plays the
mastered result bit-identically. Bypassed by default, and byte-identical when
bypassed. [docs/PATCH_FORMAT.md](docs/PATCH_FORMAT.md), section 5.1.

## Patches, kits and effects

Field by field, with every range, default and a recipe cookbook:
[docs/PATCH_FORMAT.md](docs/PATCH_FORMAT.md). In short:

- **Nine wavetable generators**, all band-limited per mip (the nonlinear ones go
  through DFT analysis and capped resynthesis, so no recipe can alias):
  additive harmonic morph, pulse (PWM morph), drawbar organ, waveshaper,
  Chebyshev, a 64-partial harmonics histogram, hard sync, 2-operator FM, and a
  custom drawn or imported cycle.
- `osc: "wavetable" | "noise"` per layer: snares and hi-hats are noise layers
  through the filter.
- Per layer: fixed `pitchNote` (drum layers), a velocity curve, unison stacks,
  ring mod, pre/post-filter saturation, a second filter (serial or parallel,
  comb and vowel formant modes), 4 LFOs with tempo sync, DAHDSR and looping
  envelopes, per-note aftertouch, release velocity, layer start delay (flams),
  round-robin groups, glide (CC5/CC65) and microtuning.
- Drum patches add `oneShot` (ignores note-off) and `exclusiveGroup` (choke).
- Per-channel buses with a 3-band EQ, and reverb, chorus and delay (CC94) sends.
- Envelopes and gain ramps run in Q8.24 with per-sample wavetable-morph
  interpolation, and the 16-bit output uses error-feedback rounding, so quiet
  tails glide instead of stair-stepping.

Live music direction: a song can be replaced under a running transport (the
engine diffs note and controller state at the playhead; it never hard-cuts),
cues can be scheduled at loop end or the next bar (`Sequencer::queue_next`,
`MusicDirector::transition`), and piece switches raise callbacks.

## Design notes

- **Mip tables floor at 64 samples** (`MIN_TABLE_SIZE`) rather than halving
  down to 8: 4-point Hermite on an 8-sample cycle leaves interpolation spurs at
  about -32 dB (audible on top-octave notes); 64 keeps them below about -66 dB,
  pinned by a regression test.
- **Web threading.** The whole engine runs inside the AudioWorklet, imported as
  a single-file ES module, and the main thread drives it over the MessagePort.
  `-sAUDIO_WORKLET` would need SharedArrayBuffer and COOP/COEP headers; this
  way no special headers are needed anywhere.
- `k = 2.0` (zero resonance) does not exist in Q2.30's ±2 range; it clamps to
  `0x7FFFFFFF` (Q = 0.5 + 1 ulp, indistinguishable).
- Baked tables are generated offline and committed, rather than computed at
  startup: the strongest possible "identical on every target".
- Voices are independent, so there is no cross-layer modulation; the ring mod's
  internal carrier covers the classic sound. Mips are selected at note-on, not
  crossfaded.
