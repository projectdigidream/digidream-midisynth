//
// Live output scope: log-spectrum bars behind a trigger-stabilised waveform.
//
import { canvas2d } from './ui.js';

export function createScope(canvas, analyser) {
  const time = new Uint8Array(4096);
  let freq = null;
  let raf = 0;
  const draw = () => {
    raf = requestAnimationFrame(draw);
    const { g, w, h, dpr } = canvas2d(canvas);
    g.fillStyle = '#101318';
    g.fillRect(0, 0, w, h);
    if (!analyser) return;
    if (!freq) freq = new Uint8Array(analyser.frequencyBinCount);

    analyser.getByteFrequencyData(freq);
    g.fillStyle = 'rgba(74,144,217,0.28)';
    const bars = 96;
    for (let i = 0; i < bars; i++) {
      const f0 = Math.pow(freq.length, i / bars);
      const f1 = Math.pow(freq.length, (i + 1) / bars);
      let peak = 0;
      for (let k = Math.floor(f0); k < Math.max(f0 + 1, f1); k++) peak = Math.max(peak, freq[k] || 0);
      const bh = (peak / 255) * h * 0.9;
      g.fillRect((i / bars) * w, h - bh, w / bars - 1 * dpr, bh);
    }

    analyser.getByteTimeDomainData(time);
    let start = 0;
    for (let i = 1; i < 2048; i++)
      if (time[i - 1] < 128 && time[i] >= 128) { start = i; break; }
    g.strokeStyle = '#6fd66f';
    g.lineWidth = 1.5 * dpr;
    g.beginPath();
    const n = 2048;
    for (let i = 0; i < n; i++) {
      const v = time[start + i] / 255;
      const x = (i / n) * w;
      const y = h - v * h;
      if (i === 0) g.moveTo(x, y); else g.lineTo(x, y);
    }
    g.stroke();
    g.strokeStyle = 'rgba(255,255,255,0.08)';
    g.lineWidth = 1;
    g.beginPath(); g.moveTo(0, h / 2); g.lineTo(w, h / 2); g.stroke();
  };
  return {
    start() { if (!raf) draw(); },
    stop() { cancelAnimationFrame(raf); raf = 0; },
  };
}
