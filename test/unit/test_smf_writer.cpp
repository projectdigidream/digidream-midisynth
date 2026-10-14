//
// SMF writer: smf_parse(smf_write(song)) must reproduce the song exactly --
// events (incl. baked sample positions), tempo segments, loops, end.
//
#include "test_framework.h"

#include <fstream>

#include "midi/smf_writer.h"
#include "smf_builder.h"
#include "track/track.h"

using namespace msynth;
using testfw::TrackBuilder;
using testfw::build_smf;

namespace {

bool read_file(const char* path, std::vector<u8>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool songs_equal(const ParsedSong& a, const ParsedSong& b, std::string* why) {
    if (a.events.size() != b.events.size()) { *why = "event count"; return false; }
    for (size_t i = 0; i < a.events.size(); ++i) {
        const auto& x = a.events[i];
        const auto& y = b.events[i];
        if (x.tick != y.tick || x.status != y.status || x.d1 != y.d1 || x.d2 != y.d2 ||
            x.track != y.track || x.seq != y.seq || x.sample != y.sample) {
            *why = "event " + std::to_string(i) + " differs";
            return false;
        }
    }
    if (a.tempo.segments().size() != b.tempo.segments().size()) { *why = "tempo segs"; return false; }
    for (size_t i = 0; i < a.tempo.segments().size(); ++i) {
        const auto& s = a.tempo.segments()[i];
        const auto& t = b.tempo.segments()[i];
        if (s.start_tick != t.start_tick || s.start_sample != t.start_sample ||
            s.us_per_quarter != t.us_per_quarter) {
            *why = "tempo seg " + std::to_string(i);
            return false;
        }
    }
    if (a.end_tick != b.end_tick || a.end_sample != b.end_sample) { *why = "end"; return false; }
    if (a.has_loop_start != b.has_loop_start || a.loop_start_tick != b.loop_start_tick ||
        a.loop_end_tick != b.loop_end_tick) {
        *why = "loop";
        return false;
    }
    return true;
}

void check_roundtrip(const std::vector<u8>& smf, const char* label) {
    ParsedSong a;
    ParseResult pa = smf_parse(smf.data(), smf.size(), a);
    CHECK(pa.ok);
    std::vector<u8> written;
    LoadResult w = smf_write(a, written);
    CHECK(w.ok);
    ParsedSong b;
    ParseResult pb = smf_parse(written.data(), written.size(), b);
    CHECK(pb.ok);
    std::string why;
    const bool eq = songs_equal(a, b, &why);
    if (!eq) std::printf("  [%s] roundtrip mismatch: %s\n", label, why.c_str());
    CHECK(eq);
}

}  // namespace

TEST(smf_writer_roundtrips_golden_corpus) {
    for (const char* f : {"test/golden/melody.mid", "test/golden/drums.mid",
                          "test/golden/tempos.mid", "test/golden/poly.mid"}) {
        std::vector<u8> smf;
        if (!read_file(f, smf)) { CHECK(false); continue; }
        check_roundtrip(smf, f);
    }
}

TEST(smf_writer_roundtrips_multitrack_with_metas) {
    TrackBuilder t0, t1, t2;
    t0.tempo(0, 500000).time_sig(0, 3, 2).tempo(480, 250000).marker(0, "loopStart").end(1920);
    t1.program(0, 0, 48).note_on(0, 0, 60, 100).note_off(480, 0, 60)
      .cc(0, 0, 7, 90).bend(0, 0, 9000).note_on(0, 0, 62, 90).note_off(240, 0, 62).end();
    t2.ev(0, {0x99, 36, 100}).ev(120, {0x99, 42, 80}).marker(0, "loopEnd").end();
    check_roundtrip(build_smf(1, 480, {t0, t1, t2}), "multitrack");
}

TEST(smf_writer_roundtrips_cut_slice) {
    std::vector<u8> smf;
    CHECK(read_file("test/golden/tempos.mid", smf));
    ParsedSong parent, slice;
    smf_parse(smf.data(), smf.size(), parent);
    CHECK(cut_song(parent, 480, 1440, slice).ok);
    std::vector<u8> written;
    CHECK(smf_write(slice, written).ok);
    ParsedSong back;
    CHECK(smf_parse(written.data(), written.size(), back).ok);
    std::string why;
    const bool eq = songs_equal(slice, back, &why);
    if (!eq) std::printf("  [cut slice] roundtrip mismatch: %s\n", why.c_str());
    CHECK(eq);
}

TEST(smf_writer_normalizes_hand_built_song) {
    // A DAW-built song: out of order seq, note-on vel 0, an unterminated note.
    ParsedSong s;
    s.tempo.build(480, {{0, 500000}});
    auto ev = [&](u32 tick, u8 st, u8 d1, u8 d2, u16 track) {
        MidiEvent e;
        e.tick = tick; e.status = st; e.d1 = d1; e.d2 = d2; e.track = track;
        s.events.push_back(e);
    };
    ev(480, 0x90, 64, 100, 1);
    ev(0, 0xC0, 5, 0, 1);         // inserted before the tick-0 note: stable order kept
    ev(0, 0x90, 60, 100, 1);
    ev(240, 0x90, 60, 0, 1);      // vel-0 note-off
    s.end_tick = 960;
    normalize_seq(s);
    for (auto& e : s.events) e.sample = s.tempo.tick_to_sample(e.tick);
    s.end_sample = s.tempo.tick_to_sample(960);

    std::vector<u8> written;
    LoadResult w = smf_write(s, written);
    CHECK(w.ok);
    CHECK(!w.warnings.empty());  // vel-0 rewrite + unterminated note 64
    ParsedSong back;
    CHECK(smf_parse(written.data(), written.size(), back).ok);
    // Program change precedes the note at tick 0; vel-0 became a note-off; the
    // open note 64 got closed at the end.
    CHECK_EQ(back.events[0].kind(), 0xC0);
    CHECK_EQ(back.events[1].kind(), 0x90);
    bool off60 = false, off64_at_end = false;
    for (const auto& e : back.events) {
        if (e.kind() == 0x80 && e.d1 == 60 && e.tick == 240) off60 = true;
        if (e.kind() == 0x80 && e.d1 == 64 && e.tick == 960) off64_at_end = true;
    }
    CHECK(off60);
    CHECK(off64_at_end);
    CHECK_EQ(back.end_tick, 960);
    // Idempotent: writing the parsed result again reproduces it exactly.
    std::vector<u8> again;
    CHECK(smf_write(back, again).ok);
    ParsedSong back2;
    smf_parse(again.data(), again.size(), back2);
    std::string why;
    CHECK(songs_equal(back, back2, &why));
}

TEST(smf_writer_empty_song) {
    ParsedSong s;
    s.tempo.build(480, {});
    std::vector<u8> written;
    CHECK(smf_write(s, written).ok);
    ParsedSong back;
    CHECK(smf_parse(written.data(), written.size(), back).ok);
    CHECK_EQ(back.events.size(), 0);
}
