//
// DigidreamSynth desktop shell (Electron).
//
// The DAW is the same static app that runs on the web -- it is loaded over a
// private `app://digidream/` protocol rather than file:// because ES modules,
// AudioWorklet and fetch() all need a real origin (and a secure context).
// Everything the desktop adds is opt-in on the page side: the renderer uses
// window.digidream when it exists and falls back to <a download> / <input
// type=file> in a browser, so tool/ stays one codebase.
//
const { app, BrowserWindow, Menu, dialog, ipcMain, protocol, session, shell } = require('electron');
const path = require('node:path');
const fs = require('node:fs');
const fsp = require('node:fs/promises');

const SCHEME = 'app';
const HOST = 'digidream';
// Packaged: the static payload staged by scripts/package_electron.sh.
// Unpackaged: the repo tree itself, so `npm start` always runs the working
// copy rather than the last staged snapshot (needs tool/dist/midisynth.mjs --
// scripts/build_wasm.sh).
const STAGED = path.join(__dirname, 'payload');
const DEV_ROOT = path.join(__dirname, '..');
const ROOT = (!app.isPackaged && fs.existsSync(path.join(DEV_ROOT, 'tool', 'daw.html')))
  ? DEV_ROOT
  : STAGED;

const PAGES = {
  daw: 'tool/daw.html',
  patch: 'tool/index.html',
  cutter: 'tool/cutter.html',
};
const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.json': 'application/json',
  '.mid': 'audio/midi',
  '.wasm': 'application/wasm',
  '.css': 'text/css',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
};
// Dialog filters by extension, so the page can keep calling save(name, bytes).
const FILTERS = {
  '.msproj': [{ name: 'DigidreamSynth project', extensions: ['msproj'] }],
  '.mstk': [{ name: 'DigidreamSynth track', extensions: ['mstk'] }],
  '.mid': [{ name: 'MIDI file', extensions: ['mid', 'midi'] }],
  '.wav': [{ name: 'WAV audio', extensions: ['wav'] }],
  '.json': [{ name: 'JSON', extensions: ['json'] }],
};
const filtersFor = (name) => [
  ...(FILTERS[path.extname(name).toLowerCase()] || []),
  { name: 'All files', extensions: ['*'] },
];

protocol.registerSchemesAsPrivileged([{
  scheme: SCHEME,
  privileges: { standard: true, secure: true, supportFetchAPI: true, stream: true, codeCache: true },
}]);

const pageUrl = (page) => `${SCHEME}://${HOST}/${PAGES[page] || PAGES.daw}`;

// ---------------------------------------------------------------- state
const windows = new Map();   // BrowserWindow -> {page, projectPath, dirty}
let lastDir = app.getPath('documents');
let pendingOpen = null;      // file passed on the command line / open-file

function meta(win) {
  if (!windows.has(win)) windows.set(win, { page: 'daw', projectPath: null, dirty: false });
  return windows.get(win);
}

// ---------------------------------------------------------------- windows
function createWindow(page = 'daw') {
  const win = new BrowserWindow({
    width: page === 'daw' ? 1440 : 1180,
    height: page === 'daw' ? 900 : 860,
    minWidth: 900,
    minHeight: 600,
    backgroundColor: '#14161a',
    show: !process.env.DGS_SMOKE,   // the smoke test drives it windowless
    title: page === 'daw' ? 'DigidreamSynth' : `DigidreamSynth — ${page}`,
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
      // A desktop app has already been "clicked": let the engine boot without
      // the web build's Start-audio gesture.
      autoplayPolicy: 'no-user-gesture-required',
    },
  });
  meta(win).page = page;
  win.loadURL(pageUrl(page));

  // Everything external opens in the real browser; nothing navigates away.
  win.webContents.setWindowOpenHandler(({ url }) => {
    if (/^https?:/.test(url)) shell.openExternal(url);
    return { action: 'deny' };
  });
  win.webContents.on('will-navigate', (e, url) => {
    if (!url.startsWith(`${SCHEME}://${HOST}/`)) e.preventDefault();
  });
  // The page's beforeunload guard is advisory in Electron: ask here instead.
  win.webContents.on('will-prevent-unload', (e) => {
    const choice = dialog.showMessageBoxSync(win, {
      type: 'warning',
      buttons: ['Discard changes', 'Cancel'],
      defaultId: 1,
      cancelId: 1,
      message: 'This project has unsaved changes.',
      detail: 'Closing now loses everything since the last save.',
    });
    // In Electron preventDefault() here means "ignore the page's guard", i.e.
    // let the close go through.
    if (choice === 0) e.preventDefault();
  });
  if (process.env.DGS_SMOKE) {   // the smoke test reads the renderer's console
    win.webContents.on('console-message', (...a) => {
      const d = (a[0] && typeof a[0] === 'object' && 'message' in a[0]) ? a[0]
              : { level: a[1], message: a[2], lineNumber: a[3], sourceId: a[4] };
      console.log(`[renderer:${d.level}] ${d.message} (${d.sourceId}:${d.lineNumber})`);
    });
    win.webContents.on('did-fail-load', (e, code, desc, url) => {
      console.log(`[renderer] load failed ${code} ${desc} ${url}`);
    });
  }
  win.on('closed', () => windows.delete(win));
  return win;
}

const focused = () => BrowserWindow.getFocusedWindow() || BrowserWindow.getAllWindows()[0] || null;
const sendMenu = (action) => focused()?.webContents.send('dgs:menu', action);

// ---------------------------------------------------------------- menu
function buildMenu() {
  const isMac = process.platform === 'darwin';
  const item = (label, action, accelerator) => ({ label, accelerator, click: () => sendMenu(action) });
  const template = [
    ...(isMac ? [{ role: 'appMenu' }] : []),
    {
      label: 'File',
      submenu: [
        item('New Project', 'new', 'CmdOrCtrl+N'),
        item('Open Project…', 'open', 'CmdOrCtrl+O'),
        { type: 'separator' },
        item('Save', 'save', 'CmdOrCtrl+S'),
        item('Save As…', 'save-as', 'CmdOrCtrl+Shift+S'),
        { type: 'separator' },
        {
          label: 'Import',
          submenu: [
            item('MIDI or baked track (.mid / .mstk)…', 'import-song'),
            item('Instrument bank (bank.json)…', 'import-bank'),
            item('Drum kit (kit.json)…', 'import-kit'),
            item('Effects (fx.json)…', 'import-fx'),
            item('Mastering (master.json)…', 'import-master'),
          ],
        },
        {
          label: 'Export',
          submenu: [
            item('MIDI (.mid)', 'export-mid'),
            item('Bake track (.mstk)', 'export-mstk', 'CmdOrCtrl+B'),
            item('Render audio (.wav)', 'export-wav', 'CmdOrCtrl+R'),
            item('Slices (.mstk)', 'export-slices'),
            item('Cue table (cues.json)', 'export-cues'),
            { type: 'separator' },
            item('bank.json', 'export-bank'),
            item('kit.json', 'export-kit'),
            item('fx.json', 'export-fx'),
            item('master.json', 'export-master'),
          ],
        },
        { type: 'separator' },
        isMac ? { role: 'close' } : { role: 'quit' },
      ],
    },
    {
      label: 'Edit',
      submenu: [
        item('Undo', 'undo', 'CmdOrCtrl+Z'),
        item('Redo', 'redo', isMac ? 'Shift+Cmd+Z' : 'Ctrl+Y'),
        { type: 'separator' },
        { role: 'cut' }, { role: 'copy' }, { role: 'paste' }, { role: 'selectAll' },
      ],
    },
    {
      label: 'Transport',
      submenu: [
        item('Play / Stop', 'play-stop', 'Space'),
        item('Record', 'record', 'CmdOrCtrl+Shift+R'),
        item('Return to Start', 'rewind', 'Home'),
        { type: 'separator' },
        item('Toggle Loop', 'loop'),
        item('Toggle Metronome', 'click'),
      ],
    },
    {
      label: 'View',
      submenu: [
        item('Arrange', 'tab-arrange', 'CmdOrCtrl+1'),
        item('Track', 'tab-track', 'CmdOrCtrl+2'),
        item('Synth', 'tab-synth', 'CmdOrCtrl+3'),
        item('Master', 'tab-master', 'CmdOrCtrl+4'),
        item('Cut & Loop', 'tab-cut', 'CmdOrCtrl+5'),
        item('Cues', 'tab-cues', 'CmdOrCtrl+6'),
        { type: 'separator' },
        { role: 'resetZoom' }, { role: 'zoomIn' }, { role: 'zoomOut' },
        { type: 'separator' },
        { role: 'togglefullscreen' },
        { role: 'toggleDevTools' },
        { label: 'Reload', accelerator: 'CmdOrCtrl+Shift+Alt+R', click: () => focused()?.reload() },
      ],
    },
    {
      label: 'Window',
      submenu: [
        { label: 'New DAW Window', click: () => createWindow('daw') },
        { label: 'Patch Editor', click: () => createWindow('patch') },
        { label: 'Track Cutter', click: () => createWindow('cutter') },
        { type: 'separator' },
        { role: 'minimize' },
        ...(isMac ? [{ role: 'zoom' }, { type: 'separator' }, { role: 'front' }] : []),
      ],
    },
    {
      role: 'help',
      submenu: [
        { label: 'Patch format reference', click: () => openDoc('docs/PATCH_FORMAT.md') },
        { label: 'DAW guide', click: () => openDoc('docs/DAW.md') },
      ],
    },
  ];
  Menu.setApplicationMenu(Menu.buildFromTemplate(template));
}

function openDoc(rel) {
  // Unpackaged: the repo's docs/. Packaged: extraResources/docs.
  const candidates = [path.join(ROOT, rel), path.join(process.resourcesPath || '', path.basename(path.dirname(rel)), path.basename(rel))];
  const found = candidates.find((p) => fs.existsSync(p));
  if (found) shell.openPath(found);
  else dialog.showMessageBox({ message: 'Documentation is not bundled in this build.', detail: rel });
}

// ---------------------------------------------------------------- IPC
function register() {
  ipcMain.handle('dgs:open', async (e, { multiple = false, extensions = null } = {}) => {
    const win = BrowserWindow.fromWebContents(e.sender);
    const filters = extensions
      ? [{ name: 'Supported files', extensions }, { name: 'All files', extensions: ['*'] }]
      : [{ name: 'All files', extensions: ['*'] }];
    const r = await dialog.showOpenDialog(win, {
      defaultPath: lastDir,
      filters,
      properties: multiple ? ['openFile', 'multiSelections'] : ['openFile'],
    });
    if (r.canceled || !r.filePaths.length) return [];
    lastDir = path.dirname(r.filePaths[0]);
    return Promise.all(r.filePaths.map(async (p) => ({
      name: path.basename(p),
      path: p,
      data: await fsp.readFile(p),
    })));
  });

  // Save with a dialog, or straight to a known path (Save over an open file).
  ipcMain.handle('dgs:save', async (e, { name, data, toPath = null } = {}) => {
    const win = BrowserWindow.fromWebContents(e.sender);
    let out = toPath;
    if (!out) {
      const r = await dialog.showSaveDialog(win, {
        defaultPath: path.join(lastDir, name),
        filters: filtersFor(name),
      });
      if (r.canceled || !r.filePath) return null;
      out = r.filePath;
    }
    await fsp.writeFile(out, Buffer.from(data));
    lastDir = path.dirname(out);
    app.addRecentDocument(out);
    return { path: out, name: path.basename(out) };
  });

  // Many files at once (slice sets): one folder picker, not N dialogs.
  ipcMain.handle('dgs:save-many', async (e, { files = [] } = {}) => {
    const win = BrowserWindow.fromWebContents(e.sender);
    const r = await dialog.showOpenDialog(win, {
      title: 'Choose a folder for the exported files',
      defaultPath: lastDir,
      properties: ['openDirectory', 'createDirectory'],
      buttonLabel: 'Export here',
    });
    if (r.canceled || !r.filePaths.length) return null;
    const dir = r.filePaths[0];
    for (const f of files) {
      await fsp.writeFile(path.join(dir, path.basename(f.name)), Buffer.from(f.data));
    }
    lastDir = dir;
    return { dir, count: files.length };
  });

  ipcMain.handle('dgs:confirm', async (e, { message, detail, ok = 'OK' } = {}) => {
    const win = BrowserWindow.fromWebContents(e.sender);
    const r = await dialog.showMessageBox(win, {
      type: 'warning', buttons: [ok, 'Cancel'], defaultId: 1, cancelId: 1, message, detail,
    });
    return r.response === 0;
  });

  // Window chrome reflects the document: name, the macOS edited dot, and the
  // proxy icon of the file on disk.
  ipcMain.on('dgs:document', (e, { name, path: p, dirty }) => {
    const win = BrowserWindow.fromWebContents(e.sender);
    if (!win) return;
    const m = meta(win);
    m.projectPath = p || null;
    m.dirty = !!dirty;
    win.setTitle(`${dirty ? '• ' : ''}${name || 'Untitled'} — DigidreamSynth`);
    win.setDocumentEdited(!!dirty);
    if (p) win.setRepresentedFilename(p);
  });

  ipcMain.on('dgs:reveal', (e, p) => { if (p) shell.showItemInFolder(p); });

  // The renderer says it is live; hand it any file we were launched with.
  ipcMain.handle('dgs:ready', () => {
    const p = pendingOpen;
    pendingOpen = null;
    return p ? readDoc(p) : null;
  });
}

// ---------------------------------------------------------------- launch
const openable = (p) => p && /\.(msproj|mid|midi|mstk)$/i.test(p) && fs.existsSync(p);
const fromArgv = (argv) => argv.slice(1).find((a) => openable(a)) || null;

const readDoc = (p) => {
  try { return { name: path.basename(p), path: p, data: fs.readFileSync(p) }; }
  catch { return null; }
};

app.on('open-file', (e, p) => {           // macOS: double-click / drop on dock
  e.preventDefault();
  const win = focused();
  const doc = win && readDoc(p);
  if (doc) win.webContents.send('dgs:open-path', doc);
  else pendingOpen = p;                   // still starting up: hand it over at ready
});

// One instance owns the user's session (a second launch just hands its file
// over); the smoke test opts out so runs cannot collide.
const primary = process.env.DGS_SMOKE ? true : app.requestSingleInstanceLock();
if (!primary) {
  app.quit();
} else {
  app.on('second-instance', (e, argv) => {
    const win = focused();
    if (win) { if (win.isMinimized()) win.restore(); win.focus(); }
    const p = fromArgv(argv);
    const doc = p && win && readDoc(p);
    if (doc) win.webContents.send('dgs:open-path', doc);
  });

  app.whenReady().then(() => {
    protocol.handle(SCHEME, async (req) => {
      const url = new URL(req.url);
      if (url.host !== HOST) return new Response('not found', { status: 404 });
      let rel = decodeURIComponent(url.pathname);
      if (rel.endsWith('/')) rel += 'index.html';
      const file = path.normalize(path.join(ROOT, rel));
      if (!file.startsWith(ROOT)) return new Response('forbidden', { status: 403 });
      try {
        const data = await fsp.readFile(file);
        return new Response(data, {
          headers: {
            'content-type': MIME[path.extname(file).toLowerCase()] || 'application/octet-stream',
            // Local-only app: every source is this bundle, no network origin
            // is allowed at all. 'unsafe-eval' is required because the engine
            // compiles its WASM inside an AudioWorklet, where Chromium still
            // does not honour 'wasm-unsafe-eval' (re-checked on Electron 44 --
            // swapping it back makes the engine fail to boot).
            'content-security-policy':
              "default-src 'self'; script-src 'self' 'unsafe-eval'; " +
              "style-src 'self' 'unsafe-inline'; img-src 'self' data: blob:; " +
              "media-src 'self' blob:; connect-src 'self' blob: data:; worker-src 'self' blob:",
          },
        });
      } catch {
        return new Response('not found: ' + rel, { status: 404 });
      }
    });

    // Web MIDI is the point of the Track tab: grant it, refuse the rest.
    const allowed = new Set(['midi', 'midiSysex', 'audioCapture', 'fullscreen', 'clipboard-sanitized-write']);
    const ses = session.defaultSession;
    ses.setPermissionRequestHandler((wc, perm, done) => done(allowed.has(perm)));
    ses.setPermissionCheckHandler((wc, perm) => allowed.has(perm));

    register();
    buildMenu();
    pendingOpen = fromArgv(process.argv);
    createWindow('daw');

    app.on('activate', () => {
      if (BrowserWindow.getAllWindows().length === 0) createWindow('daw');
    });
  });

  app.on('window-all-closed', () => { if (process.platform !== 'darwin') app.quit(); });
}
