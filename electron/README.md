# DigidreamSynth desktop (Electron)

The DAW as a standalone application: the same `tool/` code the browser runs,
wrapped in a shell that gives it a native menu, real Save/Open dialogs, file
associations and no server.

```
cd midisynth/electron
npm install            # electron + electron-builder (~200 MB, once)
npm start              # runs the working copy in midisynth/tool
```

Installers:

```
./scripts/package_electron.sh            # .dmg / .exe / .AppImage for this platform
./scripts/package_electron.sh --dir      # unpacked app only (fast)
./scripts/package_electron.sh --stage-only   # refresh electron/payload/ and stop
./scripts/package_electron.sh --linux    # cross-build Linux targets
```

Artifacts land in `electron/dist/`.

## How it is put together

| | |
|---|---|
| `main.js` | app lifecycle, the `app://digidream/` protocol, window, native menu, IPC, permissions |
| `preload.js` | the only bridge to the page: `window.digidream` (open/save/confirm/document/menu) |
| `run.js` | `npm start` helper — clears `ELECTRON_RUN_AS_NODE` (VS Code terminals export it) |
| `payload/` | the static bundle from `scripts/package_web.sh`, staged at build time (gitignored) |
| `resources/icon.svg` → `icon.png` | app icon (the PNG is rendered from the SVG; electron-builder makes `.icns`/`.ico`) |

**Why a custom protocol and not `file://`** — the DAW is ES modules plus an
AudioWorklet that compiles WASM and `fetch()`es its instrument content.
`file://` fails all three (module CORS); `app://digidream/` is a standard,
secure origin served straight from disk, with no TCP port open anywhere.

**Where the files come from** — unpackaged runs serve `midisynth/` itself, so
`npm start` always shows the working copy; packaged apps serve `payload/`
from inside `app.asar`. Both go through the same handler.

**What the page gains** (all optional — `tool/lib/desktop.js` falls back to
`<a download>` / `<input type=file>` in a browser, so the web build is
unchanged):

* native File menu, Edit, Transport, View (tabs), Window, Help
* Save writes over the open `.msproj`; Save As asks; the title bar, the macOS
  edited dot and the proxy icon follow the document
* one folder picker for slice exports instead of a dialog per file
* `.msproj` / `.mstk` file associations — double-click a project to open it
* Web MIDI granted by the shell; the engine boots without the Start-audio click

## Dependencies

`electron` and `electron-builder` are **build-time only** — the shipped app
contains `main.js`, `preload.js` and `payload/` and nothing else (no
`node_modules` inside `app.asar`). Keep `npm audit` at zero anyway: the
builder chain unpacks downloaded tarballs on your machine, which is exactly
what the node-tar advisories are about. Electron itself is the app's
Chromium, so track a supported major (the last three) rather than pinning an
old one.

## Tests

```
cd midisynth/electron && npm install
node test/electron_smoke.mjs              # dev shell
node test/electron_smoke.mjs --packaged   # the built app in electron/dist
```
(from a directory with `puppeteer-core`; skipped otherwise). It drives the
real app over the remote debugging port and checks the page loads from
`app://`, the engine boots with audio actually running, the preload bridge is
there, and saving writes a real file.
