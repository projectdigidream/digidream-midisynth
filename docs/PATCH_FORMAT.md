# midisynth JSON formats — full reference

Three JSON file kinds feed the engine, all loaded on the control thread and
converted to fixed point before any audio code sees them:

| File | Loaded by | Purpose |
|---|---|---|
| **Patch bank** (`gm_bank.json`) | `load_patch_bank` | melodic instruments per GM program number + the fallback patch |
| **Drum kit** (`gm_kit.json`) | `load_drum_kit` | one mini-patch per MIDI note on channel 10 |
| **Song FX config** (`content/fx/*.json`) | `load_song_config` | per-song reverb/chorus character |

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

  "ampEnv":    { ... }, "filterEnv": { ... },
  "pitchEnv":  { ... }, "wtPosEnv":  { ... },

  "filter": { "mode": "lowpass", "cutoffHz": 1200, "resonance": 0.3 },
  "keyTrackFilter": 0.5,

  "lfos": [ { "shape": "sine", "rateHz": 5.0, "delayMs": 300 } ],
  "modMatrix": [ { "src": "LFO1", "dst": "Pitch", "amount": 0.02 } ]
}
```

#### Oscillator source

| Field | Type / range | Default | Meaning |
|---|---|---|---|
| `osc` | `"wavetable"` \| `"noise"` | `"wavetable"` | `noise` is a white-noise source (deterministic per-voice xorshift; seed derives from pool slot + note, so output never depends on allocation history). Shape noise with the filter — hi-hats are noise through a highpass, snares tone + highpassed noise. `wavetable` is ignored for noise layers. |
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

#### Envelopes — `ampEnv`, `filterEnv`, `pitchEnv`, `wtPosEnv`

All four share one shape:

```json
{ "attack": 5, "decay": 200, "sustain": 0.7, "release": 300,
  "attackCurve": "linear", "decayCurve": "exp", "releaseCurve": "exp",
  "depth": 0.5 }
```

| Field | Type / range | Default | Meaning |
|---|---|---|---|
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

#### Filter

```json
"filter": { "mode": "lowpass", "cutoffHz": 1200, "resonance": 0.3 },
"keyTrackFilter": 0.5
```

| Field | Type / range | Default | Meaning |
|---|---|---|---|
| `mode` | `"lowpass"` \| `"bandpass"` \| `"highpass"` \| `"bypass"` | `"lowpass"` (when object present) | output tap of the per-voice SVF. **Omitting the whole `filter` object = bypass** — zero CPU, and the only way to a truly wide-open bright sound (see cutoff cap). |
| `cutoffHz` | `20..12000` | `1000` | base cutoff. **Hard-capped at 12 kHz** (fs/4, the fixed-point stability bound) — a higher value clamps with a warning; if you wanted "no filtering", use `bypass` instead. |
| `resonance` | `0..0.95` | `0` | Q at the cutoff. Capped below self-oscillation (values above clamp with a warning). `0.2–0.4` = character; `0.6+` = squelch. |
| `keyTrackFilter` | `0..1` | `0` | cutoff follows pitch: `1.0` moves the cutoff one octave per octave played (anchored at middle C), keeping brightness constant up the keyboard. `0.5–0.8` is typical for plucked/keyed instruments. |

Modulations sum **in cutoff-index space** (exponential, perceptually even):
base + `filterEnv×depth` + mod-matrix `FilterCutoff` + channel CC74 offset +
key tracking, then clamp to the table.

#### LFOs

```json
"lfos": [ { "shape": "sine", "rateHz": 5.0, "delayMs": 300 },
          { "shape": "sampleHold", "rateHz": 8 } ]
```

Up to **2** per layer (extras dropped with a warning). LFOs do nothing by
themselves — route them in `modMatrix`.

| Field | Type / range | Default | Meaning |
|---|---|---|---|
| `shape` | `sine`, `triangle`, `saw`, `square`, `sampleHold` | `sine` | `saw` ramps `-1→+1`; `sampleHold` picks a new deterministic random value each cycle (per-voice seeded). |
| `rateHz` | `0..40` | `5` | update rate (evaluated per control block, 750 Hz). |
| `delayMs` | ms ≥ 0 | `0` | silent onset — vibrato that fades in after the attack. |

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
| `LFO1`, `LFO2` | −1..1 | the layer's LFOs |
| `AmpEnv`, `FilterEnv`, `PitchEnv`, `WtPosEnv` | 0..1 | envelope outputs (reusable as general modulators) |
| `Velocity` | 0..1 | post-`velCurve` gain |
| `KeyTrack` | ≈−1..1 | `(note−60)/64` — pitch position on the keyboard |
| `ModWheel` | 0..1 | channel CC1 |
| `ChannelAftertouch` | 0..1 | channel pressure |

Destinations and what `amount = 1.0` means:

| `dst` | `amount` = 1.0 equals | Typical amounts |
|---|---|---|
| `Pitch` | ±1 octave (1200 cents) | vibrato `0.003–0.008` (≈4–10¢) |
| `FilterCutoff` | full cutoff range (256 steps) | wah/wobble `0.1–0.5` |
| `FilterRes` | full resonance range | `0.1–0.3` |
| `Amplitude` | gain multiplier `1 + value`, clamped 0..1 | tremolo `0.1–0.5` (use negative-capable sources) |
| `WtPosition` | full frame range | morph wobble `0.1–0.4` |
| `Pan` | full right | auto-pan `0.3–1.0` with an LFO |

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
  (pulse, waveshaper, fm2op) are analyzed by DFT and re-synthesized under
  each mip's harmonic cap, so no recipe can alias, whatever you type in.

### 4.1 `additiveHarmonicMorph`

Harmonic count sweeps from `startHarmonics` to `endHarmonics` across frames.

| Param | Range | Default | Meaning |
|---|---|---|---|
| `startHarmonics`, `endHarmonics` | `1..366` | `1`, `8` | partial count at first/last frame (values above each mip's safe cap are limited automatically). |
| `falloff` | `saw`, `square`, `triangle`, `flat` | `saw` | per-harmonic amplitude law: `saw` = 1/h (full sawtooth series); `square` = 1/h odd only; `triangle` = 1/h² odd, alternating sign (very mellow); `flat` = all equal (organ/buzzy). |

`{start: 1, end: 1, falloff: "flat"}` is the canonical pure sine.

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

A sine driven through a nonlinear shape; **drive** morphs across frames.
More drive = more harmonics — distortion-style timbre growth.

| Param | Range | Default | Meaning |
|---|---|---|---|
| `shape` | `soft`, `hard`, `fold` | `soft` | `soft` = smooth saturation (`1.5x−0.5x³`, tube-ish); `hard` = clipping (buzzier, strong high partials); `fold` = wavefolding (reflects instead of clipping — West-coast metallic sweeps). |
| `startDrive`, `endDrive` | `0.25..64` | `1`, `8` | input gain into the shaper at first/last frame. Drive 1 ≈ nearly clean; 4–10 = strongly colored; higher = extreme (fold gets gnarly fast). |

All three shapes are symmetric, so they generate odd harmonics only.

### 4.5 `fm2op`

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
  "chorus": { "rateHz": 0.8, "depthMs": 3.5, "wetGain": 0.2 }
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
| `chorus.rateHz` | 0..10 | modulation speed |
| `chorus.depthMs` | 0..20 | delay swing around the 15 ms base |
| `chorus.wetGain` | 0..2 | return level (chorus is wet-only on a send) |

---

## 6. Recipe cookbook

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

Authoring workflow: `node tool/serve.js` → the visual editor edits every
field on this page live (only voices on the edited patch restart), shows the
per-generator waveform, layer sum, envelopes and filter response, and
exports the bank/kit files back out.
