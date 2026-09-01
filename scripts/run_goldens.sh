#!/usr/bin/env bash
#
# Golden-file determinism harness (plan §14).
#
#   scripts/run_goldens.sh              -- render all references, compare hashes
#   scripts/run_goldens.sh --regen      -- re-render, write new hashes + review WAVs
#
# Any hash divergence fails (exit 1). Regeneration writes the WAVs to
# test/golden/review/ so the author can A/B listen before committing the new
# hashes (the process in §14: regeneration never skips the listen).
#
set -euo pipefail
cd "$(dirname "$0")/.."

RENDER=${RENDER:-build/midisynth_render}
GOLDEN=test/golden
HASHFILE=$GOLDEN/goldens.txt
REGEN=0
[[ "${1:-}" == "--regen" ]] && REGEN=1

if [[ ! -x "$RENDER" ]]; then
  echo "error: $RENDER not built" >&2
  exit 2
fi

python3 tools/make_test_midi.py > /dev/null

# name : midi file : extra renderer args
CASES=(
  "melody:melody.mid:"
  "melody_dither:melody.mid:--dither"
  "drums:drums.mid:"
  "tempos:tempos.mid:--seconds 8"
  "tempos_loop:tempos.mid:--loop --seconds 12"
  "poly:poly.mid:"
  "swan_lake_30s:swan_lake.mid:--seconds 30"
)

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
NEWHASHES=$TMP/goldens.txt
FAIL=0

mkdir -p "$GOLDEN/review"

for case in "${CASES[@]}"; do
  IFS=: read -r name midi args <<< "$case"
  if [[ ! -f "$GOLDEN/$midi" ]]; then
    echo "skip $name ($midi missing)"
    continue
  fi
  wav="$TMP/$name.wav"
  # shellcheck disable=SC2086
  hash=$("$RENDER" "$GOLDEN/$midi" "$wav" \
      --bank content/patches/gm_bank.json --drums content/drums/gm_kit.json \
      --hash $args 2>/dev/null | awk '/^sha256/ {print $2}')
  echo "$name $hash" >> "$NEWHASHES"
  if [[ $REGEN == 1 ]]; then
    cp "$wav" "$GOLDEN/review/$name.wav"
    echo "regen $name $hash"
  else
    expected=$(awk -v n="$name" '$1==n {print $2}' "$HASHFILE" 2>/dev/null || true)
    if [[ -z "$expected" ]]; then
      echo "MISSING golden for $name (got $hash) -- run --regen" >&2
      FAIL=1
    elif [[ "$expected" != "$hash" ]]; then
      echo "DIVERGED $name: expected $expected got $hash" >&2
      FAIL=1
    else
      echo "ok $name"
    fi
  fi
done

if [[ $REGEN == 1 ]]; then
  cp "$NEWHASHES" "$HASHFILE"
  echo "new hashes written to $HASHFILE; review WAVs in $GOLDEN/review/"
  echo "LISTEN to the review WAVs before committing (plan §14)."
fi

# Baked-track leg: a track baked from melody's sources must render the very
# same PCM as the direct render (tree-shaken content, pre-parsed song).
BAKE=${BAKE:-build/midisynth_bake}
if [[ -x "$BAKE" ]]; then
  "$BAKE" "$GOLDEN/melody.mid" -o "$TMP/melody.mstk"       --bank content/patches/gm_bank.json --drums content/drums/gm_kit.json > /dev/null 2>&1
  track_hash=$("$RENDER" "$TMP/melody.mstk" "$TMP/track.wav" --hash 2>/dev/null |
      awk '/^sha256/ {print $2}')
  expected=$(awk '$1=="melody" {print $2}' "$HASHFILE" 2>/dev/null || true)
  if [[ -n "$expected" && "$track_hash" != "$expected" ]]; then
    echo "DIVERGED baked-track melody: expected $expected got $track_hash" >&2
    FAIL=1
  else
    echo "ok baked track == source render"
  fi
fi

# Cross-platform leg: the WASM build must hash identically to native
# (plan §14 -- any divergence fails the build). Skipped when the module or
# node is absent.
if [[ -f tool/dist/midisynth.mjs ]] && command -v node > /dev/null; then
  native=$("$RENDER" "$GOLDEN/melody.mid" "$TMP/xplat.wav" \
      --bank content/patches/gm_bank.json --drums content/drums/gm_kit.json \
      --hash --seconds 10 2>/dev/null | awk '/^sha256/ {print $2}')
  wasm=$(node test/determinism_wasm.mjs "$GOLDEN/melody.mid" 10 | awk '/^sha256/ {print $2}')
  if [[ "$native" != "$wasm" ]]; then
    echo "DIVERGED native-vs-wasm: $native vs $wasm" >&2
    FAIL=1
  else
    echo "ok native == wasm ($native)"
  fi
else
  echo "skip native-vs-wasm (build tool/dist/midisynth.mjs first)"
fi
exit $FAIL
