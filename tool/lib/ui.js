//
// Small DOM helpers shared by the tool pages and the DAW tabs.
//
import { saveBytes } from './desktop.js';

export const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));

// Root-scoped id lookup: every panel works inside its own subtree so the
// same markup can be hosted several times (tool page, DAW tab).
export const scoped = (root) => (id) => root.querySelector('#' + id);

export function fmtTime(ms) {
  const s = Math.floor(ms / 1000);
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
}
export function fmtTimeTenths(ms) {
  const s = ms / 1000;
  return `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, '0')}.${Math.floor((s % 1) * 10)}`;
}

// Save bytes under `name`: a browser download, or a native Save dialog in
// the desktop build. Resolves to {name, path} (path only on the desktop) or
// null when the user cancelled; callers that report where a file went should
// await it, the rest can fire and forget.
export function download(name, data, mime = 'application/octet-stream', opts = {}) {
  return saveBytes(name, data, mime, opts);
}

// Log lines into an element; returns log(msg, cls).
export function makeLogger(el, max = 150) {
  return (msg, cls = '') => {
    if (!el) { console.log(msg); return; }
    const line = document.createElement('div');
    if (cls) line.className = cls;
    line.textContent = msg;
    el.appendChild(line);
    el.scrollTop = el.scrollHeight;
    while (el.childNodes.length > max) el.removeChild(el.firstChild);
  };
}

// Log a worklet report ({ok, warnings, errors}) under a tag.
export function logReport(log, what, r, okMsg = 'loaded ok') {
  for (const w of r.warnings || []) log(`[${what}] ${w}`, 'warn');
  for (const e of r.errors || []) log(`[${what}] ${e}`, 'err');
  if (r.ok && okMsg) log(`[${what}] ${okMsg}`, 'ok');
}

// Resize a canvas to its CSS box at device pixel ratio; returns {g, w, h, dpr}.
export function canvas2d(c) {
  const dpr = window.devicePixelRatio || 1;
  const w = Math.max(1, Math.round(c.clientWidth * dpr));
  const h = Math.max(1, Math.round(c.clientHeight * dpr));
  if (c.width !== w || c.height !== h) { c.width = w; c.height = h; }
  return { g: c.getContext('2d'), w, h, dpr };
}

export const isMstk = (u8) => u8.length >= 4 && u8[0] === 0x4D && u8[1] === 0x53 &&
                             u8[2] === 0x54 && u8[3] === 0x4B;  // "MSTK"

// Boot progress on a start overlay: swaps the Start button for an animated
// bar + stage line. Returns setStage(text); call overlay.remove() when done.
export function bootStatus(overlay) {
  const btn = overlay.querySelector('#start-btn');
  const box = document.createElement('div');
  box.style.cssText = 'width:300px;display:flex;flex-direction:column;gap:8px;align-items:center';
  box.innerHTML = `
    <div style="width:100%;height:6px;background:#23262e;border-radius:3px;overflow:hidden">
      <div class="bp" style="height:100%;width:34%;background:#64b5f6;border-radius:3px;animation:ms-boot 0.9s ease-in-out infinite alternate"></div>
    </div>
    <span class="bs" style="color:#8a8f9c;font:12px ui-monospace,monospace">starting…</span>`;
  const style = document.createElement('style');
  style.textContent = '@keyframes ms-boot{from{margin-left:0}to{margin-left:66%}}';
  box.appendChild(style);
  if (btn) btn.replaceWith(box);
  else overlay.appendChild(box);
  return (stage, failed = false) => {
    const el = box.querySelector('.bs');
    if (el) { el.textContent = stage; if (failed) el.style.color = 'var(--bad, #e07070)'; }
    if (failed) box.querySelector('.bp').style.animationPlayState = 'paused';
  };
}
