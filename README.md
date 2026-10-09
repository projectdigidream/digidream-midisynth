# DigidreamSynth

*(directory `midisynth/`; code identifiers keep the `msynth`/`midisynth_` prefixes)*

A fully self-synthesized, **bit-identical deterministic** MIDI music engine —
wavetable + subtractive synthesis in fixed-point integer math, playable
natively and in the browser (WASM), with game-facing music direction
(seamless loops, crossfades, layer muting, ducking, beat sync) and a web
authoring tool with live MIDI keyboard input.

Adjacent project to the Digidream engine; not yet wired into the root
build. It reuses the engine's fixed-point primitives
(`core/include/dnp/math/fixed.hpp` — the `asr`/`asl` shift helpers and type
aliases) and its determinism discipline: every transcendental-derived constant
is baked by `scripts/gen_tables.py` (Python `decimal` at 60 digits) into
integer literals, so platform libm never touches the audio path.

**Proven determinism**: `scripts/run_goldens.sh` renders a reference corpus
(including 30s of a real 31-track Tchaikovsky SMF) and compares SHA-256 PCM
hashes, plus a native-vs-WASM cross-check — the same MIDI rendered by the
arm64 clang binary and by the Emscripten build under Node hashes identically.

## Layout

```
src/synth/       oscillator, envelopes, SVF filter, LFO/mod matrix, voices,
                 patches, effects, engine   (audio path: integer-only)
src/midi/        SMF parser, tempo map (cumulative sample anchors)
src/track/       .mstk baked-track format (bake + load, tree-shaking)
src/sequencer/   song, sequencer (virtual clock), music_director, SPSC queue
src/output/      offline WAV renderer + render CLI, sha256
src/bindings/    embind API (WebSynth) for the WASM build
content/         gm_bank.json (37 patches + fallback), gm_kit.json (drum kit),
                 dnb_bank.json + dnb_kit.json (liquid DnB voices, all synthesised),
                 music/dreamy_dnb.msproj + .mstk (the demo track)
tool/            web tools: patch editor, track cutter, DAW (AudioWorklet hosts the WASM engine)
tool/lib/        shared panels (synth client, patch editor, cutter, scope, MIDI input)
tool/daw/        the DAW: project model, JS SMF, arrange, piano roll, recorder, cues, master
src/output/fit.h patch fitting (finite-difference search); CLI + the DAW's Fit tab share it
electron/        desktop shell: app:// protocol, native menu, file dialogs (docs in electron/README.md)
mobile/          Android shell (Capacitor): same payload, WebView at https://localhost
test/unit/       265 tests / ~27.5k checks
test/golden/     reference MIDI + pinned PCM hashes
```

## Build & test

```sh
cmake -S . -B build -G Ninja && ninja -C build   # native lib + CLIs + tests
./build/midisynth_tests                          # unit suite (265 tests)
./scripts/build_wasm.sh                          # tool/dist/midisynth.mjs (repo emsdk)
./scripts/run_goldens.sh                         # determinism gate (+wasm cross-check)
node test/smf_roundtrip.mjs                      # DAW model <-> SMF round trips (node)
./scripts/ci.sh                                  # all of the above
node test/tool_smoke.mjs                         # browser smoke tests (need
node test/daw_smoke.mjs                          # `node tool/serve.js` running,
                                                 # Chrome + puppeteer-core in the cwd)
```

Render a song offline, or play it live through the OS audio device
(miniaudio, vendored in `third_party/`); `--fx` loads a §13 song-level effect
config (examples in `content/fx/`):

```sh
./build/midisynth_render song.mid out.wav \
    --bank content/patches/gm_bank.json --drums content/drums/gm_kit.json \
    [--fx content/fx/cathedral.json] [--master content/fx/master_default.json] \
    [--seconds N] [--loop] [--dither] [--hash]

./build/midisynth_play song.mid --bank ... --drums ... [--fx ...] [--master ...] [--loop] \
    [--tempo milli] [--volume milli] [--seek ms]
# prints live position, voices, steals/drops, and render-callback CPU
# min/avg/max per second (plan §11 diagnostics)
```

Native integration goes through the plain C API
(`src/bindings/c_api.h`) — `midisynth_play` is the reference embedding, and a
unit test pins the C path bit-for-bit against the C++ path.

Authoring tool — visual patch/kit editor: live output scope + spectrum,
per-generator parameter panels with waveform previews, a **layer-mix view**
(each layer's wave color-coded with the weighted sum, plus an overlay of all
amp envelopes), draggable ADSR envelope canvas, filter response curve with
the 12 kHz cap marked, A/B snapshot compare, undo (Ctrl+Z), init patch, and
a two-way JSON drawer. Live audition via on-screen/computer/Web MIDI
keyboard (drum-pad grid in kit mode), per-song instrument list with live
activity, in-browser offline render with progress, voice meter, song
transport with per-channel mutes. Edits hot-swap a single program — only
voices on the edited patch restart. Export the current patch or the whole
bank/kit; import replaces the loaded document:

```sh
node tool/serve.js            # then open http://localhost:8930/tool/
```

## Hosting the tools

The DAW, patch editor and cutter are static files — host them anywhere:

```sh
./scripts/build_wasm.sh          # once: the engine module
./scripts/package_web.sh         # -> dist-web/ + dist-web.tar.gz
node dist-web/serve.js 8930 --root dist-web    # local check
```

Deploy `dist-web/` with any static server. Two hard requirements: **HTTPS**
(browsers gate AudioWorklet and Web MIDI behind a secure context; localhost
is exempt) and `.mjs` served as `text/javascript`. No COOP/COEP headers are
needed. Ready-made options in `deploy/`: an nginx site config and a
`Caddyfile` site (both written as name-based vhosts, so the synth can live
on a subdomain beside other services on the same box), a `Dockerfile`
(nginx:alpine), and a systemd unit for the node fallback server. Everything
runs client-side — the server never executes anything, which is also why the
same bundle is what the desktop app ships.

### Android app

```sh
./scripts/package_android.sh     # -> mobile/android/app/build/outputs/apk/debug/app-debug.apk
```

Capacitor wraps the same bundle; the WebView serves it from
`https://localhost` so AudioWorklet and WASM keep their secure context.
Needs the Android SDK and JDK 21 (the script borrows Android Studio's).
Saving goes through the Filesystem plugin into `Documents/` — a WebView has
no download manager. The phone layout is one media query in `tool/daw.html`,
shared with the browser build. No Web MIDI in a WebView; the UI already says
so and everything else works.

### Desktop app

```sh
cd electron && npm install       # once: electron + electron-builder
npm start                        # the working copy, as an application
cd .. && ./scripts/package_electron.sh    # .dmg / .exe / .AppImage in electron/dist/
```

The Electron shell serves the identical `package_web.sh` bundle over a
private `app://digidream/` origin (no port, no `file://` — ES modules and the
AudioWorklet need a real secure origin) and adds what a browser cannot: a
native menu, Save/Save As over the open `.msproj`, native open/import
dialogs, one folder picker for slice exports, `.msproj`/`.mstk` file
associations, granted Web MIDI, and boot without the Start-audio click. The
page code is shared: `tool/lib/desktop.js` uses the shell when it is there
and falls back to `<a download>` / `<input type=file>` in a browser.
`electron/README.md` has the details.

## Baked tracks (.mstk)

The shipping format: `midisynth_bake` compiles MIDI + bank + kit + FX into
one self-contained, integrity-sealed binary track — pre-parsed events (no
SMF parsing or tick math at load), tree-shaken content (only the instruments
the song actually plays), baked loop points, and an optional embedded
reference-render sha256 that `midisynth_bake --check` re-verifies on any
machine. Every consumer (render/play CLIs, C API `msynth_load_track`, WASM,
the tool — which also has a **Bake .mstk** button) accepts `.mstk` wherever
it accepts `.mid`. Corrupt tracks are rejected outright. Format spec:
[docs/TRACK_FORMAT.md](docs/TRACK_FORMAT.md). The golden harness proves a
baked track renders bit-identically to its sources.

**Track cutter** (`/tool/cutter.html`): slices baked tracks into smaller
gameplay-ready pieces — bar-snapped region selection on a density timeline,
loop audition, crossfade audition at any fade length, per-slice export.
Slices are state-chased at their start (they open sounding exactly like the
parent did there), close cross-boundary notes, loop whole by default, and
inherit the parent's content verbatim so sibling slices are always
crossfade-compatible (`msynth_crossfade_track` is the runtime counterpart).

**Mastering** is part of the deliverable: a deterministic fixed-point chain
(3-band EQ → compressor → stereo width → gain → lookahead limiter) inside
the engine, `master.json` / `--master` on the CLIs, baked into the `.mstk`
(`MSTR` section) so the game plays the mastered result bit-identically.
Bypassed by default — the goldens are unchanged.
[docs/PATCH_FORMAT.md §5.1](docs/PATCH_FORMAT.md). `midisynth_bake --to-mid
track.mstk out.mid` writes a track's song back out as a Standard MIDI File.

## DAW (`/tool/daw.html`)

One page, in-page tabs — **Arrange · Track · Synth · Master · Cut & Loop ·
Cues** — on the same engine: 1 track = 1 MIDI channel, piano-roll editing
that hot-swaps the playing song **keeping the position** (the engine diffs
note/controller state at the playhead instead of cutting), recording from a
MIDI keyboard with count-in, click track, quantize and overdub/replace, the
patch editor and the cutter hosted as tabs, the mastering chain with meters,
and a **cue tester**: pieces (slices, the whole project, imported `.mstk`)
can be queued to take over **exactly when the current piece finishes its
loop** (or at the next bar), crossfaded, or chained ("then → B after N
loops") the way the game will script them; `cues.json` exports the board.
Projects are `.msproj` JSON holding everything (bank, kit, fx, mastering,
slices, cue rules); bank/kit/fx/master are also imported/exported as
standalone files. Runs in a browser, on a static host, or as a standalone
desktop app (`electron/`). [docs/DAW.md](docs/DAW.md).

## Patch / kit / FX JSON

**Full field-by-field reference: [docs/PATCH_FORMAT.md](docs/PATCH_FORMAT.md)**
(every field's range, default and behavior, all five wavetable generators, a
recipe cookbook). Highlights beyond the plan's §13 example:

* **Five wavetable generators**, all band-limited per mip (the nonlinear ones
  go through DFT analysis + capped resynthesis, so no recipe can alias):
  `additiveHarmonicMorph`, `pulse` (PWM morph), `drawbar` (organ),
  `waveshaper` (soft/hard/fold drive morph), `fm2op` (2-op FM index morph).
* `osc: "wavetable" | "noise"` per layer — snare/hi-hat recipes are noise
  layers through the SVF.
* `pitchNote` per layer: fixed synthesis pitch (fractional MIDI note) — drum
  layers use it; the MIDI note then only *selects* the drum.
* `velCurve` per layer: `squared` (default) / `soft` / `linear` velocity law.
* Envelope depth fields resolve to implicit mod routings:
  `filterEnv.depth`, `pitchEnv.depthSemis`, `wtPosEnv.depth`.
* Drum patches add `oneShot` (note-off immune) and `exclusiveGroup` (choke).

## Deliberate deviations from the plan

* **Mip table sizes floor at 64 samples** (`MIN_TABLE_SIZE`), not a pure
  halving to 8: 4-point Hermite on an 8-sample cycle leaves interpolation
  spurs at ~−32 dB (audible hash on top-octave notes); 64 keeps them below
  ~−66 dB, pinned by a regression test. Costs ~150 samples per frame.
* **Web threading**: the plan's §12 named `-sAUDIO_WORKLET=1`, but that flag
  *requires* SharedArrayBuffer + COOP/COEP — the very dependency §12 wanted to
  avoid. Instead the whole engine runs inside the AudioWorklet via a
  single-file ES6 module import; the main thread drives it over the
  MessagePort. No special headers anywhere.
* `k = 2.0` (zero resonance) doesn't exist in Q2.30's ±2 range; it clamps to
  `0x7FFFFFFF` (Q = 0.5 + 1 ulp — indistinguishable).
* Baked tables are generated offline by `scripts/gen_tables.py` and committed
  (`tables_baked.inc`), engine-style, rather than computed at startup — the
  strongest possible "identical on every target".

## Golden regeneration

Intentional DSP changes invalidate hashes. `scripts/run_goldens.sh --regen`
re-renders everything, writes new hashes, and drops review WAVs into
`test/golden/review/` — **listen to them** before committing (plan §14: the
regeneration process never weakens the cross-platform check, it only moves
the reference point).

## Status vs the plan's milestones

All 20 build-order steps are done. Phases A/B (steps 1–15) ✓; Phase C:
offline render + golden harness (16) ✓, WASM + worklet (17, message-port
variant) ✓, embind + C API bindings + music director (18) ✓, authoring tool
(19) ✓ — visual editors (draggable envelopes, wavetable preview, live scope,
drum pads, per-song instrument navigation, in-browser offline render) plus
Web MIDI input; content (20): fallback + 37-patch curated set + GM kit +
song FX configs, iterated against a real orchestral SMF — content work is
ongoing by nature, which is what the tool is for.

Beyond the plan (Sep 2026) — everything below is engine-side, deterministic
fixed point, and defaults to an exact no-op unless a patch/config asks for it:

* **Live music direction**: song replacement under a running transport
  (state-diffed, never cut), cue scheduling at loop end / next bar
  (`Sequencer::queue_next`, `MusicDirector::transition`), piece-switch
  callbacks; an SMF writer (`exportSmf`, `midisynth_bake --to-mid`).
* **Mastering**: EQ → compressor → bus saturation → width → gain → lookahead
  limiter, baked into `.mstk` (`MSTR`), meters; bypass is byte-identical.
* **Generators**: `custom` (draw/import a cycle), `harmonics` (64-partial
  histogram), `hardsync`, `chebyshev` (drag-the-curve editor), extended
  `waveshaper` (inputs, bias, crush/stairs), plus the original five.
* **Per-layer synthesis**: unison stacks, ring mod (internal carrier),
  pre/post-filter saturation, an independent second filter
  (serial/parallel, comb resonator and vowel-morphing formant modes),
  routable filter drive, 4 LFOs with tempo sync, DAHDSR + looping
  envelopes, per-note aftertouch (end to end), release velocity, layer
  start delay (flams), round-robin groups, glide (CC5/65), microtuning.
* **Mix & sends**: per-channel buses with 3-band channel EQ (flat = exact
  bypass), a delay/echo send (CC94, ping-pong) beside reverb and chorus.
* **Precision**: envelopes and voice gain ramps in Q8.24 with per-sample
  wavetable-morph interpolation — quiet exponential tails glide instead of
  stair-stepping — and the 16-bit output stage uses first-order
  error-feedback rounding, so sub-LSB detail from many summed quiet voices
  survives instead of floor-truncating away (goldens regenerated for both,
  deliberately). Envelope stages accept DX7-style `attackRate` 0..99 with a
  documented absolute anchor, plus `keyScaleRate` (faster envelopes up the
  keyboard).
* **Tools**: the DAW (`tool/daw.html`, `docs/DAW.md`) with its own model and
  browser tests; shared `tool/lib/` panels behind all three pages.

Still deferred by design: true cross-layer modulation — voices are
architecturally independent; the internal-carrier ring mod covers the
classic sound.

Explicitly deferred (the plan's own open decision): mip crossfading (the
interface allows it; v1 selects mips at note-on). The plan's other open
decision — additional wavetable generators — is resolved well past the
original ask: nine generators ship (additive, pulse, drawbar, waveshaper,
chebyshev, harmonics, hardsync, fm2op, custom). Formerly out of scope but
since implemented: portamento (CC5/CC65 glide). Still out of scope per the
plan: SFX playback, game-runtime MIDI input.
