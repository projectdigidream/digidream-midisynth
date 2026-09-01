#!/usr/bin/env bash
#
# Full local gate: build native, run the unit suite, build WASM (if emsdk is
# available), run goldens including the native-vs-wasm hash comparison.
# Any divergence or test failure fails the script (plan §14).
#
set -euo pipefail
cd "$(dirname "$0")/.."

cmake -S . -B build -G Ninja > /dev/null
ninja -C build

./build/midisynth_tests

if [[ -x ../emsdk/upstream/emscripten/em++ ]] || command -v em++ > /dev/null; then
  ./scripts/build_wasm.sh > /dev/null
else
  echo "emsdk not found; skipping WASM build (goldens will skip the cross check)"
fi

./scripts/run_goldens.sh
echo "CI gate: all green"
