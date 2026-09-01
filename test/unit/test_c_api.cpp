//
// C API: lifecycle, report plumbing, and — the load-bearing check — PCM
// equality with the C++ OfflineRenderer path for the same inputs.
//
#include "test_framework.h"

#include <fstream>

#include "bindings/c_api.h"
#include "output/audio_output_offline.h"
#include "smf_builder.h"
#include "synth/patch_loader.h"

using namespace msynth;
using testfw::TrackBuilder;
using testfw::build_smf;

namespace {
std::vector<u8> two_note_song() {
    TrackBuilder t;
    t.tempo(0, 500000);
    t.cc(0, 0, 91, 80);  // reverb send
    t.note_on(0, 0, 60, 100).note_off(480, 0, 60);
    t.note_on(0, 0, 67, 90).note_off(480, 0, 67);
    t.end(240);
    return build_smf(0, 480, {t});
}
}  // namespace

TEST(c_api_lifecycle_and_reports) {
    msynth_ctx* s = msynth_create();
    CHECK(s != nullptr);
    char report[2048];

    // Bad JSON: error return + message in the report buffer.
    CHECK(msynth_load_bank_json(s, "{{{", 3, report, sizeof(report)) != 0);
    CHECK(std::string(report).find("error") != std::string::npos);

    // Bad SMF: error, and playing stays a no-op.
    const u8 junk[] = "nope";
    CHECK(msynth_load_song(s, junk, sizeof(junk), report, sizeof(report)) != 0);
    msynth_play(s);
    CHECK_EQ(msynth_playing(s), 0);

    // Good song, no explicit bank: builtin fallback still makes sound.
    auto smf = two_note_song();
    CHECK_EQ(msynth_load_song(s, smf.data(), smf.size(), report, sizeof(report)), 0);
    msynth_play(s);
    CHECK_EQ(msynth_playing(s), 1);
    i16 buf[512 * 2];
    i64 peak = 0;
    for (int i = 0; i < 20; ++i) {
        msynth_render(s, buf, 512);
        for (i16 v : buf) peak = std::max<i64>(peak, v < 0 ? -v : v);
    }
    CHECK(peak > 100);
    CHECK(msynth_active_voices(s) >= 1);
    CHECK(msynth_duration_ms(s) > 900);
    msynth_destroy(s);
}

TEST(c_api_matches_cpp_path_bit_for_bit) {
    auto smf = two_note_song();
    const int FRAMES = SAMPLE_RATE;  // 1s

    // C++ path.
    WaveTableCache cache;
    PatchBank bank;
    DrumKit kit;
    make_builtin_fallback(cache, bank.fallback());
    OfflineRenderer renderer;
    renderer.engine().set_content(&bank, &kit);
    renderer.director().load_song(smf.data(), smf.size());
    renderer.director().play();
    std::vector<i16> a;
    renderer.render(a, FRAMES);

    // C path.
    msynth_ctx* s = msynth_create();
    char report[256];
    msynth_load_song(s, smf.data(), smf.size(), report, sizeof(report));
    msynth_play(s);
    std::vector<i16> b(a.size());
    for (size_t done = 0; done + BUFFER_FRAMES * 2 <= b.size(); done += BUFFER_FRAMES * 2)
        msynth_render(s, b.data() + done, BUFFER_FRAMES);
    msynth_destroy(s);

    CHECK(a == b);
    CHECK(!a.empty());
}

TEST(c_api_song_config_and_transport) {
    msynth_ctx* s = msynth_create();
    char report[512];
    static const char* kFx = R"({"reverb":{"wetGain":0.5},"chorus":{"wetGain":0}})";
    CHECK_EQ(msynth_load_song_config_json(s, kFx, std::strlen(kFx), report, sizeof(report)), 0);
    CHECK_EQ(msynth_load_song_config_json(s, "bad", 3, report, sizeof(report)), -1);

    auto smf = two_note_song();
    msynth_load_song(s, smf.data(), smf.size(), report, sizeof(report));
    msynth_set_loop(s, 1);
    msynth_set_tempo_scale_milli(s, 2000);  // double speed
    msynth_play(s);
    i16 buf[512 * 2];
    for (int i = 0; i < 200; ++i) msynth_render(s, buf, 512);  // > song length: loop keeps going
    CHECK_EQ(msynth_playing(s), 1);
    uint32_t bar = 99, beat = 99;
    msynth_bar_beat(s, &bar, &beat);
    CHECK(bar < 4);
    msynth_stop(s);
    // Voices fast-release on stop, but the REVERB TAIL keeps ringing --
    // that's the seamless-tail behavior the plan requires. Zero the wet
    // return via config to assert full silence.
    static const char* kDry = R"({"reverb":{"wetGain":0},"chorus":{"wetGain":0}})";
    msynth_load_song_config_json(s, kDry, std::strlen(kDry), nullptr, 0);
    for (int i = 0; i < 8; ++i) msynth_render(s, buf, 512);
    i64 peak = 0;
    msynth_render(s, buf, 512);
    for (i16 v : buf) peak = std::max<i64>(peak, v < 0 ? -v : v);
    CHECK_EQ(peak, 0);
    msynth_destroy(s);
}

TEST(c_api_beat_callback_fires) {
    struct Ctx {
        int count = 0;
    } cb_ctx;
    msynth_ctx* s = msynth_create();
    char report[256];
    auto smf = two_note_song();
    msynth_load_song(s, smf.data(), smf.size(), report, sizeof(report));
    msynth_set_beat_callback(
        s, [](void* user, uint32_t, uint32_t) { ++static_cast<Ctx*>(user)->count; }, &cb_ctx);
    msynth_play(s);
    i16 buf[512 * 2];
    for (int i = 0; i < 100; ++i) msynth_render(s, buf, 512);  // ~1.07s = 2+ beats
    CHECK(cb_ctx.count >= 2);
    msynth_destroy(s);
}
