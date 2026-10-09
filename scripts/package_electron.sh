#!/usr/bin/env bash
#
# Build the DigidreamSynth desktop app (Electron).
#
#   ./scripts/package_electron.sh               installers for this platform
#   ./scripts/package_electron.sh --dir         unpacked app only (fast)
#   ./scripts/package_electron.sh --stage-only  just refresh electron/payload/
#   ./scripts/package_electron.sh --linux       cross-build Linux targets
#
# The desktop app is the web bundle plus a shell: this stages the exact same
# static payload scripts/package_web.sh produces, so browser and desktop run
# byte-identical tool code.
#
set -euo pipefail
cd "$(dirname "$0")/.."

MODE=${1:-dist}

if [[ ! -f tool/dist/midisynth.mjs ]]; then
  echo "tool/dist/midisynth.mjs missing -- run ./scripts/build_wasm.sh first" >&2
  exit 1
fi

echo "staging static payload -> electron/payload/"
./scripts/package_web.sh electron/payload > /dev/null
rm -f electron/payload.tar.gz          # package_web.sh also tars its outdir
rm -f electron/payload/serve.js        # the shell serves over app://, no server
du -sh electron/payload | sed 's/^/  /'

[[ "$MODE" == "--stage-only" ]] && { echo "staged. run: cd electron && npm start"; exit 0; }

cd electron
if [[ ! -d node_modules ]]; then
  echo "installing electron + electron-builder (first run, ~200 MB)"
  npm install --no-fund --no-audit
fi

case "$MODE" in
  --dir)   npx electron-builder --dir ;;
  --linux) npx electron-builder --linux ;;
  --win)   npx electron-builder --win ;;
  --mac)   npx electron-builder --mac ;;
  *)       npx electron-builder ;;
esac
echo "artifacts in electron/dist/"
