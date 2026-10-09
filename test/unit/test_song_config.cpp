#include "test_framework.h"

#include "engine_helpers.h"
#include "synth/song_config.h"

using namespace msynth;
using testfw::EngineFixture;
using testfw::peak_abs;

TEST(song_config_parses_plan_example) {
    static const char* kJson = R"({
      "version": 1,
      "reverb": { "roomSize": 0.8, "damping": 0.4, "width": 1.0, "wetGain": 0.3 },
      "chorus": { "rateHz": 0.8, "depthMs": 3.5, "wetGain": 0.2 }
    })";
    SongFxConfig cfg;
    LoadResult r = load_song_config(kJson, std::strlen(kJson), cfg);
    CHECK(r.ok);
    CHECK(r.warnings.empty());
    CHECK_EQ(cfg.reverb.room_size, (8 * GAIN_ONE) / 10);
    CHECK_EQ(cfg.reverb.damping, (4 * GAIN_ONE) / 10);
    CHECK_EQ(cfg.reverb.width, GAIN_ONE);
    CHECK_EQ(cfg.reverb.wet_gain, (3 * GAIN_ONE) / 10);
    CHECK_EQ(cfg.chorus.rate_hz_q16, (8 * GAIN_ONE) / 10);
    CHECK_EQ(cfg.chorus.depth_ms_q16, (7 * GAIN_ONE) / 2);
    CHECK_EQ(cfg.chorus.wet_gain, (2 * GAIN_ONE) / 10);
}

TEST(song_config_defaults_when_sections_missing) {
    static const char* kJson = R"({ "version": 1 })";
    SongFxConfig cfg;
    LoadResult r = load_song_config(kJson, std::strlen(kJson), cfg);
    CHECK(r.ok);
    const SongFxConfig defaults;
    CHECK_EQ(cfg.reverb.room_size, defaults.reverb.room_size);
    CHECK_EQ(cfg.chorus.wet_gain, defaults.chorus.wet_gain);
}

TEST(song_config_clamps_out_of_range_with_warning) {
    static const char* kJson = R"({ "reverb": { "roomSize": 5.0, "damping": -1 } })";
    SongFxConfig cfg;
    LoadResult r = load_song_config(kJson, std::strlen(kJson), cfg);
    CHECK(r.ok);
    CHECK_EQ(r.warnings.size(), 2);
    CHECK_EQ(cfg.reverb.room_size, GAIN_ONE);
    CHECK_EQ(cfg.reverb.damping, 0);
}

TEST(song_config_bad_json_is_error) {
    SongFxConfig cfg;
    LoadResult r = load_song_config("{{nope", 6, cfg);
    CHECK(!r.ok);
    CHECK(!r.errors.empty());
}

TEST(song_config_unknown_version_warns) {
    static const char* kJson = R"({ "version": 9 })";
    SongFxConfig cfg;
    LoadResult r = load_song_config(kJson, std::strlen(kJson), cfg);
    CHECK(r.ok);
    CHECK(!r.warnings.empty());
}

TEST(song_config_applies_to_engine) {
    // Wet 0 config -> no reverb tail; wet-heavy config -> tail. Same note.
    auto tail_with = [](const char* fx) {
        EngineFixture f;   // fixture zeroes wet by default
        SongFxConfig cfg;
        LoadResult r = load_song_config(fx, std::strlen(fx), cfg);
        CHECK(r.ok);
        push_song_config(f.engine, cfg);
        f.cc(0, 91, 127);
        f.note_on(0, 60, 127);
        f.render(SAMPLE_RATE / 4);
        f.cc(0, 120, 0);
        f.render(BUFFER_FRAMES * 2);
        return peak_abs(f.render(BUFFER_FRAMES * 4));
    };
    CHECK_EQ(tail_with(R"({"reverb":{"wetGain":0},"chorus":{"wetGain":0}})"), 0);
    CHECK(tail_with(R"({"reverb":{"wetGain":0.6},"chorus":{"wetGain":0}})") > 20);
}

TEST(song_config_channel_eq) {
    SongFxConfig cfg;
    static const char* kFx = R"({"channelEq":{"1":{"lowDb":-3,"midHz":900,"midDb":2,"highDb":1},
                                             "10":{"highDb":-6},"17":{"lowDb":1},"x":{}}})";
    LoadResult r = load_song_config(kFx, std::strlen(kFx), cfg);
    CHECK(r.ok);
    CHECK_EQ(cfg.channel_eq[0].low_db_q16, -(3 << GAIN_SHIFT));
    CHECK_EQ(cfg.channel_eq[0].mid_hz_q16, 900 << GAIN_SHIFT);
    CHECK_EQ(cfg.channel_eq[9].high_db_q16, -(6 << GAIN_SHIFT));
    CHECK(cfg.channel_eq[0].active());
    CHECK(!cfg.channel_eq[1].active());
    CHECK_EQ(r.warnings.size(), 2);  // channel 17 + non-numeric key
}
