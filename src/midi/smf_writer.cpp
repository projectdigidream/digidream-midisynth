#include "smf_writer.h"

#include <algorithm>

namespace msynth {
namespace {

void vlq(std::vector<u8>& out, u32 v) {
    u8 buf[4];
    int n = 0;
    buf[n++] = static_cast<u8>(v & 0x7F);
    while (v >>= 7) buf[n++] = static_cast<u8>(0x80 | (v & 0x7F));
    while (n) out.push_back(buf[--n]);
}

void be32(std::vector<u8>& out, u32 v) {
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<u8>(v >> (8 * i)));
}
void be16(std::vector<u8>& out, u32 v) {
    out.push_back(static_cast<u8>(v >> 8));
    out.push_back(static_cast<u8>(v));
}

// One serialized event awaiting delta encoding. `order` breaks ties within a
// tick: metas (tempo/markers) go first (-1), channel events by their seq.
struct Pending {
    u32 tick;
    i64 order;
    std::vector<u8> bytes;
};

}  // namespace

void normalize_seq(ParsedSong& song) {
    std::stable_sort(song.events.begin(), song.events.end(),
                     [](const MidiEvent& a, const MidiEvent& b) {
                         if (a.tick != b.tick) return a.tick < b.tick;
                         if (a.track != b.track) return a.track < b.track;
                         return a.seq < b.seq;
                     });
    u16 max_track = 0;
    for (const auto& e : song.events) max_track = std::max(max_track, e.track);
    std::vector<u32> counters(static_cast<size_t>(max_track) + 1, 0);
    for (auto& e : song.events) e.seq = counters[e.track]++;
}

LoadResult smf_write(const ParsedSong& song, std::vector<u8>& out) {
    LoadResult rep;
    out.clear();

    u16 max_track = 0;
    for (const auto& e : song.events) max_track = std::max(max_track, e.track);
    const int ntracks = static_cast<int>(max_track) + 1;
    std::vector<std::vector<Pending>> tracks(ntracks);

    // Track 0 metas: every tempo segment, loop markers.
    for (const auto& seg : song.tempo.segments()) {
        u32 us = seg.us_per_quarter;
        if (us > 0xFFFFFFu) {
            rep.warn("tempo " + std::to_string(us) + " us/quarter exceeds 24 bits, clamped");
            us = 0xFFFFFFu;
        }
        Pending p{seg.start_tick, -3, {0xFF, 0x51, 0x03, static_cast<u8>(us >> 16),
                                       static_cast<u8>(us >> 8), static_cast<u8>(us)}};
        tracks[0].push_back(std::move(p));
    }
    if (song.has_loop_start) {
        static const char* name = "loopStart";
        Pending p{song.loop_start_tick, -2, {0xFF, 0x06, 9}};
        p.bytes.insert(p.bytes.end(), name, name + 9);
        tracks[0].push_back(std::move(p));
    }
    if (song.has_loop_end && song.loop_end_tick > song.loop_start_tick) {
        static const char* name = "loopEnd";
        Pending p{song.loop_end_tick, -2, {0xFF, 0x06, 7}};
        p.bytes.insert(p.bytes.end(), name, name + 7);
        tracks[0].push_back(std::move(p));
    }

    // Channel + time-signature events, with the parser's unterminated-note
    // bookkeeping so a re-parse adds nothing.
    std::vector<std::vector<int>> open(ntracks, std::vector<int>(16 * 128, 0));
    int rewritten_vel0 = 0;
    for (const auto& e : song.events) {
        auto& tr = tracks[e.track];
        if (e.status == MidiEvent::META_TIME_SIG) {
            tr.push_back({e.tick, e.seq, {0xFF, 0x58, 0x04, e.d1, e.d2, 0x18, 0x08}});
            continue;
        }
        if (!e.is_channel()) continue;  // other pseudo-metas are position data only
        const u8 kind = e.kind();
        const u8 ch = e.channel();
        u8 status = e.status;
        u8 d1 = e.d1 & 0x7F, d2 = e.d2 & 0x7F;
        if (kind == 0x90) {
            if (d2 == 0) {  // parser would normalize this to a note-off
                status = static_cast<u8>(0x80 | ch);
                ++rewritten_vel0;
                if (open[e.track][(ch << 7) | d1]) open[e.track][(ch << 7) | d1]--;
            } else {
                open[e.track][(ch << 7) | d1]++;
            }
        } else if (kind == 0x80) {
            if (open[e.track][(ch << 7) | d1]) open[e.track][(ch << 7) | d1]--;
        }
        Pending p{e.tick, e.seq, {status, d1}};
        if (kind != 0xC0 && kind != 0xD0) p.bytes.push_back(d2);
        tr.push_back(std::move(p));
    }
    if (rewritten_vel0)
        rep.warn(std::to_string(rewritten_vel0) + " note-on(s) with velocity 0 written as note-off");

    // Serialize each track.
    std::vector<std::vector<u8>> chunks(ntracks);
    for (int t = 0; t < ntracks; ++t) {
        auto& tr = tracks[t];
        std::stable_sort(tr.begin(), tr.end(), [](const Pending& a, const Pending& b) {
            if (a.tick != b.tick) return a.tick < b.tick;
            return a.order < b.order;
        });
        u32 last_tick = 0;
        for (const auto& p : tr) last_tick = std::max(last_tick, p.tick);
        // Every track ends at the song end: the parser takes end_tick as the
        // max EOT, and unterminated notes must close at the SONG end, not at
        // a track's last event.
        const u32 eot_tick = std::max(song.end_tick, last_tick);

        // Close notes still open at the track end, in the parser's order.
        int closed = 0;
        for (int ch = 0; ch < 16; ++ch)
            for (int n = 0; n < 128; ++n)
                while (open[t][(ch << 7) | n] > 0) {
                    open[t][(ch << 7) | n]--;
                    tr.push_back({eot_tick, static_cast<i64>(1) << 40,
                                  {static_cast<u8>(0x80 | ch), static_cast<u8>(n), 0}});
                    ++closed;
                }
        if (closed)
            rep.warn("track " + std::to_string(t) + ": " + std::to_string(closed) +
                     " unterminated note(s) closed at track end");
        std::stable_sort(tr.begin(), tr.end(), [](const Pending& a, const Pending& b) {
            if (a.tick != b.tick) return a.tick < b.tick;
            return a.order < b.order;
        });

        auto& c = chunks[t];
        u32 tick = 0;
        for (const auto& p : tr) {
            const u32 delta = p.tick - tick;
            if (delta > 0x0FFFFFFFu) {
                rep.error("tick delta exceeds the 4-byte VLQ limit");
                return rep;
            }
            vlq(c, delta);
            c.insert(c.end(), p.bytes.begin(), p.bytes.end());
            tick = p.tick;
        }
        vlq(c, eot_tick - tick);
        c.insert(c.end(), {0xFF, 0x2F, 0x00});
    }

    // File.
    out.insert(out.end(), {'M', 'T', 'h', 'd'});
    be32(out, 6);
    be16(out, ntracks > 1 ? 1 : 0);
    be16(out, static_cast<u32>(ntracks));
    u32 division = song.tempo.division();
    if (division == 0 || division > 0x7FFF) {
        rep.warn("division out of range, writing 480 PPQ");
        division = 480;
    }
    be16(out, division);
    for (const auto& c : chunks) {
        out.insert(out.end(), {'M', 'T', 'r', 'k'});
        be32(out, static_cast<u32>(c.size()));
        out.insert(out.end(), c.begin(), c.end());
    }
    return rep;
}

}  // namespace msynth
