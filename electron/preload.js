//
// The only bridge between the DAW page and the desktop shell. Everything is
// a narrow, typed call -- no Node, no ipcRenderer, no file paths the page did
// not already get from a dialog.
//
const { contextBridge, ipcRenderer } = require('electron');

// The page hands us strings (JSON), ArrayBuffers (baked tracks, WAV) or
// typed arrays; the main process only ever sees bytes.
function toBytes(data) {
  if (typeof data === 'string') return new TextEncoder().encode(data);
  if (data instanceof ArrayBuffer) return new Uint8Array(data);
  if (ArrayBuffer.isView(data)) return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
  throw new TypeError('save: expected string, ArrayBuffer or typed array');
}

const listeners = { menu: [], openPath: [] };
ipcRenderer.on('dgs:menu', (e, action) => listeners.menu.forEach((f) => f(action)));
ipcRenderer.on('dgs:open-path', (e, file) => listeners.openPath.forEach((f) => f(file)));

contextBridge.exposeInMainWorld('digidream', {
  isDesktop: true,
  platform: process.platform,
  versions: { electron: process.versions.electron, chrome: process.versions.chrome },

  // [{name, path, data: Uint8Array}] -- empty when the dialog was cancelled.
  open: (opts = {}) => ipcRenderer.invoke('dgs:open', opts),
  // {path, name} or null when cancelled. toPath writes in place, no dialog.
  save: (name, data, { toPath = null } = {}) =>
    ipcRenderer.invoke('dgs:save', { name, data: toBytes(data), toPath }),
  // One folder picker, then every file written into it. {dir, count}|null.
  saveMany: (files) =>
    ipcRenderer.invoke('dgs:save-many', { files: files.map((f) => ({ name: f.name, data: toBytes(f.data) })) }),
  confirm: (message, detail, ok) => ipcRenderer.invoke('dgs:confirm', { message, detail, ok }),
  // Any file the app was launched with (double-clicked .msproj / .mstk).
  ready: () => ipcRenderer.invoke('dgs:ready'),

  setDocument: (doc) => ipcRenderer.send('dgs:document', doc),
  reveal: (path) => ipcRenderer.send('dgs:reveal', path),

  onMenu: (fn) => { listeners.menu.push(fn); },
  onOpenPath: (fn) => { listeners.openPath.push(fn); },
});
