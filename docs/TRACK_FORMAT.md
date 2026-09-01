# .mstk — the baked track format

A **track** is midisynth's self-contained music deliverable: MIDI + patches +
drum kit + effects config compiled into one binary file — the music analogue
of the engine's `mapc`-baked maps. The game ships tracks, not loose JSON and
SMF files, and everything that can fail at load already failed at bake.

```sh
# bake
build/midisynth_bake song.mid -o song.mstk \
    --bank content/patches/gm_bank.json --drums content/drums/gm_kit.json \
    [--fx content/fx/cathedral.json] [--name "boss_theme"] \
    [--loop-start-tick T --loop-end-tick T] [--verify-seconds 20]

# verify a track's embedded determinism seal (re-renders and compares)
build/midisynth_bake --check song.mstk

# every consumer accepts .mstk wherever it accepts .mid
build/midisynth_render song.mstk out.wav --hash
build/midisynth_play   song.mstk
msynth_load_track(ctx, bytes, len, report, cap)          # C API
synth.loadTrackFromBuffer()                              # WASM
```

The authoring tool bakes too: load a `.mid`, tweak patches, press
**Bake .mstk** — the download embeds your current bank/kit edits and the FX
slider state. The tool also *plays* `.mstk` files directly.

## What baking does

1. **Parses and validates everything up front.** SMF salvage notes, patch
   clamps, unknown fields — every warning the runtime would ever produce is
   surfaced at bake time. Hard errors abort the bake; a track that exists is
   a track that loads.
2. **Pre-parses the song.** The merged, time-ordered event list with
   *baked absolute sample positions*, the tempo map with cumulative anchors,
   and the loop points are stored in binary. Loading a track does **no SMF
   parsing and no tick→sample math** — the sequencer adopts the data as-is.
3. **Tree-shakes the content.** Only the programs the song actually *plays
   notes with* (plus the fallback patch) and only the drum notes it actually
   hits survive into the file. A full 37-patch bank shrinks to exactly the
   song's instruments. Programs that resolve to the fallback and drum notes
   with no mapping (which will be **silent**) are called out as warnings.
4. **Resolves the FX config** to fixed-point parameters.
5. **Seals it.** The payload's sha256 goes into the header; a flipped bit
   anywhere makes the loader reject the file (baked artifacts are
   trusted-or-rejected — salvage is for source SMFs, not compiled ones).
   With `--verify-seconds N`, the baker additionally renders N seconds
   through the real engine and embeds the PCM sha256: `--check` re-renders
   on any machine and proves the track still produces bit-identical audio.

Real numbers: the 16.8-minute, 31-track Swan Lake SMF (195 KB) bakes to a
1.25 MB track — 56,636 pre-parsed events, 16 instruments and 2 drum notes
tree-shaken out of the full bank, with a 20-second render seal.

## Loop overrides

`--loop-start-tick/--loop-end-tick` bake explicit loop points into the file
(the plan's highest-priority loop source), overriding whatever the SMF's
markers/CC111 declared — so a game-ready looping track needs no runtime
configuration at all.

## Cutting tracks into gameplay pieces

`cut_track` (and the **track cutter** web tool at `/tool/cutter.html`) slices
a baked track into smaller self-contained tracks:

* **State chase at the cut point**: program, CCs, pitch bend, RPN bend
  range/tuning, time signature and tempo in effect at the cut start are
  re-emitted at tick 0 — a slice from the middle of a song opens sounding
  exactly as the parent did there (the §10 seek chase, applied at bake time).
* **Note hygiene**: note-ons from before the cut aren't carried in and their
  stray note-offs are dropped (they'd clip re-struck notes); notes still
  sounding at the cut end get synthetic note-offs there.
* **Loops whole by default** — a slice is a loopable gameplay piece as-is.
* **Content is inherited verbatim** from the parent (no re-shaking):
  every slice of one parent shares the same `content_hash`, so a game can
  hold one loaded bank/kit and **crossfade freely between sibling slices**
  (`msynth_crossfade_track` warns — doesn't fail — on hash mismatch).
* The tempo map is re-anchored through the same canonical conversion, so
  slice event timing is exact, not resampled.

The cutter tool: load a `.mstk`, drag a bar-snapped region on the timeline
(note-density strip + bar grid + playhead), **▶ Loop region** to hear how it
loops, **⇄ Crossfade into region** to hear the transition at a chosen fade
length, then add slices to a list and export each as its own `.mstk`.

## Binary layout (version 1)

All integers little-endian, fixed width. No alignment padding.

```
header:
  u32  magic      "MSTK" (0x4B54534D LE)
  u16  version    readers reject newer versions
  u16  flags      0
  char sha256_hex[64]   integrity hash of the payload
  u32  section_count
  { u32 tag; u32 offset; u32 size; }[section_count]   offsets into payload
payload: concatenated section bodies
```

| Tag | Content |
|---|---|
| `SONG` | `u32 division`, `u32 end_tick`, `u64 end_sample`, `u8 loop_flags`, `u32 loop_start_tick`, `u32 loop_end_tick`, tempo segments (`u32 start_tick, u64 start_sample, u32 us_per_quarter, u64 samples_per_tick_fp`), events (`u64 sample, u32 tick, u8 status, u8 d1, u8 d2, u16 track, u32 seq`) |
| `BANK` | length-prefixed UTF-8: tree-shaken patch-bank JSON |
| `KIT ` | length-prefixed UTF-8: tree-shaken drum-kit JSON |
| `FX  ` | 7 × `i32` Q16.16: reverb room/damping/width/wet, chorus rate/depth/wet |
| `META` | name (length-prefixed), `u32 duration_ms`, `u8 programs_used[16]` bitmap, `u8 drum_notes_used[16]` bitmap, `u8 has_render_hash`, `u8 render_hash[32]`, `u32 render_seconds`, `u8 content_hash[32]` (sha256 of bank+kit JSON — equal hashes ⇒ crossfade-compatible) |

Two consistency properties are enforced at load:

* The tempo map is **rebuilt from its raw (tick, tempo) pairs through the
  engine's one canonical conversion and compared against the baked anchors**
  — a mismatch means the file is corrupt or was baked by a build whose
  tempo math diverged, and the load is rejected. This extends the golden-
  test discipline to every shipped track.
* The content sections still pass through the normal JSON loaders at load,
  so schema evolution is handled by the loaders' own versioning/clamping —
  the binary format never needs to track the patch schema.

Why the content stays JSON inside a binary container: the patch schema is
the part of the system that evolves (new generators, new fields), and the
loaders already own validation and migration for it. Baking to resolved
binary structs would create a second serialization of `LayerDef` to keep in
lockstep forever. The song data, by contrast, is stable and hot — that's
what gets the binary treatment.
