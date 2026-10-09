#!/usr/bin/env bash
#
# Package the DigidreamSynth web tools (DAW, patch editor, track cutter) into
# a self-contained static bundle for hosting: dist-web/ plus a tarball.
#
# The bundle is plain static files -- any web server works. Two requirements:
#   * HTTPS (or localhost): browsers require a secure context for
#     AudioWorklet and Web MIDI.
#   * .mjs served as text/javascript (deploy/nginx.conf handles it).
# No COOP/COEP headers are needed: the WASM ships as a single ES module.
#
#   ./scripts/package_web.sh [outdir]     default: dist-web
#
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=${1:-dist-web}

if [[ ! -f tool/dist/midisynth.mjs ]]; then
  echo "tool/dist/midisynth.mjs missing -- run ./scripts/build_wasm.sh first" >&2
  exit 1
fi

rm -rf "$OUT"
mkdir -p "$OUT/tool" "$OUT/content" "$OUT/test/golden"

# The three pages + their modules + the engine.
cp tool/*.html tool/*.js "$OUT/tool/"
cp -R tool/lib tool/daw tool/dist "$OUT/tool/"

# Content the pages fetch at boot / demo songs the buttons load.
cp -R content/patches content/drums content/fx "$OUT/content/"
cp test/golden/*.mid "$OUT/test/golden/" 2>/dev/null || true

# Landing page.
cat > "$OUT/index.html" <<'HTML'
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>DigidreamSynth</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
  body { margin: 0; background: #14161a; color: #d6d9e0; display: flex; min-height: 100vh;
         align-items: center; justify-content: center;
         font: 15px/1.5 -apple-system, "Segoe UI", sans-serif; }
  main { max-width: 560px; padding: 24px; }
  h1 { color: #64b5f6; font-size: 22px; }
  a.card { display: block; margin: 10px 0; padding: 14px 16px; border: 1px solid #32363f;
           border-radius: 8px; color: #d6d9e0; text-decoration: none; background: #1d2026; }
  a.card:hover { border-color: #64b5f6; }
  a.card b { color: #64b5f6; }
  p { color: #8a8f9c; }
</style>
</head>
<body>
<main>
  <h1>DigidreamSynth</h1>
  <p>A deterministic MIDI music engine, in your browser. Everything renders
     bit-identically to the game runtime.</p>
  <a class="card" href="tool/daw.html"><b>DAW</b> — arrange, record, edit, master, cut loops, test cues</a>
  <a class="card" href="tool/"><b>Patch editor</b> — author instruments and drum kits</a>
  <a class="card" href="tool/cutter.html"><b>Track cutter</b> — slice baked .mstk tracks into gameplay pieces</a>
  <p>Audio starts after a click (browser policy). A MIDI keyboard is picked
     up automatically if the site is served over HTTPS.</p>
</main>
</body>
</html>
HTML

# The static server doubles as a production fallback (bind with --host).
cp tool/serve.js "$OUT/serve.js"

SIZE=$(du -sh "$OUT" | cut -f1)
tar -czf "$OUT.tar.gz" "$OUT"
echo "bundle: $OUT ($SIZE), tarball: $OUT.tar.gz"
echo "try it: node $OUT/serve.js 8930 --root $OUT   ->  http://localhost:8930/"
