# DigidreamSynth DAW — `tool/daw.html`

A mini-DAW on the deterministic engine, in one browser tab: **Arrange · Track
(piano roll) · Synth · Master · Cut & Loop · Cues**, a global transport and a
File menu. Everything you hear comes from the same WASM engine that the game
runs, and everything you export (`.mid`, `.mstk`, `.wav`, slices, `cues.json`)
is produced by that engine, so the deliverable is what you auditioned.

```
node tool/serve.js            # http://localhost:8930/tool/daw.html
```

The page is self-contained (relative URLs, one ES module bundle for the WASM,
no server features beyond static files); `tool/serve.js` is only a static
server. The same files ship three ways:

* **browser** — `node tool/serve.js`
* **hosted** — `./scripts/package_web.sh` produces a `dist-web/` bundle +
  tarball for any static host (HTTPS required for AudioWorklet/Web MIDI;
  `deploy/` has nginx, Docker and systemd recipes)
* **desktop** — `./scripts/package_electron.sh` builds a standalone app
  (see [Desktop app](#desktop-app) and `electron/README.md`)
* **android** — `./scripts/package_android.sh` builds an APK
  (see [Android app](#android-app))

## Model

* **1 track = 1 MIDI channel** (≤ 16; channel 10 is the drum kit). A track
  has a name, colour, GM program, mute/solo/arm, and its notes
  (`{tick, dur, note, vel}`), CCs and pitch bends.
* Tempo map, time signatures, a loop region, the embedded **bank**, **kit**,
  **fx** (reverb/chorus) and **master** (mastering chain), remembered slices
  and cue rules — all in one **`.msproj`** JSON file (`File › Save`).
* The engine never sees the model directly: on every edit the project is
  serialized to a Standard MIDI File (`tool/daw/smf.js`; conductor track +
  one track per channel, loop markers) and **hot-swapped into the running
  sequencer keeping the musical position** (`replaceSong`: the engine diffs
  note and controller state at the playhead — a deleted sounding note gets a
  normal release, an added note that straddles the playhead is struck, a
  program change is applied; never a hard cut). Muted tracks are simply left
  out of the playback SMF; the metronome is a click track on channel 10 that
  only exists in the playback SMF.

## Tabs

**Arrange** — track list (name, channel, program, M/S/R, ♪ = edit in Synth,
×), note lanes, bar ruler; drag on the ruler sets the loop region (bar
snapped), click a lane to move the playhead, double-click to open the Track
tab. Lit dots show which tracks the engine is triggering right now
(`status.activeNotes`).

**Track** — piano roll: `draw` (drag on empty = new note, length follows the
drag), `select` (box / shift-click), `erase` (or right-click). Drag a note to
move it, its right edge to resize; ⌫ deletes the selection, ⌘/Ctrl+D
duplicates it after itself. Snap comes from the header (`1/1 … 1/32`,
triplets, off). The bottom lane edits velocity, a CC (1/7/10/11/64/91/93) or
pitch bend (CC94 is the delay send). The left keyboard auditions on the
track's channel.

**Record** (inside Track) — pick a device (or all), arm a track (R), press
**● Rec** (or `R`). With *count-in* > 0 the metronome plays that many bars
first: the playback SMF is built with every event shifted later and clicks in
front, so recording timing is real song time. Web MIDI timestamps are mapped
through the 10 Hz engine status anchor (`performance.now()`) and the project
tempo map to ticks; *latency ms* shifts them; *quantize* snaps on commit;
*overdub* adds to the track, *replace* clears the loop region first. Stop
commits the take as one undo step. Live input is always monitored through
the track's channel.

**Synth** — the patch editor (`tool/lib/patch-editor.js`, the same one as
`index.html`) hosted as a tab. Opening it selects **the instrument the
selected track plays** (so what you edit is what you hear), the hint line
names every track playing the edited patch — or warns in amber when none
does — and the instrument list shows a live green dot on tracks the engine
is sounding right now. Picking a program in the dropdown makes the selected
track play it; edits go into the project's embedded bank/kit. Import/export
of `bank.json` / `kit.json` is in the File menu.

**Master** — the engine's mastering chain (`docs/PATCH_FORMAT.md` §5.1): EQ
curve with draggable handles (shift-drag the bell for Q), compressor with a
gain-reduction meter, **bus saturation**, lookahead limiter, width, output
gain, peak/RMS meters, 3 s RMS loudness, clip counter, dither, hold *A/B
bypass* to compare. Off by default — bypass is byte-identical to an
unmastered engine. Below it: **per-channel EQ** (mixer strips — pick a
track, three bands; flat costs nothing) and the song send effects
(reverb, chorus, and the **delay/echo send** with time/feedback/wet and
ping-pong; route it per track with the CC94 lane). Everything here is baked
into the `.mstk`.

**Cut & Loop** — *Bake project → load here* bakes the whole project in memory
(bank/kit/fx/mastering embedded) and opens it in the track cutter
(`tool/lib/cutter-panel.js`): drag a bar range, loop it, crossfade into it,
add it as a slice. Slices are gameplay-ready `.mstk` files (state-chased
opening, cross-boundary notes closed, loop whole) and automatically become
**cue pieces**; the project remembers their ranges and re-cuts them when you
bake again after loading a saved project.

**Cues** — the piece-combination tester. Pieces = slices, the whole project,
imported `.mstk` files. Per piece: **▶ now**, **⇥ at loop end** (the change is
queued and executes exactly when the current piece finishes its loop — the
"Now playing" card counts down `msToBoundary`), **⇥ next bar**, **⇄ fade**
(crossfade). *Cancel queued* aborts a pending change. Chain rules — *then →
B after N loops (at loop end / next bar)* — are executed by the tab on the
engine's piece-switch notification and loop wraps, exactly the way a game
would script them; the transition log records each boundary switch with the
song time. *Entry piece* marks where a level starts. `File › Export
cues.json` writes:

```json
{ "version": 1, "entry": "intro",
  "pieces": [{ "name": "intro", "file": "intro.mstk", "loop": true, "lengthMs": 8000 }, ...],
  "transitions": { "intro": { "then": "loopA", "after": 1, "mode": "loopEnd" } } }
```

a superset of the client's `name → url` cue table (`client/src/audio/music.js`
can adopt `queueTrack` at loop end through one worklet message; today it
crossfades).

## Files

| Action | Result |
|---|---|
| Save / Open | `name.msproj` (everything, including bank/kit/fx/master) |
| Import `.mid` | tracks/tempo/time signatures/loop replace the project's; content stays |
| Import `.mstk` | the engine exports its SONG as SMF (`exportSmf`) and hands over its embedded bank/kit/fx/mastering |
| Import `bank.json` / `kit.json` / `fx.json` / `master.json` | replaces that part of the project |
| Export `.mid` | format 1, conductor + one track per channel, loop markers |
| Bake `.mstk` | `bakeTrack` in the engine with bank/kit/fx/**master** — the game deliverable |
| Render `.wav` | offline render worker with the same content, fx and mastering |
| Export slices / cues.json | from Cut & Loop / Cues |
| Export `bank.json` / `kit.json` / `fx.json` / `master.json` | standalone documents |

Drag & drop any of these onto the page.

Long operations show progress: the start overlay runs a staged boot bar
(engine compile → instruments → wavetables → session), and a bottom-right
toast tracks `.wav` renders (percentage + cancel) and `.mstk` bakes
(indeterminate).

## Fit

The **Fit** tab points the engine at a recorded sample and searches for synth
parameters whose render matches it, then drops the result straight into the
bank or the kit. Same optimiser and loss as `midisynth_fit`, compiled into the
WASM build and run in a worker (`tool/fit-worker.js`), so a fit done in the
browser is the fit the CLI would produce.

There is nothing to backpropagate through -- the engine is fixed-point integer
arithmetic -- so gradients are **finite differences**, one extra render per
parameter per step. That is affordable because the renders happen in-process.

**Architecture** is a preset, not a search. Oscillator types, filter modes,
layer count and the wavetable recipe are fixed by the chosen preset and only
the continuous knobs move: those discrete choices are exactly the ones a
gradient cannot cross.

| percussive (fixed pitch, rendered as a kit note) | melodic (bank program, keyboard-tracking) |
|---|---|
| `kick`, `snare`, `hat`, `bass` | `piano`, `strings`, `pad`, `organ`, `brass`, `pluck`, `bell`, `lead` |

Melodic presets are rendered as a bank program at a fixed note and hold the
key for part of the window so the release is part of the fit; they carry a
`detuneCents` parameter (±2 octaves) so a target recorded at another pitch can
still be matched, and the patch that comes out tracks the keyboard. **Give them
a single sustained note as the target** -- a phrase of several notes cannot be
matched by one note and the fit will be poor in a way the loss will not warn
you about. `window ms` limits the
fit to the first N ms, because a recording's room tail is not something the
synth can reproduce and fitting it wastes the budget. Prefer **restarts** over
**iters**: descent on this landscape saturates after ~20 steps, so more
iterations buy nothing while more restarts do.

**Likelihood** is built in the panel rather than baked in, because the choice
of objective decides what "best fit" even means:

| term | what it measures |
|---|---|
| spectrogram | multi-resolution log-magnitude STFT distance |
| envelope | amplitude envelope in dB, so decays line up |
| band balance | gross octave-band distribution: *where the energy is* |

A pure spectrogram term treats a near-silent bin as equal to the fundamental
and will settle on a sound in entirely the wrong octaves -- that really
happened here, twice, before the band term existed. The preview plots target
against fit per band and breaks the loss out per term, so a fit that scores
well while sitting in different bars is visibly the loss being gamed rather
than a match. Four weight presets cover the usual cases.

**Into the roster**: pick bank program or kit note, name it, and *Add to
instruments* writes it into the project's bank/kit as one undoable edit -- the
Synth tab and the sequencer see it immediately. The patch JSON is editable in
place and re-scored against the same target when you change it.

## Groove

Nothing in the engine swings; the DAW does it **when the project is
serialised to a SMF**, which is also what the bake consumes — so a swung
project bakes swung, with no engine change and no format change. The model
itself never moves: the notes stay on the grid you drew them on, and turning
swing back to 0 gives you the straight pattern again.

`swing` in the header delays every second subdivision of its grid; 100% is a
full triplet shuffle (a `1/16` pair landing 2:1). Note starts shift and their
lengths ride along; CC, bend and the metronome stay put. Stored in the
`.msproj` as `groove: {amount, grid}`.

## Desktop app

`midisynth/electron/` wraps the DAW in Electron: one window, the same engine,
no server and no browser chrome.

```
cd midisynth/electron && npm install    # once
npm start                               # runs the working copy
./scripts/package_electron.sh           # .dmg / .exe / .AppImage in electron/dist/
```

The page is served from a private `app://digidream/` origin rather than
`file://` (ES modules, the AudioWorklet and its `fetch()`es all need a real
secure origin) with no port open. Unpackaged runs serve `midisynth/` itself,
so `npm start` shows the working copy; packaged apps serve the staged bundle
from inside `app.asar`.

What the desktop adds, all of it optional — `tool/lib/desktop.js` detects
`window.digidream` and otherwise falls back to the browser behaviour, so one
codebase serves both:

| | |
|---|---|
| Native menu | File / Edit / Transport / View (tabs) / Window / Help, driving the same action table as the in-page File menu (which hides itself) |
| Save / Save As | Save writes over the open `.msproj`; the title bar, the macOS edited dot and the proxy icon follow the document |
| Open / Import | native dialogs; the log reports the real path |
| Slice export | one folder picker instead of one dialog per slice |
| File associations | double-click a `.msproj` (or `.mstk`) to open it; a second launch hands the file to the running app |
| Audio & MIDI | the shell grants Web MIDI and boots the engine without the Start-audio click |

## Android app

`midisynth/mobile/` is a [Capacitor](https://capacitorjs.com) shell around the
same payload the web and desktop builds use.

```
./scripts/package_android.sh              # debug APK
./scripts/package_android.sh --release    # unsigned release APK
./scripts/package_android.sh --stage-only # refresh mobile/www/ only
adb install -r mobile/android/app/build/outputs/apk/debug/app-debug.apk
```

Capacitor serves the bundle from `https://localhost` inside the WebView, so
the AudioWorklet and WASM engine get their secure context; `index.html` is a
redirect into `tool/daw.html` (its URLs are relative to `tool/`). Needs the
Android SDK (`ANDROID_HOME`, default `~/Library/Android/sdk`) and JDK 21 —
the script borrows Android Studio's if the default `java` is older.

Saving is the one thing a WebView cannot do like a browser: there is no
download manager, so `tool/lib/desktop.js` routes it to the Filesystem plugin
and files land in `Documents/` (`/storage/emulated/0/Documents`). Web MIDI
does not exist in a WebView; the MIDI panel already reports that and
everything else works without it.

The compact layout keys off the **size of the viewport, not the device**:
`max-width: 700px` *or* `max-height: 520px`. A phone in landscape is 844 wide
but under 420 tall, so width alone would drop the compact layout exactly when
vertical space is scarcest; a desktop window dragged small gets it too. Under
520 px of height the header stays **one line whether collapsed or expanded**
(it scrolls sideways to reach the rest) and the log keeps its one-line strip,
because there every row of chrome costs a third of the sheet.

On a phone the **header and the log start collapsed** to a single line each
and expand on tap (the chevron at either end). The header keeps the transport
— play, stop, position — visible while collapsed, because that is what you
reach for mid-take; collapsing it to nothing would make the bar tidy and
useless. Collapsed, the two bars cost 50 px instead of 232, which is the
difference between seeing two bars of the arrangement and seeing six. The
state is per-project (`view.hdrOpen` / `view.logOpen`) and the classes do
nothing above 700 px, so desktop is untouched by the same markup.

**Mobile layout** is one `@media (max-width: 700px)` block in `tool/daw.html`
— compact header, tabs on one swipeable row, wide grids stacked, mouse-only
hints hidden, safe-area insets for Android 15's edge-to-edge. No separate
mobile UI and no JS: every surface already uses pointer events with
`touch-action: none`, so touch worked as-is. It applies to the browser and
desktop builds too, at a narrow enough window.

## Keys

**Touch**: the sheet sets `touch-action: none` (a draw gesture must not scroll
the page away), so **two fingers** pan and pinch-zoom the roll instead —
horizontally through time, vertically through pitch, spread to zoom, all
measured from the gesture's start so it cannot drift. A second finger landing
mid-drag abandons that drag rather than committing it. **Hold a note** to
delete it: a touch screen has no right-click, and switching to the erase tool
for a single note is a detour. The piano-roll hint line swaps to the touch
version under the compact layout, since the mouse one names gestures that do
not exist there.

In the CC/bend lane: drag paints, **shift-drag writes a straight ramp**
between the two ends at grid resolution (the 32-bar filter sweep), and the
erase tool or right-click **deletes** points.

Space play/stop · `R` record · ⌘/Ctrl+Z / ⇧⌘Z undo/redo (project; the Synth
tab's own patch undo while it is active) · ⌘/Ctrl+S save · piano roll: `D`
`S` `E` tools, ⌫ delete, ⌘/Ctrl+D duplicate, wheel scroll, ⌘/Ctrl+wheel zoom.

## Engine features the DAW leans on

* `Sequencer::replace_song_keep_position` / `MusicDirector::replace_song`
  (live edit), `queue_next` / `transition(AtLoopEnd | AtNextBar | Immediate)`
  (cue changes), piece-switch callback, `samples_to_boundary`.
* `smf_write` (`exportSmf`, `midisynth_bake --to-mid`).
* `MasterChain` + `MasterParam` command + `master.json` + `MSTR` track
  section + meters.
* Worklet protocol: every request carries a `requestId` echoed on its
  replies (`tool/lib/synth-client.js` turns them into promises).

## Tests

* `node test/smf_roundtrip.mjs` — model ↔ SMF round trips, tempo math, undo,
  golden corpus stability, engine cross-check through `--to-mid`.
* `node test/electron_smoke.mjs` (`--packaged` for the built app) — launches
  the desktop shell and checks the page loads from `app://`, the engine boots
  with the audio clock actually advancing, the preload bridge is exposed and
  saving writes a real file (needs `electron/node_modules`; skipped
  otherwise).
* `node test/android_smoke.mjs` — drives the installed app through the debug
  WebView's DevTools over adb: served from `https://localhost`, engine boots,
  transport clock advances, saving writes a real file, phone layout applied
  (needs a device/emulator with the APK installed; skipped otherwise).
* `node test/daw_smoke.mjs` — headless Chrome: boot, play, edit while
  playing, piano-roll pointer edits + undo, synthetic MIDI recording,
  mastering + meters, bake + cut, queue at loop end → switch at the
  boundary, exports (needs `puppeteer-core` in the working directory and
  Chrome; skipped otherwise).
