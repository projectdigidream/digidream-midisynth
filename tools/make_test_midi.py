#!/usr/bin/env python3
"""Generate the deterministic reference MIDI corpus for golden tests.

Byte-exact by construction (no timestamps, no randomness) -- running this
twice must produce identical files. Each file targets a different slice of
the engine: melody+bend+CC, drums+choke, tempo changes, dense polyphony.
"""
import os
import struct

OUT = os.path.join(os.path.dirname(__file__), "..", "test", "golden")


def vlq(v):
    out = [v & 0x7F]
    v >>= 7
    while v:
        out.append(0x80 | (v & 0x7F))
        v >>= 7
    return bytes(reversed(out))


class Track:
    def __init__(self):
        self.data = b""

    def ev(self, delta, *bytes_):
        self.data += vlq(delta) + bytes(bytes_)
        return self

    def tempo(self, delta, us):
        self.data += vlq(delta) + bytes([0xFF, 0x51, 3, (us >> 16) & 255, (us >> 8) & 255, us & 255])
        return self

    def marker(self, delta, name):
        b = name.encode()
        self.data += vlq(delta) + bytes([0xFF, 0x06]) + vlq(len(b)) + b
        return self

    def end(self, delta=0):
        return self.ev(delta, 0xFF, 0x2F, 0)


def smf(format_, division, tracks):
    out = b"MThd" + struct.pack(">IHHH", 6, format_, len(tracks), division)
    for t in tracks:
        out += b"MTrk" + struct.pack(">I", len(t.data)) + t.data
    return out


def write(name, data):
    path = os.path.join(OUT, name)
    with open(path, "wb") as f:
        f.write(data)
    print(f"wrote {path} ({len(data)} bytes)")


def melody():
    """Melody with program change, bend, mod wheel, sustain, expression."""
    t = Track()
    t.tempo(0, 500000)
    t.ev(0, 0xC0, 48)          # strings
    t.ev(0, 0xB0, 7, 100)
    t.ev(0, 0xB0, 91, 70)      # reverb send
    notes = [60, 64, 67, 72, 67, 64, 62, 59, 60]
    for i, n in enumerate(notes):
        t.ev(0, 0x90, n, 70 + i * 5)
        t.ev(240, 0x80, n, 0)
    t.ev(0, 0xB0, 64, 127)     # pedal down
    t.ev(0, 0x90, 55, 90)
    t.ev(120, 0x80, 55, 0)     # deferred by pedal
    t.ev(0, 0x90, 59, 90)
    t.ev(120, 0x80, 59, 0)
    t.ev(240, 0xB0, 64, 0)     # pedal up
    # Bend sweep on a held note.
    t.ev(120, 0x90, 62, 100)
    for step in range(16):
        bend = 8192 + (step + 1) * 480
        t.ev(30, 0xE0, bend & 0x7F, (bend >> 7) & 0x7F)
    t.ev(120, 0xE0, 0, 64)     # center
    t.ev(0, 0xB0, 1, 90)       # mod wheel vibrato
    t.ev(480, 0x80, 62, 0)
    t.ev(0, 0xB0, 11, 40)      # expression down
    t.ev(0, 0x90, 48, 100)
    t.ev(480, 0x80, 48, 0)
    t.end(240)
    return smf(0, 480, [t])


def drums():
    """Drum groove: kick/snare/hats with open-hat chokes, toms, crash."""
    t = Track()
    t.tempo(0, 500000)
    for bar in range(4):
        for beat in range(4):
            t.ev(0, 0x99, 36, 110)                 # kick
            if beat % 2 == 1:
                t.ev(0, 0x99, 38, 100)             # snare
            t.ev(0, 0x99, 42, 80)                  # closed hat
            t.ev(120, 0x99, 46 if beat == 3 else 42, 70)  # open hat on the 4th
            t.ev(120, 0x89, 42, 0)
    t.ev(0, 0x99, 49, 120)     # crash
    t.ev(0, 0x99, 45, 100)
    t.ev(120, 0x99, 47, 100)
    t.ev(120, 0x99, 50, 100)
    t.end(480)
    return smf(0, 480, [t])


def tempos():
    """Tempo map stress: changes mid-note, plus loop markers."""
    t0 = Track()
    t0.tempo(0, 500000)
    t0.tempo(480, 250000)
    t0.tempo(960, 1000000)
    t0.tempo(1440, 375000)
    t0.end(1920)
    t1 = Track()
    t1.marker(0, "loopStart")
    t1.ev(0, 0xC0, 81)
    for i in range(8):
        t1.ev(0, 0x90, 57 + i, 100)
        t1.ev(240, 0x80, 57 + i, 0)
    t1.marker(0, "loopEnd")
    t1.end()
    return smf(1, 480, [t0, t1])


def poly():
    """Dense polyphony across channels: stealing under sustained chords."""
    t = Track()
    t.tempo(0, 500000)
    for ch in range(4):
        t.ev(0, 0xC0 | ch, [0, 48, 89, 19][ch])
    for wave in range(6):
        for ch in range(4):
            base = 36 + wave * 3 + ch * 12
            for k in range(4):
                t.ev(0, 0x90 | ch, base + k * 7, 90)
        t.ev(480, 0xB0, 123, 0)  # All Notes Off, ch 0 only
    for ch in range(4):
        t.ev(0, 0xB0 | ch, 123, 0)
    t.end(960)
    return smf(0, 480, [t])


def main():
    os.makedirs(OUT, exist_ok=True)
    write("melody.mid", melody())
    write("drums.mid", drums())
    write("tempos.mid", tempos())
    write("poly.mid", poly())


if __name__ == "__main__":
    main()
