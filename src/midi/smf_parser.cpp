#include "smf_parser.h"

#include <algorithm>
#include <cstring>

namespace msynth {
namespace {

struct Reader {
    const u8* p;
    const u8* end;

    size_t remaining() const { return static_cast<size_t>(end - p); }
    bool eof() const { return p >= end; }
    u8 byte() { return *p++; }
    u32 be16() {
        const u32 v = (static_cast<u32>(p[0]) << 8) | p[1];
        p += 2;
        return v;
    }
    u32 be32() {
        const u32 v = (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
                      (static_cast<u32>(p[2]) << 8) | p[3];
        p += 4;
        return v;
    }
    // Variable-length quantity; max 4 bytes, a 5th continuation = corruption.
    bool vlq(u32& out) {
        u32 v = 0;
        for (int i = 0; i < 4; ++i) {
            if (eof()) return false;
            const u8 b = byte();
            v = (v << 7) | (b & 0x7F);
            if (!(b & 0x80)) {
                out = v;
                return true;
            }
        }
        return false;  // 5th continuation byte
    }
};

struct TrackEvents {
    std::vector<MidiEvent> events;
    std::vector<std::pair<u32, u32>> tempos;  // (tick, us_per_quarter)
};

// Parse one MTrk chunk; salvage up to the corruption point.
void parse_track(const u8* data, size_t len, u16 track_idx, bool smpte, ParsedSong& song,
                 TrackEvents& out, ParseResult& res) {
    Reader r{data, data + len};
    u32 tick = 0;
    u8 running = 0;
    u32 seq = 0;
    // Open notes per (channel << 7 | note) for unterminated-note cleanup.
    u8 open_notes[16 * 128] = {};

    bool corrupted = false;
    auto salvage = [&](const char* why) {
        res.warnings.push_back("track " + std::to_string(track_idx) + ": " + why +
                               "; salvaged events up to tick " + std::to_string(tick));
        corrupted = true;
    };

    auto push = [&](u8 status, u8 d1, u8 d2) {
        MidiEvent e;
        e.tick = tick;
        e.status = status;
        e.d1 = d1;
        e.d2 = d2;
        e.track = track_idx;
        e.seq = seq++;
        out.events.push_back(e);
    };

    while (!r.eof() && !corrupted) {
        u32 delta;
        if (!r.vlq(delta)) { salvage("bad delta time"); break; }
        tick += delta;
        if (r.eof()) { salvage("truncated event"); break; }

        u8 status = *r.p;
        if (status < 0x80) {
            if (running == 0) { salvage("data byte with no running status"); break; }
            status = running;
        } else {
            r.p++;
            if (status < 0xF0) running = status;
        }

        if (status == 0xFF) {
            running = 0;  // meta cancels running status
            if (r.eof()) { salvage("truncated meta"); break; }
            const u8 type = r.byte();
            u32 mlen;
            if (!r.vlq(mlen) || r.remaining() < mlen) { salvage("truncated meta"); break; }
            const u8* body = r.p;
            r.p += mlen;
            if (type == 0x2F) {  // end of track
                if (tick > song.end_tick) song.end_tick = tick;
                break;
            } else if (type == 0x51 && mlen >= 3) {
                const u32 us = (static_cast<u32>(body[0]) << 16) |
                               (static_cast<u32>(body[1]) << 8) | body[2];
                if (smpte)
                    res.warnings.push_back("track " + std::to_string(track_idx) +
                                           ": tempo event ignored (SMPTE division)");
                else
                    out.tempos.emplace_back(tick, us);
            } else if (type == 0x58 && mlen >= 2) {
                push(MidiEvent::META_TIME_SIG, body[0], body[1]);
            } else if (type == 0x06) {  // marker
                const std::string name(reinterpret_cast<const char*>(body), mlen);
                if (name == "loopStart") {
                    song.has_loop_start = true;
                    song.loop_start_tick = tick;
                } else if (name == "loopEnd") {
                    song.has_loop_end = true;
                    song.loop_end_tick = tick;
                }
            }
            continue;
        }
        if (status == 0xF0 || status == 0xF7) {  // SysEx: skip, cancels running
            running = 0;
            u32 slen;
            if (!r.vlq(slen) || r.remaining() < slen) { salvage("truncated sysex"); break; }
            r.p += slen;
            continue;
        }
        if (status >= 0xF0) { salvage("unexpected system status in track"); break; }

        const u8 kind = status & 0xF0;
        const int data_bytes = (kind == 0xC0 || kind == 0xD0) ? 1 : 2;
        if (r.remaining() < static_cast<size_t>(data_bytes)) { salvage("truncated event"); break; }
        const u8 d1 = r.byte() & 0x7F;
        const u8 d2 = data_bytes == 2 ? (r.byte() & 0x7F) : 0;
        const u8 ch = status & 0x0F;

        switch (kind) {
            case 0x90:
                if (d2 == 0) {
                    // Note-on velocity 0 -> note-off, normalized here and
                    // nowhere else (Appendix E).
                    push(0x80 | ch, d1, 0);
                    if (open_notes[(ch << 7) | d1]) --open_notes[(ch << 7) | d1];
                } else {
                    push(status, d1, d2);
                    ++open_notes[(ch << 7) | d1];
                }
                break;
            case 0x80:
                push(status, d1, d2);
                if (open_notes[(ch << 7) | d1]) --open_notes[(ch << 7) | d1];
                break;
            case 0xB0:
                if (d1 == 111 && !song.has_loop_start) {
                    // CC111: the de-facto RPG Maker loop-start convention.
                    song.has_loop_start = true;
                    song.loop_start_tick = tick;
                } else {
                    push(status, d1, d2);
                }
                break;
            case 0xA0:
                push(status, d1, d2);  // per-note aftertouch (mod source)
                break;
            default:
                push(status, d1, d2);
                break;
        }
    }

    if (tick > song.end_tick) song.end_tick = tick;

    // Unterminated notes: synthetic note-off at track end + warning.
    int unterminated = 0;
    for (int ch = 0; ch < 16; ++ch)
        for (int n = 0; n < 128; ++n)
            while (open_notes[(ch << 7) | n]) {
                --open_notes[(ch << 7) | n];
                MidiEvent e;
                e.tick = tick;
                e.status = static_cast<u8>(0x80 | ch);
                e.d1 = static_cast<u8>(n);
                e.track = track_idx;
                e.seq = seq++;
                out.events.push_back(e);
                ++unterminated;
            }
    if (unterminated)
        res.warnings.push_back("track " + std::to_string(track_idx) + ": " +
                               std::to_string(unterminated) +
                               " unterminated note(s), closed at track end");
}

}  // namespace

ParseResult smf_parse(const u8* data, size_t len, ParsedSong& out) {
    ParseResult res;
    out = ParsedSong{};
    Reader r{data, data + len};

    // Header chunk.
    if (r.remaining() < 14 || std::memcmp(r.p, "MThd", 4) != 0) {
        res.errors.push_back("not an SMF file (missing MThd)");
        return res;
    }
    r.p += 4;
    const u32 hlen = r.be32();
    if (hlen < 6 || r.remaining() < hlen) {
        res.errors.push_back("bad MThd length");
        return res;
    }
    const u8* header_end = r.p + hlen;
    const u32 format = r.be16();
    const u32 ntrks = r.be16();
    const u32 division_raw = r.be16();
    r.p = header_end;  // skip any MThd extension bytes (spec: ignore)

    bool smpte = false;
    u32 division = division_raw;
    if (division_raw & 0x8000) {
        // SMPTE: ticks per second = fps * ticks-per-frame; emulated as PPQ
        // with a fixed 1s "quarter".
        smpte = true;
        const int fps = 256 - static_cast<int>(division_raw >> 8);  // -(signed high byte)
        const int tpf = division_raw & 0xFF;
        division = static_cast<u32>(fps * tpf);
        res.warnings.push_back("SMPTE division converted (" + std::to_string(fps) + " fps x " +
                               std::to_string(tpf) + ")");
    }
    if (division == 0) {
        res.warnings.push_back("division 0, using 480 PPQ");
        division = 480;
    }
    out.division = division;

    if (format > 2) res.warnings.push_back("unknown SMF format, treating as format 1");
    u32 tracks_to_read = ntrks;
    if (format == 2) {
        res.warnings.push_back("SMF format 2: loading track 0 only");
        tracks_to_read = 1;
    }

    // Track chunks. Unknown chunk types are skipped (spec-required).
    std::vector<TrackEvents> tracks;
    u16 track_idx = 0;
    while (!r.eof() && track_idx < tracks_to_read) {
        if (r.remaining() < 8) {
            res.warnings.push_back("trailing bytes after last chunk");
            break;
        }
        char kind[5] = {};
        std::memcpy(kind, r.p, 4);
        r.p += 4;
        u32 clen = r.be32();
        if (clen > r.remaining()) {
            // Never trust chunk lengths past the actual buffer.
            res.warnings.push_back(std::string("chunk '") + kind + "' truncated, salvaging");
            clen = static_cast<u32>(r.remaining());
        }
        if (std::memcmp(kind, "MTrk", 4) == 0) {
            tracks.emplace_back();
            parse_track(r.p, clen, track_idx, smpte, out, tracks.back(), res);
            ++track_idx;
        } else {
            res.warnings.push_back(std::string("skipping unknown chunk '") + kind + "'");
        }
        r.p += clen;
    }
    if (track_idx < ntrks && format != 2)
        res.warnings.push_back("expected " + std::to_string(ntrks) + " tracks, found " +
                               std::to_string(track_idx));

    // Tempo events from all tracks, in tick order.
    std::vector<std::pair<u32, u32>> tempos;
    for (auto& t : tracks) tempos.insert(tempos.end(), t.tempos.begin(), t.tempos.end());
    std::stable_sort(tempos.begin(), tempos.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    if (smpte) tempos.assign(1, {0u, 1000000u});
    out.tempo.build(division, tempos);

    // Merge: explicit (tick, track, seq) ordering -- events at the same tick
    // must keep deterministic file order (program change before note-on).
    size_t total = 0;
    for (auto& t : tracks) total += t.events.size();
    out.events.reserve(total + 1);
    for (auto& t : tracks)
        out.events.insert(out.events.end(), t.events.begin(), t.events.end());
    std::sort(out.events.begin(), out.events.end(), [](const MidiEvent& a, const MidiEvent& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        if (a.track != b.track) return a.track < b.track;
        return a.seq < b.seq;
    });

    // Bake absolute sample positions through the one canonical conversion.
    for (auto& e : out.events) e.sample = out.tempo.tick_to_sample(e.tick);
    out.end_sample = out.tempo.tick_to_sample(out.end_tick);

    // Loop defaults: whole song.
    if (!out.has_loop_start) out.loop_start_tick = 0;
    if (!out.has_loop_end || out.loop_end_tick <= out.loop_start_tick) {
        out.has_loop_end = false;
        out.loop_end_tick = out.end_tick;
    }

    if (out.events.empty() && res.warnings.empty() && tracks.empty()) {
        res.errors.push_back("no tracks found");
        return res;
    }
    res.ok = true;
    return res;
}

}  // namespace msynth
