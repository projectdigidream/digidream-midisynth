#!/usr/bin/env bash
#
# Builds the synth to a single-file ES6 WASM module importable from an
# AudioWorklet (tool/synth-worklet.js imports it). Mirrors the native build's
# determinism flags; the fixed-point core has no float path, but if one ever
# leaks in it must not silently pick up per-target FMA/fast-math.
#
# -sSINGLE_FILE: the worklet scope has no fetch(); the wasm rides inside the JS.
# -sENVIRONMENT=shell: neutral glue -- no window/document/importScripts probes.
# (TextDecoder is assumed present -- true of current worklet scopes; the tool
# falls back to main-thread rendering if the worklet import fails.)
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EMXX="$ROOT/../emsdk/upstream/emscripten/em++"
if [[ ! -x "$EMXX" ]]; then
  EMXX=$(command -v em++ || true)
fi
if [[ -z "${EMXX:-}" ]]; then
  echo "em++ not found (looked in repo emsdk and PATH)" >&2
  exit 1
fi

OUT="$ROOT/tool/dist"
mkdir -p "$OUT"

"$EMXX" -std=c++17 -O3 \
  -Wall -Wextra -Werror \
  -fno-fast-math -ffp-contract=off -fno-strict-aliasing \
  -fno-exceptions -fno-rtti -DEMSCRIPTEN_HAS_UNBOUND_TYPE_NAMES=0 \
  -I"$ROOT/src" -I"$ROOT/../engine/core/include" \
  "$ROOT/src/synth/tables.cpp" \
  "$ROOT/src/synth/wavetables.cpp" \
  "$ROOT/src/synth/wavetable_gen_additive.cpp" \
  "$ROOT/src/synth/wavetable_gen_extra.cpp" \
  "$ROOT/src/synth/wavetable_cache.cpp" \
  "$ROOT/src/synth/json_min.cpp" \
  "$ROOT/src/synth/patch_loader.cpp" \
  "$ROOT/src/synth/song_config.cpp" \
  "$ROOT/src/synth/voice.cpp" \
  "$ROOT/src/synth/voice_pool.cpp" \
  "$ROOT/src/synth/synth_engine.cpp" \
  "$ROOT/src/synth/effects/reverb.cpp" \
  "$ROOT/src/synth/master_chain.cpp" \
  "$ROOT/src/midi/smf_parser.cpp" \
  "$ROOT/src/midi/smf_writer.cpp" \
  "$ROOT/src/track/track.cpp" \
  "$ROOT/src/midi/tempo_map.cpp" \
  "$ROOT/src/sequencer/song.cpp" \
  "$ROOT/src/sequencer/sequencer.cpp" \
  "$ROOT/src/sequencer/music_director.cpp" \
  "$ROOT/src/bindings/embind_api.cpp" \
  -lembind \
  -sMODULARIZE=1 \
  -sEXPORT_ES6=1 \
  -sEXPORT_NAME=createMidiSynth \
  -sSINGLE_FILE=1 \
  -sENVIRONMENT=shell \
  -sALLOW_MEMORY_GROWTH=0 \
  -sINITIAL_MEMORY=134217728 \
  -sMALLOC=emmalloc \
  -sSTACK_SIZE=1048576 \
  -sASSERTIONS=0 \
  -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAP16,HEAPF32 \
  -sEXPORTED_FUNCTIONS=_malloc,_free \
  -o "$OUT/midisynth.mjs"

ls -la "$OUT"
