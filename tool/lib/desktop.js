//
// Desktop bridge. In the Electron shell `window.digidream` exists and file
// operations become native dialogs that write real paths; in a browser every
// function here falls back to the web behaviour (<a download>, <input
// type=file>), so the tool pages are one codebase either way.
//
const api = globalThis.digidream || null;
// Capacitor (Android): a WebView has no download manager, so saving goes
// through the Filesystem plugin instead of <a download>.
const native = globalThis.Capacitor?.isNativePlatform?.() ? globalThis.Capacitor.Plugins : null;

export const isDesktop = !!api;
export const desktop = api;

// Extensions from an <input accept> string: '.mid,.midi,.mstk' -> [mid,midi,mstk]
const extsOf = (accept) => (accept || '')
  .split(',')
  .map((s) => s.trim().replace(/^\./, ''))
  .filter((s) => s && !s.includes('/'));

// Wrap {name, path, data} from the shell so the rest of the code can treat
// it exactly like a DOM File.
export const wrapFile = (f) => ({
  name: f.name,
  path: f.path,
  size: f.data.byteLength,
  arrayBuffer: async () => f.data.buffer.slice(f.data.byteOffset, f.data.byteOffset + f.data.byteLength),
  text: async () => new TextDecoder().decode(f.data),
});

// Returns [] when cancelled. `accept` is the same string an <input> takes.
export async function pickFiles(accept, multiple = false) {
  if (api) {
    const picked = await api.open({ multiple, extensions: extsOf(accept) });
    return picked.map(wrapFile);
  }
  return new Promise((resolve) => {
    const inp = document.createElement('input');
    inp.type = 'file';
    inp.accept = accept;
    inp.multiple = multiple;
    inp.onchange = () => resolve([...inp.files]);
    inp.click();
  });
}

const base64 = (data) => {
  const b = typeof data === 'string' ? new TextEncoder().encode(data)
          : data instanceof ArrayBuffer ? new Uint8Array(data)
          : new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
  let s = '';  // chunked: a .wav render is tens of MB
  for (let i = 0; i < b.length; i += 0x8000) s += String.fromCharCode(...b.subarray(i, i + 0x8000));
  return btoa(s);
};

// Write bytes out. Desktop: Save dialog (or straight to `toPath`), resolving
// to {path, name} or null if cancelled. Android: the app's Documents folder.
// Browser: a download, resolving to {name} -- the browser owns where it lands.
export async function saveBytes(name, data, mime = 'application/octet-stream', { toPath = null } = {}) {
  if (api) return api.save(name, data, { toPath });
  if (native?.Filesystem) {
    const opts = { path: name, directory: 'DOCUMENTS' };
    await native.Filesystem.writeFile({ ...opts, data: base64(data), recursive: true });
    return { name, path: (await native.Filesystem.getUri(opts)).uri };
  }
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([data], { type: mime }));
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 5000);
  return { name, path: null };
}

// A set of files at once: one folder picker on the desktop, a burst of
// downloads in a browser. Resolves to {dir, count} or null if cancelled.
export async function saveAllBytes(files, mime = 'application/octet-stream') {
  if (!files.length) return { dir: null, count: 0 };
  if (api) return api.saveMany(files);
  for (const f of files) await saveBytes(f.name, f.data, mime);
  return { dir: null, count: files.length };
}

// Native message box in the app, window.confirm in a browser.
export async function confirmAction(message, detail = '', ok = 'OK') {
  if (api) return api.confirm(message, detail, ok);
  return window.confirm(detail ? `${message}\n\n${detail}` : message);
}

// Window title / macOS edited dot / proxy icon. No-op in a browser.
export function setDocument(doc) { api?.setDocument(doc); }
export function onMenu(fn) { api?.onMenu(fn); }
export function onOpenPath(fn) { api?.onOpenPath(fn); }
// Any file the app was launched with (double-clicked project), else null.
export async function launchFile() { return api ? api.ready() : null; }
