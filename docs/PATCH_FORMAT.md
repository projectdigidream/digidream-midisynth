# DigidreamSynth JSON formats — full reference

Three JSON file kinds feed the engine, all loaded on the control thread and
converted to fixed point before any audio code sees them:

| File | Loaded by | Purpose |
|---|---|---|
| **Patch bank** (`gm_bank.json`) | `load_patch_bank` | melodic instruments per GM program number + the fallback patch |
| **Drum kit** (`gm_kit.json`) | `load_drum_kit` | one mini-patch per MIDI note on channel 10 |
| **Song FX config** (`content/fx/*.json`) | `load_song_config` | per-song reverb/chorus character (+ optional mastering) |
| **Master config** (`master.json`) | `load_master_config` | standalone mastering chain preset (same object as `fx.master`) |

General rules that apply to *everything* below:

* **Numbers must be plain decimals** — `1200`, `0.35`, `-7.5`. Exponent
  notation (`1e3`) is rejected: numbers are parsed with integer math straight
  into Q16.16 fixed point so a platform's float parser can never affect the
  deterministic output. About 4–5 decimal places survive the conversion.
* **Never crash, always produce something**: a bad value is clamped with a
  warning; a broken layer falls back to a plain sine; a broken patch resolves
  to the fallback patch; unknown keys are ignored. Loaders return
  `{ok, warnings[], errors[]}` — the tool's log shows them verbatim.
* **`version`**: every top-level file carries `"version": 1`. Unknown versions
  load best-effort with a warning.
* Time values are **milliseconds**, resolved to 64-sample control blocks
  (≈1.33 ms each); durations shorter than one block round down (0 = instant).
* Gains/levels are linear in `0..1` unless stated otherwise.

---

## 1. Patch bank file

```json
{
  "version": 1,
  "fallback": { <patch> },
  "programs": {
    "0":  { <patch> },
    "48": { <patch> }
  }
}
```

* `programs` — object keyed by **GM program number as a string** (`"0"`–`"127"`).
  Any program not present resolves to `fallback` at note-on: unknown programs
  always produce *something musical*, never silence.
* `fallback` — a full patch. If missing or broken, a built-in soft pad is used.

## 2. Drum kit file

```json
{
  "version": 1,
  "notes": {
    "36": { <patch> + drum fields },
    "42": { <patch> + drum fields }
  }
}
```

* Keys are **MIDI note numbers** (`"27"`–`"87"` covers GM percussion).
* A note *not* in the kit is **silent by design** (unlike melodic programs).
* Drum patches accept the same schema as melodic patches, plus the drum
  fields below, and typically use `pitchNote` per layer since the MIDI note
  chooses the drum, not the pitch.

---

## 3. The patch object

```json
{
  "name": "warm_pad",
  "oneShot": false,
  "exclusiveGroup": -1,
  "layers": [ { <layer> }, ... ]
}
```

| Field | Type / range | Default | Meaning |
|---|---|---|---|
| `name` | string | `"unnamed"` | display name (tool, logs) |
| `layers` | array, **1–4** entries | *required* | oscillator layers; a note-on starts one voice per matching layer. Extra layers are dropped with a warning; an empty array is an error. |
| `oneShot` | bool | `false` | drums: envelope runs Attack→Decay→done, **note-off is ignored**. Forces every layer's amp sustain to 0 (a one-shot that sustained would leak its voice forever — clamped with a warning). |
| `exclusiveGroup` | int `-1..15` | `-1` | choke group: a new note in the group fast-releases (~4 ms) everything else in it on the same channel. Classic use: closed hi-hat (42) chokes open hi-hat (46). `-1` = no group. |

### 3.1 The layer object

Every field is optional; an empty layer `{}` is a valid plain sine.

```json
{
  "osc": "wavetable",
  "wavetable": { "generator": "...", "frameCount": 8, "params": { ... } },

  "detuneCents": -7,
  "mixGain": 0.5,
  "pan": 0.0,
  "velLow": 0, "velHigh": 127,
  "velCurve": "squared",
  "pitchNote": -1,
  "startDelayMs": 0,

  "ampEnv":    { ... }, "filterEnv": { ... },
  "pitchEnv":  { ... }, "wtPosEnv":  { ... },

  "filter": { "mode": "lowpass", "cutoffHz": 1200, "resonance": 0.3,
              "slope": 12, "drive": 1 },
  "filter2": { "mode": "highpass", "cutoffHz": 150, "resonance": 0 },
  "filterRouting": "serial",
  "postDrive": 1,
  "keyTrackFilter": 0.5,
  "unison": { "voices": 1, "detuneCents": 7, "spread": 0.5 },
  "ring": { "ratio": 0.5, "amount": 1.0 },
  "releaseVel": 0,
  "rrGroup": 0,

  "lfos": [ { "shape": "sine", "rateHz": 5.0, "delayMs": 300 } ],
  "modMatrix": [ { "src": "LFO1", "dst": "Pitch", "amount": 0.02 } ]
}
```

#### Oscillator source

| Field | Type / range | Default | Meaning |
|---|---|---|---|
| `osc` | `"wavetable"` \| `"noise"` \| `"metal"` | `"wavetable"` | `noise` is a noise source (deterministic per-voice xorshift; seed derives from pool slot + note, so output never depends on allocation history) — right for snares, wind, breath. `noiseColor` tilts it: `"white"` (flat, the default and bit-identical to before the field existed), `"pink"` (−3 dB/oct — closer to a real snare's wash and to cymbal air), `"brown"` (−6 dB/oct — rumble, wind, the bed under a breakdown). `metal` is **six detuned square waves at the TR-808's inharmonic ratios** (1 / 1.342 / 1.2312 / 1.6532 / 1.9523 / 2.1523 × the voice pitch): the classic cymbal/hi-hat source — bandpass it around 4–8 kHz for hats, wider + long decay for crashes; `pitchNote` sets the cluster's base (≈ 60–80 works). Both ignore `wavetable` and, like heavy `postDrive`, are deliberately not band-limited. |
| `wavetable` | object | plain sine | see §4 (generators). A missing/broken recipe substitutes a sine with a warning. |

#### Mix & pitch

| Field | Type / range | Default | Meaning |
|---|---|---|---|
| `detuneCents` | number (cents, fractional ok) | `0` | added to the played pitch. Layer detune (±5–10¢) is the ensemble/thickness tool; ±1200 adds an octave layer. |
| `mixGain` | `0..1` | `1.0` | layer level into the mix. A single full-velocity voice at `1.0` peaks near full scale — multi-layer patches usually sit at `0.3–0.6` per layer. |
| `pan` | `-1..1` | `0` | constant-power pan; adds with the channel's CC10 and any `Pan` mod routing, clamped to ±1. |
| `velLow`, `velHigh` | `0..127` | `0`, `127` | velocity window: the layer only sounds for velocities inside it (swapped with a warning if reversed). Use for velocity-split patches (soft layer 1–90, bright layer 91–127). |
| `velCurve` | `"squared"` \| `"soft"` \| `"linear"` | `"squared"` | velocity→gain law. `squared` = `(v/127)²` (GM-ish, expressive); `soft` = `(v/127)^1.5`; `linear` = `v/127` (flattest response, good for organs/pads that shouldn't dip much at low velocity). |
| `pitchNote` | MIDI note, fractional ok; `-1` | `-1` | **fixed synthesis pitch**: the layer always sounds at this note regardless of what was played. `-1` = follow the played note. Drum layers almost always set it (`36.5` is fine — half-semitones work). |
| `unison` | object | off | voice stack: `voices` (`1..7`), `detuneCents` (`0..100`, the widest voice's offset — symmetric across the stack), `spread` (`0..1` stereo width), `phaseSpread` (`0..1`, default `0`). The classic supersaw thickener; costs `voices` pool voices per note. **`phaseSpread`** starts the stack's oscillators at different points: at `0` they all begin aligned, so the attack is one coherent edge and the stack only beats once the detune has drifted the phases apart; at `1` it beats from the first sample — which is what a Reese bass wants. Deterministic (derived from the same per-voice seed as the noise). |
| `ring` | object | off | modulation against an internal sine carrier at `ratio` × the voice pitch (`0.25..16`, non-integer = clangorous). `"mode": "ring"` (default) mixes dry→full ring by `amount` `0..1`. `"mode": "fm"` **phase-modulates** the oscillator instead, by `index` radians `0..12` — real FM, so unlike the baked `fm2op` table the index can *move*: route an envelope to `RingAmount` and the tone barks on the attack and mellows, which is what makes a Rhodes a Rhodes. |
| `releaseVel` | `0..1` | `0` | how strongly MIDI **release velocity** scales release time (hard lifts shorten, soft lifts stretch, up to ±2×/÷8). `0` = classic fixed release. |
| `sendReverb`, `sendChorus`, `sendDelay` | `0..1` | inherit | per-layer send overrides. Drums share one MIDI channel, so CC91/93/94 can't differ per drum — set these on a drum's layers to give the snare its own wet level while the kick stays dry. Omitted = the channel's CC value. |
| `rrGroup` | `0..15` | `0` | **round robin**: layers sharing a group alternate one-per-note-on (deterministic per-channel counter). Two snare layers in group 1 = alternating samples-style variation. |
| `startDelayMs` | `0..2000` ms | `0` | the layer speaks this much later than the note-on (resolved to control blocks, ≈1.33 ms). The flam/echo tool: stack near-identical layers at `0/12/25 ms` for a real hand clap, offset a bell strike, double a tom. A note-off inside the delay is deferred until the layer has played its attack (so the flam speaks, then releases); a choke/steal during the delay kills it silently. |

#### Envelopes — `ampEnv`, `filterEnv`, `pitchEnv`, `wtPosEnv`

All four share one shape:

```json
{ "delay": 0, "attack": 5, "hold": 0, "decay": 200, "sustain": 0.7, "release": 300,
  "loop": false,
  "attackCurve": "linear", "decayCurve": "exp", "releaseCurve": "exp",
  "depth": 0.5 }
```

| Field | Type / range | Default | Meaning |
|---|---|---|---|
| `delay`, `hold` | ms ≥ 0 | `0` | full **DAHDSR**: `delay` waits at the current level before the attack; `hold` pins full level between attack and decay. |
| `loop` | bool | `false` | at the end of the decay the envelope re-enters the attack (A↔D cycles while the note is held); note-off still releases normally. Tremolo/pulsing pads without an LFO. Ignored when attack+decay are both instant. |
| `attackRate`, `decayRate`, `releaseRate` | `0..99` | — | DX7-style **rate** alternative to the ms keys, with the engine's absolute anchor `t_ms(R) = 40000 · 2^(−R/6.5)` (R 0 = 40 s, 50 ≈ 187 ms, 99 ≈ 1 ms; +6.5 halves the time). An explicit ms key wins over its rate for the same stage. |
| `keyScaleRate` | `-1..1` | `0` | **key-scaled rate**: every stage duration of this envelope is multiplied by `2^(−ks·(note−60)/12)` — at `1`, envelopes run twice as fast per octave above middle C (piano-like); negative slows them going up. Composes with release velocity. |
| `attack`, `decay`, `release` | ms ≥ 0 | `0` (amp default: attack ≈1.3 ms, release 50 ms) | stage lengths. **0 = instant**: a 0 ms attack + 0 ms decay reaches sustain within a single control block (drums rely on this). Stages terminate structurally — an `exp` release of 300 ms ends at exactly 300 ms, it never trails asymptotically. |
| `sustain` | `0..1` | `1.0` | held level. With patch-level `oneShot`, amp sustain is forced to 0. |
| `attackCurve` / `decayCurve` / `releaseCurve` | curve name | `linear` / `exp` / `exp` | per-stage shape: `linear`; `exp` (fast start, RC-style tail — the natural decay/release feel); `expAttack` (slow start, accelerating — swells); `sCurve` (smoothstep — soft both ends, nice string attacks). |
| depth field | see below | | how strongly the envelope modulates its destination. |

The four envelopes and their depth fields:

* **`ampEnv`** — the voice's loudness. No depth (always full). **The amp
  envelope reaching idle is what frees the voice** — every other envelope just
  holds its final value.
* **`filterEnv`** → cutoff. `depth`: `-1..1`, **fraction of the full cutoff
  range** (256 exponentially spaced steps, 20 Hz–12 kHz). `0.5` sweeps the
  cutoff up half the range at envelope peak; negative sweeps down. Default
  `0.5` *if the object is present*, `0` (inactive) if absent.
* **`pitchEnv`** → pitch. `depthSemis`: `-48..48` semitones (fractional ok).
  Positive = starts high, falls to base as the envelope decays — set
  `attack: 0` and the note *starts* shifted by `depthSemis`. Kick drums:
  `{ "attack": 0, "decay": 45, "sustain": 0, "depthSemis": 24 }`.
* **`wtPosEnv`** → wavetable morph position. `depth`: `0..1`, fraction of the
  frame range (default `1.0` when present). The envelope drags the oscillator
  through the wavetable's frames — this is how "brightness blooms in over
  400 ms" or "pluck decays from 24 harmonics to 3" is authored.

Retrigger behavior is uniform: every stage starts from the *current* level,
so re-striking a ringing note or releasing mid-attack never clicks.

Envelope levels run in **Q8.24 internally** (the mod matrix sees a Q16.16
view): a quiet exponential tail keeps ~8 bits of relative precision even at
−96 dB, the voice gain ramp is Q8.24 as well, and wavetable morph position
ramps per sample — so long soft releases glide instead of stair-stepping.

#### Filter

```json
"filter": { "mode": "lowpass", "cutoffHz": 1200, "resonance": 0.3, "slope": 24, "drive": 2.5 },
"keyTrackFilter": 0.5
```

| Field | Type / range | Default | Meaning |
|---|---|---|---|
| `mode` | `"lowpass"` \| `"bandpass"` \| `"highpass"` \| `"notch"` \| `"comb"` \| `"formant"` \| `"bypass"` | `"lowpass"` (when object present) | output tap of the per-voice SVF (`notch` = input minus the bandpass). `comb` and `formant` are voice-level structures, see below. **Omitting the whole `filter` object = bypass** — zero CPU, and the only way to a truly wide-open bright sound (see cutoff cap). |
| `cutoffHz` | `20..12000` | `1000` | base cutoff. **Hard-capped at 12 kHz** (fs/4, the fixed-point stability bound) — a higher value clamps with a warning; if you wanted "no filtering", use `bypass` instead. |
| `resonance` | `0..0.95` | `0` | Q at the cutoff. Capped below self-oscillation (values above clamp with a warning). `0.2–0.4` = character; `0.6+` = squelch. |
| `slope` | `12` \| `24` | `12` | `24` cascades a second identical SVF stage: a steeper, "analogue ladder"-like roll-off (the resonant peak sharpens too — back resonance off a little). Costs one more filter per voice. |
| `drive` | `1..8` | `1` | pre-filter saturation: the oscillator is multiplied by `drive` and soft-clipped (cubic, ±1.0) before the filter — the classic subtractive "overdrive the filter input" grit. `1` is an exact no-op; `2–3` warms, `6+` squares things off. |
| `filter2` | object | off | an **independent second SVF** (own `mode` incl. `notch`, `cutoffHz`, `resonance`) after the first — serial HP+LP with separate corners and Qs is `filter: lowpass` + `filter2: highpass`. By default it shares the first filter's modulation offsets (env, matrix, CC74, key-tracking) so sweeps move both; set `"static": true` for a **fixed corner** that ignores all modulation — a cabinet low-cut that stays put while filter 1 sweeps. Replaces the `slope: 24` cascade when present. |
| `filterRouting` | `serial` \| `parallel` | `serial` | with `filter2`: run the filters in series, or side-by-side (outputs averaged) for dual-peak / phaser-ish shapes. |
| `postDrive` | `1..8` | `1` | **post-filter saturation** — same soft clip as `drive` but *after* the filter(s): distorts the filtered signal at play time. Deliberately **not band-limited** (that is the point); high notes + heavy postDrive can alias, use taste. `1` is an exact no-op. |
| `keyTrackFilter` | `0..1` | `0` | cutoff follows pitch: `1.0` moves the cutoff one octave per octave played (anchored at middle C), keeping brightness constant up the keyboard. `0.5–0.8` is typical for plucked/keyed instruments. |

**`comb`** — a per-voice tunable feedback delay line (`y = x + fb·y[n−d]`):
a pitched resonator with peaks at harmonics of the tuned frequency.
`cutoffHz` is the resonator frequency (every cutoff modulation — envelope,
LFO, CC74, key-tracking — retunes it, clicklessly ramped per sample);
`resonance` is the feedback (`0..0.95`). Noise through a key-tracked comb ≈
plucked strings; sweeping the tuning gives flanger-like metal. Costs 16 KB
of delay memory per voice (statically allocated for all 128 voices).

**`formant`** — three parallel band-passes at vowel formants F1–F3.
`vowel` (`a e i o u`) picks the set, optional `endVowel` is morphed to by
the **filter envelope** level; `cutoffHz` rescales all formants
(`1000` = neutral; lower = bigger/darker voice); `resonance` sets the
formant Q (defaults to `0.85` in this mode). Uses all filter stages, so
`filter2` and `slope` are ignored with a warning.

Modulations sum **in cutoff-index space** (exponential, perceptually even):
base + `filterEnv×depth` + mod-matrix `FilterCutoff` + channel CC74 offset +
key tracking, then clamp to the table.

#### LFOs

```json
"lfos": [ { "shape": "sine", "rateHz": 5.0, "delayMs": 300 },
          { "shape": "sampleHold", "rateHz": 8 } ]
```

Up to **4** per layer (extras dropped with a warning). LFOs do nothing by
themselves — route them in `modMatrix`.

| Field | Type / range | Default | Meaning |
|---|---|---|---|
| `shape` | `sine`, `triangle`, `saw`, `square`, `sampleHold` | `sine` | `saw` ramps `-1→+1`; `sampleHold` picks a new deterministic random value each cycle (per-voice seeded). |
| `rateHz` | `0..40` | `5` | update rate (evaluated per control block, 750 Hz). |
| `delayMs` | ms ≥ 0 | `0` | silent onset — vibrato that fades in after the attack. |
| `retrigger` | bool | `true` | `false` makes the LFO **free-running**: instead of restarting at phase 0 for every note, it starts where an LFO running since the song began would be. A held pad keeps drifting as chords change, rather than resetting its sweep on each one. Deterministic — the phase comes from the song clock, not from which voice the note happened to get. |
| `sync` | `"1/1".."1/64"`, `T` or `.` suffix | off | **tempo-synced** cycle length (`"1/4"` = one beat, `"1/8T"` = eighth triplet, `"1/8."` = dotted eighth); overrides `rateHz` and follows the song's tempo map live (the sequencer feeds tempo to the engine). |

#### Mod matrix

```json
"modMatrix": [
  { "src": "LFO1",     "dst": "Pitch",        "amount": 0.005 },
  { "src": "ModWheel", "dst": "FilterCutoff", "amount": 0.3 }
]
```

Up to **12** routings; sources multiply by `amount` and **sum per
destination** each control block. Unknown src/dst drops the routing with a
warning.

Sources (value range in parentheses):

| `src` | Range | Notes |
|---|---|---|
| `LFO1`..`LFO4` | −1..1 | the layer's LFOs |
| `AmpEnv`, `FilterEnv`, `PitchEnv`, `WtPosEnv` | 0..1 | envelope outputs (reusable as general modulators) |
| `Velocity` | 0..1 | post-`velCurve` gain |
| `KeyTrack` | ≈−1..1 | `(note−60)/64` — pitch position on the keyboard |
| `ModWheel` | 0..1 | channel CC1 |
| `ChannelAftertouch` | 0..1 | channel pressure |
| `PolyAftertouch` | 0..1 | **per-note** pressure (MIDI 0xA0) — parsed, baked, sequenced and routable per voice |

Destinations and what `amount = 1.0` means:

| `dst` | `amount` = 1.0 equals | Typical amounts |
|---|---|---|
| `Pitch` | ±1 octave (1200 cents) | vibrato `0.003–0.008` (≈4–10¢) |
| `FilterCutoff` | full cutoff range (256 steps) | wah/wobble `0.1–0.5` |
| `FilterRes` | full resonance range | `0.1–0.3` |
| `Amplitude` | gain multiplier `1 + value`, clamped 0..1 | tremolo `0.1–0.5` (use negative-capable sources) |
| `WtPosition` | full frame range | morph wobble `0.1–0.4` |
| `Pan` | full right | auto-pan `0.3–1.0` with an LFO |
| `FilterDrive` | +7× (the whole 1..8 drive range) | `Velocity → FilterDrive 0.3–0.6`: palm-mute vs open-chord grit at the filter input |
| `PostDrive` | +7× on the post-filter stage | the "power amp": `Velocity → PostDrive` makes hard picking crunch *after* the cabinet filter |
| `RingAmount` | full ring depth, or the whole 12-radian FM index in `"mode": "fm"` | the moving FM index: `FilterEnv → RingAmount` is the Rhodes bark, `Velocity → RingAmount` makes harder keys brighter |

`amount` is clamped to ±8.

---

## 4. Wavetable generators

```json
"wavetable": {
  "generator": "additiveHarmonicMorph",
  "frameCount": 8,
  "params": { ... }
}
```

* `frameCount` — `1..32` morph frames. Frame 0 uses each generator's
  *start* parameters, the last frame its *end* parameters, interpolated
  linearly between. `wtPosEnv` / `WtPosition` modulation moves through them
  at play time; `frameCount: 1` is a static wave.
* Tables are **generated at load, cached and deduped** by recipe — two layers
  (or two patches) with identical `generator+frameCount+params` share one
  table in memory.
* **Every generator is band-limited per mip level.** The nonlinear ones
  (custom, pulse, waveshaper, chebyshev, hardsync, fm2op) are analyzed by
  DFT and re-synthesized under each mip's harmonic cap, so no recipe can
  alias, whatever you type or draw. (additive, drawbar and harmonics specify
  their spectra directly — the exception that proves the rule: `postDrive`,
  which distorts *after* synthesis at play time, is the one deliberately
  non-band-limited stage.)

### 4.0 `custom`

Your own single cycle: draw it in the editors or paste/import raw values.
Resampled to the analysis cycle, DFT'd and band-limited like every nonlinear
generator, so anything you draw is alias-free.

| Param | Range | Default | Meaning |
|---|---|---|---|
| `samples` | array `2..2048`, each `-1..1` | *required* | one cycle of the wave. DC is removed. |
| `endSamples` | array | = `samples` | optional second cycle morphed to across frames. |

### 4.1 `additiveHarmonicMorph`

Harmonic count sweeps from `startHarmonics` to `endHarmonics` across frames.

| Param | Range | Default | Meaning |
|---|---|---|---|
| `startHarmonics`, `endHarmonics` | `1..366` | `1`, `8` | partial count at first/last frame (values above each mip's safe cap are limited automatically). |
| `falloff` | `saw`, `square`, `triangle`, `flat` | `saw` | per-harmonic amplitude law: `saw` = 1/h (full sawtooth series); `square` = 1/h odd only; `triangle` = 1/h² odd, alternating sign (very mellow); `flat` = all equal (organ/buzzy). |

`{start: 1, end: 1, falloff: "flat"}` is the canonical pure sine.

> `falloff` shapes the *relative* partial levels, so it does nothing until
> there are partials to shape: with `frameCount: 1` the single frame sits at
> `startHarmonics`, and `startHarmonics: 1` is a sine whatever the falloff.
> The editors default new layers to 16 harmonics and warn when the effective
> count is 1.

### 4.2 `pulse`

Rectangular wave whose **duty cycle** morphs across frames — PWM in a table.

| Param | Range | Default | Meaning |
|---|---|---|---|
| `startDuty`, `endDuty` | `0.02..0.98` | `0.5`, `0.15` | high-portion fraction at first/last frame. `0.5` = square (odd harmonics only); narrow duties add even harmonics and get thinner/nasal. |

Sweep it with `wtPosEnv` (e.g. attack 900 ms, depth 1) for the classic
slow-PWM synth lead.

### 4.3 `drawbar`

Organ drawbars: nine harmonic levels specified directly.

| Param | Range | Default | Meaning |
|---|---|---|---|
| `drawbars` | array of 9, each `0..8` | `[8,0,6,0,0,4,0,0,0]` | levels for harmonics **1, 2, 3, 4, 5, 6, 8, 10, 12** (a compact take on the 16′…1′ footages). Loudness follows the organ's law ≈ `(value/8)²`. |
| `endDrawbars` | array of 9 | = `drawbars` | optional second registration morphed to across frames. |

### 4.4 `waveshaper`

An input wave (sine, triangle or saw) driven through a nonlinear shape;
**drive** morphs across frames. More drive = more harmonics —
distortion-style timbre growth. The editors draw the transfer curve
(`in → out` inset) next to the waveform.

| Param | Range | Default | Meaning |
|---|---|---|---|
| `shape` | `soft`, `hard`, `fold`, `crush`, `stairs` | `soft` | `soft` = smooth saturation (`1.5x−0.5x³`, tube-ish); `hard` = clipping (buzzier, strong high partials); `fold` = wavefolding (reflects instead of clipping — West-coast metallic sweeps). |
| `input` | `sine`, `triangle`, `saw` | `sine` | the wave driven into the shaper. Non-sine inputs start with harmonics of their own, so the shaper thickens an already-rich source (band-limited by the resynthesis as always). |
| `startDrive`, `endDrive` | `0.25..64` | `1`, `8` | input gain into the shaper at first/last frame. Drive 1 ≈ nearly clean; 4–10 = strongly colored; higher = extreme (fold gets gnarly fast). |
| `bias` | `-1..1` | `0` | DC offset added *before* the shape: shifts the operating point into the curve's asymmetric region, creating **even** harmonics (the resulting DC is removed). `0.2–0.5` = tube-style asymmetry. |
| `levels` | `2..32` | `6` | `crush` only: output amplitude steps per side after a hard clip — bit-crusher buzz (fewer levels = harsher). |
| `steps` | `2..64` | `16` | `stairs` only: the input phase is sample-held into this many steps per cycle — a "low sample rate" digital read of the wave. |

The smooth shapes are symmetric, so with `bias: 0` they generate odd
harmonics only; bias (or a non-sine input) is how you get the evens.

### 4.5 `chebyshev`

Waveshaping synthesis in its textbook form: a **cosine driven through a sum
of Chebyshev polynomials**. `T_n(cos θ) = cos nθ`, so at `drive: 1` weight
*n* is exactly harmonic *n*; below full drive the polynomials only partially
develop and the upper partials fade **nonlinearly** — sweeping the drive is
the classic waveshaping "index" gesture (brightness that blooms instead of
mixing linearly).

| Param | Range | Default | Meaning |
|---|---|---|---|
| `weights` | array of 8, each `-1..1` | `[1,0,0,0,0,0,0,0]` | weights on `T1..T8` (harmonics 1–8 at full drive). Negative weights flip a partial's phase — audible once the drive drops below 1 and the polynomials interact. All-zero falls back to `T1`. |
| `endWeights` | array of 8 | = `weights` | optional second recipe morphed to across frames. |
| `startDrive`, `endDrive` | `0..1` | `1`, `= startDrive` | input amplitude into the polynomial at first/last frame. Put the sweep in `wtPosEnv` (depth 1) and the timbre *develops* over the note. |

The editors show the transfer curve `Σ wₙ·Tₙ(x)` as an **editable plot**:
nine points sit at the Chebyshev nodes — drag them and the weights are
recovered exactly through the discrete Chebyshev transform (the readout
under the plot shows the resulting `T1..T8`). Hand-editing the numbers in
JSON stays equivalent.

### 4.6 `harmonics`

The general additive recipe (the drawbar is a 9-slot preset of this):
amplitudes for harmonics **1..64**, specified directly — the editors draw it
as a draggable histogram.

| Param | Range | Default | Meaning |
|---|---|---|---|
| `amps` | array up to 64, each `0..1` | `[1]` | amplitude per harmonic (missing entries = 0; all-zero falls back to a sine). Values above each mip's cap are dropped by the band-limiter as usual. |
| `endAmps` | array up to 64 | = `amps` | optional second spectrum morphed to across frames. |

### 4.7 `hardsync`

Hard oscillator sync: a slave wave running at `ratio` × the note's pitch,
phase-reset every master period. The reset edge is the scream; sweeping the
ratio (`wtPosEnv`) is *the* sync lead gesture. Fractional ratios are fine —
the cycle stays periodic at the master period by construction, and the DFT
resynthesis keeps every mip band-limited.

| Param | Range | Default | Meaning |
|---|---|---|---|
| `startRatio`, `endRatio` | `1..16` | `1`, `3` | slave/master frequency ratio at first/last frame. `1` = the plain slave wave; `1.5–4` = classic rip; higher = glassy. |
| `slave` | `saw`, `pulse` | `saw` | the wave being reset. |

### 4.8 `fm2op`

Two-operator FM: carrier at the note's pitch, modulator at an integer
`ratio`; modulation **index** morphs across frames. Bells, e-pianos, clangs.

| Param | Range | Default | Meaning |
|---|---|---|---|
| `ratio` | integer `1..16` | `2` | modulator frequency as a multiple of the carrier (integer keeps the cycle periodic in a single table). Low ratios = harmonic/reedy; high ratios (7, 11, 14) = bell-like, clangorous. |
| `startIndex`, `endIndex` | `0..12` | `0.5`, `4` | modulation index (radians) at first/last frame. 0 = pure sine; 1–3 = musical; 6+ = harsh. The classic FM "bright attack decaying to pure tone" is `startIndex` high, `endIndex` low, plus a decaying `wtPosEnv`… inverted: put the *bright* value where the envelope starts. |

> Note the direction: `wtPosEnv` at level 0 sits on frame 0 (the `start*`
> values) and moves toward the last frame (`end*`) as its level rises.

---

## 5. Song FX config file

```json
{
  "version": 1,
  "reverb": { "roomSize": 0.8, "damping": 0.4, "width": 1.0, "wetGain": 0.3 },
  "chorus": { "rateHz": 0.8, "depthMs": 3.5, "wetGain": 0.2 },
  "delay":  { "timeMs": 320, "feedback": 0.35, "wetGain": 0.0, "pingPong": false },
  "sidechain": { "source": 10, "note": 36, "targets": [2, 3],
                 "depth": 0.3, "attackMs": 5, "holdMs": 20, "releaseMs": 180 },
  "tuning": { "cents": [0,0,0,0,0,0,0,0,0,0,0,0] }
}
```

Loaded alongside a song (`--fx` on the CLIs, `loadSongConfig` in the API/tool)
— a cathedral track and a dry funk track want different rooms. Channels only
control their **send amounts** (CC91 reverb / CC93 chorus); these are the
shared effects' characters. Both sections optional; defaults shown above.

| Field | Range | Meaning |
|---|---|---|
| `reverb.roomSize` | 0..1 | tail length (feedback 0.7 + 0.28×size — 1.0 is *long*) |
| `reverb.damping` | 0..1 | high-frequency absorption in the tail |
| `reverb.width` | 0..1 | stereo decorrelation of the return (0 = mono) |
| `reverb.wetGain` | 0..2 | return level into the mix (0 disables) |
| `reverb.preDelayMs` | 0..200 | silence before the tail starts, so a transient is heard before its room. **0 skips the stage entirely.** |
| `reverb.highPassHz` | 0..1000 | highpass on the *send*, not the return: keeps kick and sub out of the tail, which is how a bass-heavy mix stays clean. **0 = off.** |
| `chorus.rateHz` | 0..10 | modulation speed |
| `chorus.depthMs` | 0..20 | delay swing around the 15 ms base |
| `chorus.wetGain` | 0..2 | return level (chorus is wet-only on a send) |
| `delay.timeMs` | 1..640 | echo time (send via **CC94** per channel; wet 0 = effect off, lines stay cold) |
| `delay.feedback` | 0..0.9 | repeat level, with gentle in-loop damping so echoes darken |
| `delay.wetGain` | 0..2 | return level of the delay send |
| `delay.pingPong` | bool | feedback crosses channels — echoes bounce L↔R |
| `delay.sync` | `"1/8"`, `"1/8."`, `"1/16T"`, … | echo time as a **beat division instead of milliseconds**, following the song tempo live (including mid-song changes). Dotted (`.`) and triplet (`T`) suffixes as for LFO sync. Wins over `timeMs` when present; a dotted eighth at 174 BPM is 259 ms. |
| `sidechain` | object | **ducking**: a note on `source` (channel `1..16`, optionally only `note`) pulls down every channel in `targets` by `depth` (0..1), over `attackMs`, held `holdMs`, recovering across `releaseMs`. Triggered by the note event, not by a level detector, so it is exact and reproducible — and a source note that maps to no sound still triggers it, which is how you duck from a silent marker track. Only **dry channel lanes** duck: the reverb and delay returns keep ringing, which is usually what you want. `depth: 0` = off. |
| `channelEq.<ch>` | object per MIDI channel `"1".."16"` | per-channel 3-band EQ (mixer strip): `lowDb` (shelf @120 Hz), `midDb` + `midHz` (bell, `100..8000`), `highDb` (shelf @6 kHz), each `−24..24`. Flat = exact bypass at zero cost. Baked into the `.mstk` with the rest of the FX. |
| `tuning.cents` | 12 × −200..200 | **microtuning**: per-pitch-class offsets in cents (C, C♯, … B), applied to every voice. Werckmeister, just intonation on a drone root, quarter-tone accidentals — all baked into the `.mstk` with the rest of the FX. |

### 5.1 Mastering — `master` (in fx.json) or a standalone `master.json`

```json
{
  "version": 1,
  "master": {
    "enabled": true,
    "eq": {
      "low":  { "hz": 120,  "db": 0 },
      "mid":  { "hz": 1000, "db": 0, "q": 1 },
      "high": { "hz": 8000, "db": 0 }
    },
    "comp":    { "thresholdDb": -12, "ratio": 2, "attackMs": 10, "releaseMs": 150,
                 "makeupDb": 0, "kneeDb": 6 },
    "limiter": { "ceilingDb": -1, "lookaheadBlocks": 2, "releaseMs": 80 },
    "width": 1.0,
    "gainDb": 0
  }
}
```

The mastering chain runs **inside the engine**, after master volume/ducking
and before the soft clip + dither: `EQ → compressor → width → gain →
lookahead limiter`. It is deterministic fixed point (log2-domain dynamics
from the same baked tables as everything else), **bypassed by default**, and
baked into `.mstk` (`MSTR` section) so the game plays the mastered result
bit-identically. `load_master_config` accepts the wrapped document above or
the bare `master` object; `--master m.json` on the CLIs, `masterParam`/
`loadMaster` in the worklet, `msynth_master_param`/`msynth_load_master_json`
in the C API. Defaults shown above; every field optional.

| Field | Range | Meaning |
|---|---|---|
| `enabled` | bool / 0..1 | off ⇒ the output path is byte-identical to an unmastered engine |
| `eq.low.hz`, `eq.low.db` | 20..20000, −24..24 | low shelf (√2 damping); `x + (lin(dB)−1)·LP(x)`; DC gain is exactly `dB` |
| `eq.mid.hz`, `eq.mid.db`, `eq.mid.q` | 20..20000, −24..24, 0.5..10 | bell (`k = 1/Q`); neutral at DC and Nyquist |
| `eq.high.hz`, `eq.high.db` | 20..20000, −24..24 | high shelf; `x + (lin(dB)−1)·HP(x)` |
| `comp.thresholdDb` | −60..0 | dBFS, block-rate linked peak detector |
| `comp.ratio` | 1..20 | 1 = no compression |
| `comp.attackMs`, `comp.releaseMs` | 0.1..200, 5..2000 | one-pole time constants in the log2 domain (≈63 % per τ) |
| `comp.makeupDb` | 0..24 | post-compression gain |
| `comp.kneeDb` | 0..24 | soft-knee width around the threshold (0 = hard) |
| `limiter.ceilingDb` | −12..0 | the output **never** exceeds this (rounding aside): the limiter looks `lookaheadBlocks` blocks ahead and lands its gain ramp before the peak is emitted |
| `limiter.lookaheadBlocks` | 1..8 | control blocks (64 samples) of lookahead = added latency |
| `limiter.releaseMs` | 1..1000 | gain recovery time constant |
| `satDrive` | 1..8 | bus saturation between compressor and limiter (cubic soft clip); 1 = exact no-op, 1.5–2.5 = glue/loudness before the ceiling |
| `width` | 0..2 | 0 = mono, 1 = unchanged (bit-exact), 2 = side doubled |
| `gainDb` | −24..24 | output gain; 0 is bit-exact identity |

Filter corners above 12 kHz are pinned to the engine's cutoff table cap
(the SVF's `fs/4` limit); the high shelf still works as a shelf from there.
Meters (peak, RMS mean-square, compressor GR, limiter gain, clip count) are
read through `takeMetersJson` / `msynth_take_master_meters` and accumulate on
both paths, bypass included.

---

## 6. Recipe cookbook

**Liquid drum & bass, start to finish.** `content/patches/dnb_bank.json` +
`content/drums/dnb_kit.json` + `content/music/dreamy_dnb.msproj` are a worked
example of everything below, with no samples anywhere:

* *Reese* — 5 detuned saws with `phaseSpread: 1` (beats from the first
  sample) into a lowpass plus a `notch` `filter2`, walked by a bar-synced
  `retrigger: false` LFO so a held note keeps moving.
* *Sub* — one sine, no unison, no detune, lowpassed at 220 Hz. Nothing that
  could smear the low end.
* *Liquid pad* — slow `sCurve` swell, free-running LFOs on cutoff and pan, a
  breath of `"noiseColor": "pink"` for air, most of it on the reverb send.
* *Rhodes* — `"ring": {"mode": "fm"}` with `FilterEnv → RingAmount`: the
  index falls as the note decays, so it barks then mellows.
* *Break kit* — snare is a tuned body plus pink noise, cymbals are the
  `metal` oscillator, the clap is three `startDelayMs` bursts, and ghost
  notes come from velocity rather than from separate sounds.
* *The mix* — `delay.sync: "1/8."`, `reverb.preDelayMs: 28` with
  `highPassHz: 320` so the sub stays out of the tail, and a `sidechain` on
  the kick ducking pad, pluck and atmosphere.


Field-tested starting points (all live in `content/`):

* **Pluck/piano family**: additive morph bright→dark (`start 26, end 5`),
  amp `sustain 0`, long `exp` decay, `wtPosEnv` fast decay (brightness dies
  first), `keyTrackFilter 0.7`.
* **Bowed/blown sustain**: additive morph dark→bright, slow `sCurve` attack,
  `wtPosEnv` slow attack, delayed sine-LFO→Pitch `0.004–0.006`.
* **Ensemble width**: 2–3 layers, same recipe, `detuneCents` −8/+8, slight
  `pan` splits, staggered attacks.
* **PWM lead**: `pulse 0.5→0.18`, `wtPosEnv` attack ≈900 ms depth 1.
* **E-piano**: `fm2op ratio 14` for the tine layer (index 1.6→0.15,
  `wtPosEnv` decay ≈400 ms) over a `ratio 1` body layer; `velCurve: "soft"`.
* **Kick**: layer 1 `pitchNote ≈29`, 2 harmonics, `pitchEnv depthSemis 24
  decay 45`, amp decay 180 `exp`; layer 2 noise lowpassed ≈3.5 kHz, 15 ms.
* **Hi-hats**: noise → highpass 7–8 kHz; closed decay ≈40 ms, open ≈380 ms,
  both `oneShot` + same `exclusiveGroup`.
* **Snare**: tone layer (`pitchNote ≈55`, small downward pitchEnv) + noise
  layer highpassed ≈1.6 kHz, decay 170 ms.
* **Hand clap**: 3–4 short noise bursts (bandpass ≈1.1 kHz, res 0.3, decay
  ≈25 ms) at `startDelayMs 0/11/23`, plus a longer fourth burst (decay
  ≈180 ms) at ≈35 ms — the flam *is* the clap; no LFO chopping needed. Works
  for cuica scrapes, stacked bell strikes and flammed toms the same way.
* **Sync lead**: `hardsync startRatio 1 → endRatio 3.5`, `wtPosEnv`
  decay ≈350 ms depth 1 (the rip happens on the sweep back down), 24 dB
  lowpass ≈4 kHz with `drive 2`, a little `detuneCents` on a second layer.
* **Waveshaping bloom**: `chebyshev` with a few mid weights (drag the curve
  into an S with ripples), `startDrive 0.2 → endDrive 1`, `wtPosEnv` slow
  attack — partials *develop* instead of crossfading; classic CCRMA pad.
* **Chip/bit lead**: `waveshaper shape "crush" levels 4, input "saw",
  drive 1` — instant console; add `stairs steps 12` on a second layer for
  the aliased-sampler flavor (still alias-free at the output).
* **Bell from partials**: `harmonics` histogram with amps at 1, 2.76→3,
  5.4→5, 8.9→9 (inharmonic-ish picks: 1, 3, 5, 9 with 0.6/0.4/0.25),
  `endAmps` mostly fundamental, `wtPosEnv` decay ≈600 ms, long amp release.
* **Subtractive bass**: classic `saw` (additive falloff saw, 64 harmonics)
  → `filter lowpass 24 dB`, `cutoffHz ≈700`, `resonance 0.25`, `drive 2.5`,
  `filterEnv depth 0.35 decay 120`, `keyTrackFilter 0.6`.
* **Notch sweep**: any bright wave, `filter mode "notch"` with an LFO →
  `FilterCutoff 0.2–0.4` — phaser-like movement without an effect unit.

Authoring workflow: `node tool/serve.js` → the patch editor (`/tool/`) or
the DAW's Synth tab (`/tool/daw.html`, auditioning on the selected track)
edits every field on this page live (only voices on the edited patch
restart), shows the per-generator waveform, transfer curves, draggable
harmonic histogram, layer sum, envelopes and filter response, and exports
the bank/kit files back out.
