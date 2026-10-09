//
// SMF parser: running status, VLQ, merge order, salvage, loop conventions,
// tempo map math (plan §9, Appendix E).
//
#include "test_framework.h"

#include "midi/smf_parser.h"
#include "smf_builder.h"

using namespace msynth;
using testfw::TrackBuilder;
using testfw::build_smf;

TEST(smf_format0_basic) {
    TrackBuilder t;
    t.program(0, 0, 5).note_on(0, 0, 60, 100).note_off(480, 0, 60).end();
    ParsedSong song;
    auto bytes = build_smf(0, 480, {t});
    ParseResult r = smf_parse(bytes.data(), bytes.size(), song);
    CHECK(r.ok);
    CHECK_EQ(song.events.size(), 3);
    CHECK_EQ(song.events[0].kind(), 0xC0);
    CHECK_EQ(song.events[1].kind(), 0x90);
    CHECK_EQ(song.events[2].kind(), 0x80);
    CHECK_EQ(song.events[2].tick, 480);
    // 480 ticks at default 120 BPM = one quarter = 0.5s = 24000 samples.
    CHECK_EQ(song.events[2].sample, 24000);
}

TEST(smf_running_status) {
    // Two note-ons sharing one status byte.
    TrackBuilder t;
    t.raw({0x00, 0x90, 60, 100});
    t.raw({0x10, 62, 101});  // running status: delta 0x10, data only
    t.raw({0x10, 60, 0});    // running status note-on vel 0 = note off
    t.raw({0x00, 62, 0});
    t.end();
    ParsedSong song;
    auto bytes = build_smf(0, 480, {t});
    ParseResult r = smf_parse(bytes.data(), bytes.size(), song);
    CHECK(r.ok);
    CHECK_EQ(song.events.size(), 4);
    CHECK_EQ(song.events[1].d1, 62);
    CHECK_EQ(song.events[1].tick, 0x10);
    // Velocity-0 note-on normalized to a real note-off during parsing.
    CHECK_EQ(song.events[2].kind(), 0x80);
    CHECK_EQ(song.events[3].kind(), 0x80);
}

TEST(smf_meta_cancels_running_status) {
    TrackBuilder t;
    t.raw({0x00, 0x90, 60, 100});
    t.tempo(0, 400000);        // meta cancels running status...
    t.raw({0x00, 62, 100});    // ...so this data byte is corruption
    t.end();
    ParsedSong song;
    auto bytes = build_smf(0, 480, {t});
    ParseResult r = smf_parse(bytes.data(), bytes.size(), song);
    CHECK(r.ok);  // salvaged
    CHECK(!r.warnings.empty());
    CHECK_EQ(song.events.size(), 2);  // note-on + synthetic close
}

TEST(smf_format1_merge_order) {
    // Program change on track 0 and note-on on track 1 at the same tick:
    // track order must be preserved (patch applies before the note).
    TrackBuilder t0, t1;
    t0.tempo(0, 500000).program(100, 0, 7).end();
    t1.note_on(100, 0, 64, 90).note_off(50, 0, 64).end();
    ParsedSong song;
    auto bytes = build_smf(1, 480, {t0, t1});
    ParseResult r = smf_parse(bytes.data(), bytes.size(), song);
    CHECK(r.ok);
    CHECK_EQ(song.events.size(), 3);
    CHECK_EQ(song.events[0].kind(), 0xC0);  // program first at tick 100
    CHECK_EQ(song.events[1].kind(), 0x90);
}

TEST(smf_tempo_map_anchors) {
    // 480 PPQ. 120 BPM for 480 ticks, then 60 BPM.
    TrackBuilder t;
    t.tempo(0, 500000);
    t.tempo(480, 1000000);
    t.note_on(0, 0, 60, 100);   // tick 480 (delta after tempo at 480)
    t.note_off(480, 0, 60);     // tick 960
    t.end();
    ParsedSong song;
    auto bytes = build_smf(0, 480, {t});
    ParseResult r = smf_parse(bytes.data(), bytes.size(), song);
    CHECK(r.ok);
    CHECK_EQ(song.tempo.tick_to_sample(0), 0);
    CHECK_EQ(song.tempo.tick_to_sample(480), 24000);   // 0.5s at 120 BPM
    CHECK_EQ(song.tempo.tick_to_sample(960), 72000);   // + 1s at 60 BPM
    CHECK_EQ(song.tempo.tick_to_sample(240), 12000);   // mid-segment
    // Inverse.
    CHECK_EQ(song.tempo.sample_to_tick(24000), 480);
    CHECK_EQ(song.tempo.sample_to_tick(72000), 960);
    CHECK_EQ(song.tempo.sample_to_tick(48000), 720);
}

TEST(smf_unterminated_notes_closed) {
    TrackBuilder t;
    t.note_on(0, 0, 60, 100).note_on(10, 0, 64, 100).note_off(10, 0, 60).end(100);
    ParsedSong song;
    auto bytes = build_smf(0, 480, {t});
    ParseResult r = smf_parse(bytes.data(), bytes.size(), song);
    CHECK(r.ok);
    bool warned = false;
    for (auto& w : r.warnings)
        if (w.find("unterminated") != std::string::npos) warned = true;
    CHECK(warned);
    // A synthetic off for note 64 exists at track end.
    bool closed = false;
    for (auto& e : song.events)
        if (e.kind() == 0x80 && e.d1 == 64) closed = true;
    CHECK(closed);
}

TEST(smf_vlq_five_bytes_is_corruption) {
    TrackBuilder t;
    t.raw({0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x90, 60, 100});  // 5-byte VLQ
    t.end();
    ParsedSong song;
    auto bytes = build_smf(0, 480, {t});
    ParseResult r = smf_parse(bytes.data(), bytes.size(), song);
    CHECK(r.ok);  // salvage: empty track, warning
    CHECK(!r.warnings.empty());
}

TEST(smf_loop_markers) {
    TrackBuilder t;
    t.marker(0, "intro");
    t.marker(480, "loopStart");
    t.note_on(0, 0, 60, 100).note_off(480, 0, 60);
    t.marker(0, "loopEnd");
    t.end();
    ParsedSong song;
    auto bytes = build_smf(0, 480, {t});
    ParseResult r = smf_parse(bytes.data(), bytes.size(), song);
    CHECK(r.ok);
    CHECK(song.has_loop_start);
    CHECK(song.has_loop_end);
    CHECK_EQ(song.loop_start_tick, 480);
    CHECK_EQ(song.loop_end_tick, 960);
}

TEST(smf_cc111_loop_convention) {
    TrackBuilder t;
    t.note_on(0, 0, 60, 100);
    t.cc(240, 0, 111, 0);  // RPG Maker loop point
    t.note_off(240, 0, 60);
    t.end();
    ParsedSong song;
    auto bytes = build_smf(0, 480, {t});
    ParseResult r = smf_parse(bytes.data(), bytes.size(), song);
    CHECK(r.ok);
    CHECK(song.has_loop_start);
    CHECK_EQ(song.loop_start_tick, 240);
    // The CC111 itself is not forwarded as a controller event.
    for (auto& e : song.events) CHECK(!(e.kind() == 0xB0 && e.d1 == 111));
}

TEST(smf_truncated_file_salvages) {
    TrackBuilder t;
    t.note_on(0, 0, 60, 100).note_off(480, 0, 60).end();
    auto bytes = build_smf(0, 480, {t});
    bytes.resize(bytes.size() - 5);  // chop mid-track
    ParsedSong song;
    ParseResult r = smf_parse(bytes.data(), bytes.size(), song);
    CHECK(r.ok);
    CHECK(!r.warnings.empty());
    CHECK(song.events.size() >= 1);
}

TEST(smf_not_a_midi_file) {
    const u8 garbage[] = "this is not midi at all, sorry";
    ParsedSong song;
    ParseResult r = smf_parse(garbage, sizeof(garbage), song);
    CHECK(!r.ok);
    CHECK(!r.errors.empty());
    CHECK_EQ(song.events.size(), 0);
}

TEST(smf_unknown_chunk_skipped) {
    TrackBuilder t;
    t.note_on(0, 0, 60, 100).note_off(10, 0, 60).end();
    auto bytes = build_smf(0, 480, {t});
    // Splice an unknown chunk between header and track.
    std::vector<u8> with_junk(bytes.begin(), bytes.begin() + 14);
    const u8 junk[] = {'X', 'F', 'h', 'd', 0, 0, 0, 2, 0xAB, 0xCD};
    with_junk.insert(with_junk.end(), junk, junk + sizeof(junk));
    with_junk.insert(with_junk.end(), bytes.begin() + 14, bytes.end());
    ParsedSong song;
    ParseResult r = smf_parse(with_junk.data(), with_junk.size(), song);
    CHECK(r.ok);
    CHECK_EQ(song.events.size(), 2);
}

TEST(smf_format2_loads_first_track) {
    TrackBuilder t0, t1;
    t0.note_on(0, 0, 60, 100).note_off(10, 0, 60).end();
    t1.note_on(0, 0, 70, 100).note_off(10, 0, 70).end();
    ParsedSong song;
    auto bytes = build_smf(2, 480, {t0, t1});
    ParseResult r = smf_parse(bytes.data(), bytes.size(), song);
    CHECK(r.ok);
    CHECK(!r.warnings.empty());
    for (auto& e : song.events) CHECK(e.d1 != 70);
}

TEST(smf_smpte_division) {
    // 25 fps x 40 tpf = 1000 ticks/sec.
    TrackBuilder t;
    t.note_on(0, 0, 60, 100).note_off(1000, 0, 60).end();
    ParsedSong song;
    const u32 division = ((256 - 25) << 8) | 40;
    auto bytes = build_smf(0, division, {t});
    ParseResult r = smf_parse(bytes.data(), bytes.size(), song);
    CHECK(r.ok);
    // 1000 ticks = 1 second = 48000 samples.
    CHECK_EQ(song.events[1].sample, 48000);
}

TEST(smf_time_signature_events_kept) {
    TrackBuilder t;
    t.time_sig(0, 3, 2);  // 3/4
    t.note_on(0, 0, 60, 100).note_off(10, 0, 60).end();
    ParsedSong song;
    auto bytes = build_smf(0, 480, {t});
    ParseResult r = smf_parse(bytes.data(), bytes.size(), song);
    CHECK(r.ok);
    CHECK_EQ(song.events[0].status, MidiEvent::META_TIME_SIG);
    CHECK_EQ(song.events[0].d1, 3);
}

TEST(smf_poly_aftertouch_kept_as_events) {
    // 0xA0 is a mod source now (ModSource::PolyAftertouch): the parser keeps
    // the events and the sequencer dispatches them.
    TrackBuilder t;
    t.tempo(0, 500000);
    t.note_on(0, 0, 60, 100);
    t.ev(120, {0xA0, 60, 90});
    t.note_off(360, 0, 60);
    t.end();
    auto smf = build_smf(0, 480, {t});
    ParsedSong song;
    ParseResult r = smf_parse(smf.data(), smf.size(), song);
    CHECK(r.ok);
    bool found = false;
    for (const auto& e : song.events)
        if (e.kind() == 0xA0 && e.d1 == 60 && e.d2 == 90) found = true;
    CHECK(found);
}
