#!/usr/bin/env python3
"""List the GM program numbers (and drum notes) a set of SMF files actually
uses -- the §17 content plan drives patch authoring from this."""
import sys
from collections import Counter

GM = [
    "Acoustic Grand Piano","Bright Acoustic Piano","Electric Grand Piano","Honky-tonk Piano",
    "Electric Piano 1","Electric Piano 2","Harpsichord","Clavinet","Celesta","Glockenspiel",
    "Music Box","Vibraphone","Marimba","Xylophone","Tubular Bells","Dulcimer","Drawbar Organ",
    "Percussive Organ","Rock Organ","Church Organ","Reed Organ","Accordion","Harmonica",
    "Tango Accordion","Acoustic Guitar (nylon)","Acoustic Guitar (steel)","Electric Guitar (jazz)",
    "Electric Guitar (clean)","Electric Guitar (muted)","Overdriven Guitar","Distortion Guitar",
    "Guitar Harmonics","Acoustic Bass","Electric Bass (finger)","Electric Bass (pick)",
    "Fretless Bass","Slap Bass 1","Slap Bass 2","Synth Bass 1","Synth Bass 2","Violin","Viola",
    "Cello","Contrabass","Tremolo Strings","Pizzicato Strings","Orchestral Harp","Timpani",
    "String Ensemble 1","String Ensemble 2","Synth Strings 1","Synth Strings 2","Choir Aahs",
    "Voice Oohs","Synth Voice","Orchestra Hit","Trumpet","Trombone","Tuba","Muted Trumpet",
    "French Horn","Brass Section","Synth Brass 1","Synth Brass 2","Soprano Sax","Alto Sax",
    "Tenor Sax","Baritone Sax","Oboe","English Horn","Bassoon","Clarinet","Piccolo","Flute",
    "Recorder","Pan Flute","Blown Bottle","Shakuhachi","Whistle","Ocarina","Lead 1 (square)",
    "Lead 2 (sawtooth)","Lead 3 (calliope)","Lead 4 (chiff)","Lead 5 (charang)","Lead 6 (voice)",
    "Lead 7 (fifths)","Lead 8 (bass+lead)","Pad 1 (new age)","Pad 2 (warm)","Pad 3 (polysynth)",
    "Pad 4 (choir)","Pad 5 (bowed)","Pad 6 (metallic)","Pad 7 (halo)","Pad 8 (sweep)",
    "FX 1 (rain)","FX 2 (soundtrack)","FX 3 (crystal)","FX 4 (atmosphere)","FX 5 (brightness)",
    "FX 6 (goblins)","FX 7 (echoes)","FX 8 (sci-fi)","Sitar","Banjo","Shamisen","Koto","Kalimba",
    "Bag pipe","Fiddle","Shanai","Tinkle Bell","Agogo","Steel Drums","Woodblock","Taiko Drum",
    "Melodic Tom","Synth Drum","Reverse Cymbal","Guitar Fret Noise","Breath Noise","Seashore",
    "Bird Tweet","Telephone Ring","Helicopter","Applause","Gunshot"]


def scan(path, programs, drums):
    data = open(path, "rb").read()
    if data[:4] != b"MThd":
        print(f"{path}: not SMF", file=sys.stderr)
        return
    pos = 8 + int.from_bytes(data[4:8], "big")

    def vlq(p):
        v = 0
        for _ in range(4):
            b = data[p]
            p += 1
            v = (v << 7) | (b & 0x7F)
            if not (b & 0x80):
                return v, p
        return v, p

    while pos + 8 <= len(data):
        kind = data[pos:pos + 4]
        clen = int.from_bytes(data[pos + 4:pos + 8], "big")
        body_start = pos + 8
        pos = body_start + clen
        if kind != b"MTrk":
            continue
        p = body_start
        end = min(pos, len(data))
        running = 0
        while p < end:
            _, p = vlq(p)
            if p >= end:
                break
            status = data[p]
            if status < 0x80:
                status = running
            else:
                p += 1
                if status < 0xF0:
                    running = status
            if status == 0xFF:
                p += 1
                mlen, p = vlq(p)
                p += mlen
                continue
            if status in (0xF0, 0xF7):
                slen, p = vlq(p)
                p += slen
                continue
            kind_hi = status & 0xF0
            nbytes = 1 if kind_hi in (0xC0, 0xD0) else 2
            d = data[p:p + nbytes]
            p += nbytes
            ch = status & 0x0F
            if kind_hi == 0xC0 and ch != 9:
                programs[d[0]] += 1
            if kind_hi == 0x90 and ch == 9 and len(d) > 1 and d[1] > 0:
                drums[d[0]] += 1


def main():
    programs, drums = Counter(), Counter()
    for path in sys.argv[1:]:
        scan(path, programs, drums)
    print("== melodic programs ==")
    for prog, n in sorted(programs.items()):
        print(f"  {prog:3d}  {GM[prog]:32s} x{n}")
    print("== drum notes (ch 10) ==")
    for note, n in sorted(drums.items()):
        print(f"  {note:3d}  x{n}")


if __name__ == "__main__":
    main()
