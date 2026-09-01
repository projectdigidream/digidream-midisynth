//
// Baked .mstk tracks: round-trip fidelity, tree-shaking, integrity
// (trusted-or-rejected), and the load-bearing property — a baked track
// renders bit-identically to its sources.
//
#include "test_framework.h"

#include <fstream>

#include "bindings/c_api.h"
#include "engine_helpers.h"
#include "output/audio_output_offline.h"
#include "smf_builder.h"
#include "track/track.h"

using namespace msynth;
using testfw::TrackBuilder;
using testfw::build_smf;

namespace {

std::string read_text(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::vector<u8> test_song() {
    TrackBuilder t;
    t.tempo(0, 500000);
    t.program(0, 0, 48);
    t.cc(0, 0, 91, 70);
    t.note_on(0, 0, 60, 100).note_off(480, 0, 60);
    t.ev(0, {0x99, 36, 110});   // kick
    t.ev(120, {0x99, 42, 80});  // closed hat
    t.tempo(120, 250000);
    t.note_on(0, 0, 67, 90).note_off(480, 0, 67);
    t.marker(0, "loopEnd");
    t.end();
    return build_smf(0, 480, {t});
}

const std::string& bank_json() {
    static const std::string s = read_text("content/patches/gm_bank.json");
    return s;
}
const std::string& kit_json() {
    static const std::string s = read_text("content/drums/gm_kit.json");
    return s;
}

std::vector<u8> bake_test_track(BakeOptions opts = {}, LoadResult* rep_out = nullptr) {
    auto smf = test_song();
    std::vector<u8> out;
    LoadResult r = bake_track(smf.data(), smf.size(), bank_json().data(), bank_json().size(),
                              kit_json().data(), kit_json().size(), opts, out);
    if (rep_out) *rep_out = r;
    return out;
}

}  // namespace

TEST(track_bake_load_roundtrip) {
    LoadResult rep;
    auto bytes = bake_test_track({}, &rep);
    CHECK(rep.ok);
    CHECK(!bytes.empty());
    CHECK(is_track(bytes.data(), bytes.size()));

    TrackData track;
    LoadResult lr = load_track(bytes.data(), bytes.size(), track);
    CHECK(lr.ok);

    // Song survives exactly: parse the source directly and compare.
    auto smf = test_song();
    ParsedSong direct;
    smf_parse(smf.data(), smf.size(), direct);
    CHECK_EQ(track.song.events.size(), direct.events.size());
    for (size_t i = 0; i < direct.events.size(); ++i) {
        CHECK_EQ(track.song.events[i].sample, direct.events[i].sample);
        CHECK_EQ(track.song.events[i].status, direct.events[i].status);
        CHECK_EQ(track.song.events[i].d1, direct.events[i].d1);
    }
    CHECK_EQ(track.song.end_sample, direct.end_sample);
    CHECK_EQ(track.song.tempo.segments().size(), direct.tempo.segments().size());
    CHECK(track.song.has_loop_end);
    CHECK_EQ(track.song.loop_end_tick, direct.loop_end_tick);

    // Tree-shaking: exactly program 48 + drums 36/42 survive.
    CHECK(track.bank_json.find("\"48\"") != std::string::npos);
    CHECK(track.bank_json.find("\"0\":") == std::string::npos);
    CHECK(track.bank_json.find("fallback") != std::string::npos);
    CHECK(track.kit_json.find("\"36\"") != std::string::npos);
    CHECK(track.kit_json.find("\"42\"") != std::string::npos);
    CHECK(track.kit_json.find("\"49\"") == std::string::npos);

    // Meta bitmaps agree.
    CHECK(track.meta.programs_used[48 >> 3] & (1 << (48 & 7)));
    CHECK(track.meta.drum_notes_used[36 >> 3] & (1 << (36 & 7)));
    CHECK(track.meta.duration_ms > 900);
}

TEST(track_renders_bit_identically_to_sources) {
    auto smf = test_song();
    const int FRAMES = SAMPLE_RATE * 2;

    // Direct path: full bank + kit.
    WaveTableCache cache;
    PatchBank bank;
    DrumKit kit;
    LoadResult br = load_patch_bank(bank_json().data(), bank_json().size(), cache, bank);
    LoadResult kr = load_drum_kit(kit_json().data(), kit_json().size(), cache, kit);
    CHECK(br.ok);
    CHECK(kr.ok);
    OfflineRenderer direct;
    direct.engine().set_content(&bank, &kit);
    direct.director().load_song(smf.data(), smf.size());
    direct.director().play();
    std::vector<i16> a;
    direct.render(a, FRAMES);

    // Baked path: tree-shaken content from the .mstk.
    auto bytes = bake_test_track();
    TrackData track;
    CHECK(load_track(bytes.data(), bytes.size(), track).ok);
    WaveTableCache cache2;
    PatchBank bank2;
    DrumKit kit2;
    CHECK(load_patch_bank(track.bank_json.data(), track.bank_json.size(), cache2, bank2).ok);
    CHECK(load_drum_kit(track.kit_json.data(), track.kit_json.size(), cache2, kit2).ok);
    OfflineRenderer baked;
    baked.engine().set_content(&bank2, &kit2);
    push_song_config(baked.engine(), track.fx);
    baked.director().active().load_parsed(std::move(track.song));
    baked.director().play();
    std::vector<i16> b;
    baked.render(b, FRAMES);

    CHECK(a == b);
    CHECK(testfw::peak_abs(a) > 100);
}

TEST(track_corruption_rejected_outright) {
    auto bytes = bake_test_track();
    TrackData track;

    // Flip one payload byte: integrity hash must catch it.
    auto corrupt = bytes;
    corrupt[corrupt.size() / 2] ^= 0x40;
    LoadResult r1 = load_track(corrupt.data(), corrupt.size(), track);
    CHECK(!r1.ok);

    // Truncation.
    LoadResult r2 = load_track(bytes.data(), bytes.size() / 2, track);
    CHECK(!r2.ok);

    // Wrong magic.
    auto junk = bytes;
    junk[0] = 'X';
    LoadResult r3 = load_track(junk.data(), junk.size(), track);
    CHECK(!r3.ok);

    // Future version.
    auto future = bytes;
    future[4] = 99;
    LoadResult r4 = load_track(future.data(), future.size(), track);
    CHECK(!r4.ok);

    // The pristine file still loads (no accidental mutation above).
    CHECK(load_track(bytes.data(), bytes.size(), track).ok);
}

TEST(track_bake_warns_on_silent_drums_and_fallback_programs) {
    // Pick a drum note that is genuinely unmapped in the CURRENT kit (the
    // kit is living content -- never hardcode which notes exist).
    int silent_note = -1;
    for (int n = 27; n < 88 && silent_note < 0; ++n)
        if (kit_json().find("\"" + std::to_string(n) + "\"") == std::string::npos)
            silent_note = n;
    CHECK(silent_note >= 0);  // a fully-mapped 27..87 kit would need a new probe

    TrackBuilder t;
    t.tempo(0, 500000);
    t.program(0, 0, 113);                       // unauthored program -> fallback
    t.note_on(0, 0, 60, 100).note_off(240, 0, 60);
    t.ev(0, {0x99, silent_note, 100});          // unmapped drum -> silent
    t.end(240);
    auto smf = build_smf(0, 480, {t});
    std::vector<u8> out;
    LoadResult r = bake_track(smf.data(), smf.size(), bank_json().data(), bank_json().size(),
                              kit_json().data(), kit_json().size(), {}, out);
    CHECK(r.ok);
    bool warned_prog = false, warned_drum = false;
    for (const auto& w : r.warnings) {
        if (w.find("113") != std::string::npos) warned_prog = true;
        if (w.find(std::to_string(silent_note)) != std::string::npos &&
            w.find("SILENT") != std::string::npos)
            warned_drum = true;
    }
    CHECK(warned_prog);
    CHECK(warned_drum);
}

TEST(track_loop_override_baked_in) {
    BakeOptions opts;
    opts.loop_start_tick = 480;
    opts.loop_end_tick = 960;
    auto bytes = bake_test_track(opts);
    TrackData track;
    CHECK(load_track(bytes.data(), bytes.size(), track).ok);
    CHECK(track.song.has_loop_start);
    CHECK_EQ(track.song.loop_start_tick, 480);
    CHECK_EQ(track.song.loop_end_tick, 960);
}

TEST(track_bad_sources_fail_bake) {
    const u8 junk[] = "not midi";
    std::vector<u8> out;
    LoadResult r = bake_track(junk, sizeof(junk), bank_json().data(), bank_json().size(),
                              kit_json().data(), kit_json().size(), {}, out);
    CHECK(!r.ok);
    LoadResult r2 = bake_track(test_song().data(), test_song().size(), "{{{", 3,
                               kit_json().data(), kit_json().size(), {}, out);
    CHECK(!r2.ok);
}

TEST(track_c_api_load) {
    auto bytes = bake_test_track();
    msynth_ctx* s = msynth_create();
    char report[2048];
    CHECK_EQ(msynth_load_track(s, bytes.data(), bytes.size(), report, sizeof(report)), 0);
    msynth_play(s);
    i16 buf[512 * 2];
    i64 peak = 0;
    for (int i = 0; i < 40; ++i) {
        msynth_render(s, buf, 512);
        for (i16 v : buf) peak = std::max<i64>(peak, v < 0 ? -v : v);
    }
    CHECK(peak > 100);
    CHECK(msynth_duration_ms(s) > 900);
    // Corrupt track through the C API: clean error.
    bytes[bytes.size() / 2] ^= 1;
    CHECK(msynth_load_track(s, bytes.data(), bytes.size(), report, sizeof(report)) != 0);
    msynth_destroy(s);
}

// ---------------------------------------------------------------------------
// Cutting.

namespace {
std::vector<u8> cuttable_song() {
    // 8 beats: program+CC setup, a held pad crossing bar lines, melody,
    // bend, tempo change, drums.
    TrackBuilder t;
    t.tempo(0, 500000);
    t.time_sig(0, 4, 2);
    t.program(0, 0, 48);
    t.cc(0, 0, 7, 90);
    t.cc(0, 0, 91, 60);
    // RPN bend range = 12.
    t.cc(0, 0, 101, 0).cc(0, 0, 100, 0).cc(0, 0, 6, 12);
    t.bend(0, 0, 10000);
    t.note_on(0, 0, 48, 80);            // pad held across the cut point
    t.note_on(0, 0, 60, 100).note_off(480, 0, 60);
    t.note_on(0, 0, 62, 100);
    t.tempo(240, 250000);               // tempo change inside beat 2
    t.note_off(240, 0, 62);
    t.ev(0, {0x99, 36, 110});
    t.note_on(0, 0, 64, 100).note_off(480, 0, 64);
    t.note_off(0, 0, 48);               // pad ends at tick 1920
    t.note_on(0, 0, 65, 100).note_off(480, 0, 65);
    t.end();
    return build_smf(0, 480, {t});
}
}  // namespace

TEST(cut_song_chases_state_and_rebase) {
    auto smf = cuttable_song();
    ParsedSong parent;
    CHECK(smf_parse(smf.data(), smf.size(), parent).ok);
    ParsedSong slice;
    LoadResult r = cut_song(parent, 960, 1920, slice);  // beats 3..4
    CHECK(r.ok);

    // Chased state re-emitted at tick 0: program 48, CC7, CC91, RPN bend
    // range, live bend value.
    bool prog = false, vol = false, rev = false, rpn6 = false, bend = false;
    for (const auto& e : slice.events) {
        if (e.tick > 0) break;
        if (e.kind() == 0xC0 && e.d1 == 48) prog = true;
        if (e.kind() == 0xB0 && e.d1 == 7 && e.d2 == 90) vol = true;
        if (e.kind() == 0xB0 && e.d1 == 91 && e.d2 == 60) rev = true;
        if (e.kind() == 0xB0 && e.d1 == 6 && e.d2 == 12) rpn6 = true;
        if (e.kind() == 0xE0 && ((e.d2 << 7) | e.d1) == 10000) bend = true;
    }
    CHECK(prog);
    CHECK(vol);
    CHECK(rev);
    CHECK(rpn6);
    CHECK(bend);

    // Re-based timing. Parent ticks: drum at 960 (the cut start -> slice 0);
    // note 64 spans 960..1440 -> slice 0..480. The tempo change at parent
    // tick 720 (250000 us/q) is chased, so slice tick 480 = 0.25 s = 12000
    // samples -- the canonical conversion applied to the sliced tempo map.
    bool drum_ok = false, off64_ok = false;
    for (const auto& e : slice.events) {
        if (e.kind() == 0x90 && e.channel() == 9 && e.d1 == 36) {
            CHECK_EQ(e.tick, 0);
            CHECK_EQ(e.sample, 0);
            drum_ok = true;
        }
        if (e.kind() == 0x80 && e.d1 == 64) {
            CHECK_EQ(e.tick, 480);
            CHECK_EQ(e.sample, 12000);
            off64_ok = true;
        }
    }
    CHECK(drum_ok);
    CHECK(off64_ok);

    // The held pad's note-off (orphan) was dropped; note 64's off (in range)
    // kept; slice loops whole by default.
    int off48 = 0;
    for (const auto& e : slice.events)
        if (e.kind() == 0x80 && e.d1 == 48) ++off48;
    CHECK_EQ(off48, 0);
    CHECK_EQ(slice.end_tick, 960);
    CHECK_EQ(slice.loop_end_tick, 960);
}

TEST(cut_song_closes_notes_open_at_end) {
    auto smf = cuttable_song();
    ParsedSong parent;
    smf_parse(smf.data(), smf.size(), parent);
    // Cut ends at tick 1000 while the pad (48, on at 0..1440) and note 64
    // (960..1440) are held: both must close at the cut end.
    ParsedSong slice;
    LoadResult r = cut_song(parent, 0, 1000, slice);
    CHECK(r.ok);
    bool closed48 = false, closed64 = false;
    for (const auto& e : slice.events) {
        if (e.kind() == 0x80 && e.d1 == 48 && e.tick == 1000) closed48 = true;
        if (e.kind() == 0x80 && e.d1 == 64 && e.tick == 1000) closed64 = true;
    }
    CHECK(closed48);
    CHECK(closed64);
    bool warned = false;
    for (const auto& w : r.warnings)
        if (w.find("open at the cut end") != std::string::npos) warned = true;
    CHECK(warned);
}

TEST(cut_song_bad_ranges) {
    auto smf = cuttable_song();
    ParsedSong parent;
    smf_parse(smf.data(), smf.size(), parent);
    ParsedSong slice;
    CHECK(!cut_song(parent, 500, 500, slice).ok);
    CHECK(!cut_song(parent, 900, 100, slice).ok);
    CHECK(!cut_song(parent, 999999, 1000000, slice).ok);
    LoadResult r = cut_song(parent, 100, 999999, slice);  // end clamps
    CHECK(r.ok);
    CHECK(!r.warnings.empty());
}

TEST(cut_track_end_to_end_and_loops) {
    // Bake parent -> cut a slice -> load slice -> it plays, loops whole, and
    // its content hash matches the parent's (crossfade compatibility).
    auto smf = cuttable_song();
    std::vector<u8> parent_bytes;
    LoadResult br = bake_track(smf.data(), smf.size(), bank_json().data(), bank_json().size(),
                               kit_json().data(), kit_json().size(), {}, parent_bytes);
    CHECK(br.ok);
    TrackData parent;
    CHECK(load_track(parent_bytes.data(), parent_bytes.size(), parent).ok);

    CutOptions opts;
    opts.name = "battle_a";
    std::vector<u8> slice_bytes;
    LoadResult cr = cut_track(parent_bytes.data(), parent_bytes.size(), 960, 1920, opts,
                              slice_bytes);
    CHECK(cr.ok);
    CHECK(slice_bytes.size() < parent_bytes.size());

    TrackData slice;
    CHECK(load_track(slice_bytes.data(), slice_bytes.size(), slice).ok);
    CHECK(slice.meta.name == "battle_a");
    CHECK_EQ(std::memcmp(slice.meta.content_hash, parent.meta.content_hash, 32), 0);

    // Play it looped through the C API: audio flows well past 3x its length.
    msynth_ctx* s = msynth_create();
    char report[2048];
    CHECK_EQ(msynth_load_track(s, slice_bytes.data(), slice_bytes.size(), report,
                               sizeof(report)), 0);
    msynth_set_loop(s, 1);
    msynth_play(s);
    i16 buf[512 * 2];
    i64 late_energy = 0;
    const int total_blocks = 300;  // ~3.2s; slice is ~0.75s
    for (int i = 0; i < total_blocks; ++i) {
        msynth_render(s, buf, 512);
        if (i > total_blocks - 40)
            for (i16 v : buf) late_energy += v < 0 ? -v : v;
    }
    CHECK(msynth_playing(s) == 1);
    CHECK(late_energy > 1000);  // still making music on the 4th+ loop

    // Crossfade to a second slice cut from the same parent: no content
    // warning, audio continues.
    std::vector<u8> slice2;
    opts.name = "battle_b";
    CHECK(cut_track(parent_bytes.data(), parent_bytes.size(), 0, 960, opts, slice2).ok);
    CHECK_EQ(msynth_crossfade_track(s, slice2.data(), slice2.size(), 300, report,
                                    sizeof(report)), 0);
    CHECK(std::string(report).find("different") == std::string::npos);
    i64 xfade_energy = 0;
    for (int i = 0; i < 100; ++i) {
        msynth_render(s, buf, 512);
        for (i16 v : buf) xfade_energy += v < 0 ? -v : v;
    }
    CHECK(xfade_energy > 1000);
    msynth_destroy(s);
}

TEST(cut_slice_opens_with_correct_pitch) {
    // The chase must make a slice sound right from sample 0: cut after a
    // program change to a DIFFERENT instrument and verify the slice's first
    // note actually renders (fallback-vs-authored would both render; check
    // the bend chase instead: +2 semitones of a 440 note ~= 494 Hz).
    TrackBuilder t;
    t.tempo(0, 500000);
    t.program(0, 0, 0);
    t.bend(0, 0, 16383);  // max up, default range +/-2 => +2 semitones
    t.note_on(480, 0, 69, 127).note_off(1920, 0, 69);
    t.end();
    auto smf = build_smf(0, 480, {t});
    std::vector<u8> parent_bytes;
    CHECK(bake_track(smf.data(), smf.size(), bank_json().data(), bank_json().size(),
                     kit_json().data(), kit_json().size(), {}, parent_bytes).ok);
    std::vector<u8> slice_bytes;
    CHECK(cut_track(parent_bytes.data(), parent_bytes.size(), 480, 2400, {}, slice_bytes).ok);

    msynth_ctx* s = msynth_create();
    char report[512];
    CHECK_EQ(msynth_load_track(s, slice_bytes.data(), slice_bytes.size(), report,
                               sizeof(report)), 0);
    msynth_play(s);
    std::vector<i16> pcm(48000 * 2);
    for (int i = 0; i < 48000 / 512 * 512; i += 512)
        msynth_render(s, pcm.data() + i * 2, 512);
    int crossings = 0;
    for (size_t i = 2 + 4096 * 2; i < pcm.size(); i += 2)
        if (pcm[i - 2] < 0 && pcm[i] >= 0) ++crossings;
    // ~494 Hz over ~0.96s of counted window (bend chased in), not ~440.
    CHECK(crossings > 460);
    msynth_destroy(s);
}
