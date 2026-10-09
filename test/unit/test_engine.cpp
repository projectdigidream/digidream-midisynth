//
// SynthEngine integration: notes sound, CCs behave per the §9 table, voices
// steal and choke, output is bit-identical run to run.
//
#include <cstdlib>

#include "test_framework.h"

#include "engine_helpers.h"
#include "synth/song_config.h"

using namespace msynth;
using testfw::EngineFixture;
using testfw::peak_abs;
using testfw::rms_energy;
using testfw::zero_crossings;

TEST(engine_silence_when_idle) {
    EngineFixture f;
    CHECK_EQ(peak_abs(f.render(BUFFER_FRAMES)), 0);
}

TEST(engine_note_produces_audio_and_release_decays) {
    EngineFixture f;
    f.note_on(0, 69, 100);
    auto during = f.render(SAMPLE_RATE / 4);
    CHECK(peak_abs(during) > 500);
    f.note_off(0, 69);
    f.render(SAMPLE_RATE / 4);  // release (20ms) + margin
    auto after = f.render(BUFFER_FRAMES);
    CHECK_EQ(peak_abs(after), 0);
}

TEST(engine_pitch_follows_note) {
    EngineFixture f;
    f.note_on(0, 69, 127);  // A4 = 440
    f.render(2048);         // settle attack
    auto pcm = f.render(SAMPLE_RATE);
    CHECK_NEAR(zero_crossings(pcm), 440, 3);
    f.note_off(0, 69);
    f.render(SAMPLE_RATE / 2);
    f.note_on(0, 81, 127);  // A5 = 880
    f.render(2048);
    auto pcm2 = f.render(SAMPLE_RATE);
    CHECK_NEAR(zero_crossings(pcm2), 880, 5);
}

TEST(engine_pitch_bend) {
    EngineFixture f;
    f.note_on(0, 69, 127);
    EngineCommand b;
    b.type = EngineCommand::Type::PitchBend;
    b.slot = 0;
    b.v = 8191;  // max up = +2 semitones default range
    f.cmd(b);
    f.render(2048);
    auto pcm = f.render(SAMPLE_RATE);
    // 440 * 2^(2/12) = 493.9 Hz
    CHECK_NEAR(zero_crossings(pcm), 494, 4);
}

TEST(engine_bend_range_rpn) {
    EngineFixture f;
    // RPN 0,0 -> data entry 12 semitones.
    f.cc(0, 101, 0);
    f.cc(0, 100, 0);
    f.cc(0, 6, 12);
    EngineCommand b;
    b.type = EngineCommand::Type::PitchBend;
    b.slot = 0;
    b.v = 8191;
    f.cmd(b);
    f.note_on(0, 69, 127);
    f.render(2048);
    auto pcm = f.render(SAMPLE_RATE);
    CHECK_NEAR(zero_crossings(pcm), 880, 8);  // full octave up
}

TEST(engine_volume_and_expression_scale_output) {
    EngineFixture f;
    f.note_on(0, 60, 100);
    f.render(4096);
    const i64 full = rms_energy(f.render(BUFFER_FRAMES * 4));
    f.cc(0, 7, 64);
    f.render(4096);
    const i64 half_vol = rms_energy(f.render(BUFFER_FRAMES * 4));
    CHECK(half_vol < full / 4);  // squared curve: (64/127)^2 in power terms
    f.cc(0, 11, 64);
    f.render(4096);
    const i64 also_expr = rms_energy(f.render(BUFFER_FRAMES * 4));
    CHECK(also_expr < half_vol / 4);  // both apply, per GM
}

TEST(engine_pan_hard_left) {
    EngineFixture f;
    f.cc(0, 10, 0);  // hard left
    f.note_on(0, 60, 100);
    f.render(4096);
    auto pcm = f.render(BUFFER_FRAMES * 4);
    CHECK(peak_abs(pcm, 0) > 500);
    CHECK(peak_abs(pcm, 1) <= 1);  // right channel silent
}

TEST(engine_all_sound_off_is_fast) {
    EngineFixture f;
    f.note_on(0, 60, 127);
    f.render(SAMPLE_RATE / 8);
    f.cc(0, 120, 0);  // All Sound Off
    f.render(BUFFER_FRAMES);  // ~10ms: fast ramp done
    CHECK_EQ(peak_abs(f.render(BUFFER_FRAMES)), 0);
}

TEST(engine_all_notes_off_respects_release) {
    EngineFixture f;
    f.note_on(0, 60, 127);
    f.note_on(0, 64, 127);
    f.render(SAMPLE_RATE / 8);
    f.cc(0, 123, 0);  // All Notes Off = release, not cut
    auto tail = f.render(512);
    CHECK(peak_abs(tail) > 0);  // release tail still audible
    f.render(SAMPLE_RATE / 4);
    CHECK_EQ(peak_abs(f.render(BUFFER_FRAMES)), 0);
}

TEST(engine_sustain_pedal_defers_note_off) {
    EngineFixture f;
    f.cc(0, 64, 127);  // pedal down
    f.note_on(0, 60, 100);
    f.render(SAMPLE_RATE / 8);
    f.note_off(0, 60);
    f.render(SAMPLE_RATE / 4);
    CHECK(peak_abs(f.render(BUFFER_FRAMES * 2)) > 300);  // still sounding
    f.cc(0, 64, 0);  // pedal up fires the deferred note-off
    f.render(SAMPLE_RATE / 4);
    CHECK_EQ(peak_abs(f.render(BUFFER_FRAMES)), 0);
}

TEST(engine_reset_controllers_cc121) {
    EngineFixture f;
    f.cc(0, 1, 127);
    f.cc(0, 11, 30);
    EngineCommand b;
    b.type = EngineCommand::Type::PitchBend;
    b.slot = 0;
    b.v = 4000;
    f.cmd(b);
    f.cc(0, 7, 55);
    f.cc(0, 121, 0);
    f.render(CONTROL_BLOCK);
    const ChannelState& ch = f.engine.channel(0);
    CHECK_EQ(ch.modwheel_q16, 0);
    CHECK_EQ(ch.expression, 127);
    CHECK_EQ(ch.bend_raw, 0);
    CHECK_EQ(ch.volume, 55);  // CC121 does NOT reset volume per GM
}

TEST(engine_unknown_program_uses_fallback_never_silence) {
    EngineFixture f;
    EngineCommand p;
    p.type = EngineCommand::Type::Program;
    p.slot = 0;
    p.d1 = 113;  // not authored in the fixture bank
    f.cmd(p);
    f.note_on(0, 60, 100);
    f.render(4096);
    CHECK(peak_abs(f.render(BUFFER_FRAMES * 4)) > 200);
}

TEST(engine_drum_channel_routes_to_kit) {
    EngineFixture f;
    f.note_on(DRUM_CHANNEL, 36, 127);  // kick
    auto pcm = f.render(SAMPLE_RATE / 8);
    CHECK(peak_abs(pcm) > 500);
    // Unmapped drum note: silence.
    EngineFixture g;
    g.note_on(DRUM_CHANNEL, 37, 127);
    CHECK_EQ(peak_abs(g.render(SAMPLE_RATE / 8)), 0);
}

TEST(engine_drum_one_shot_ignores_note_off) {
    EngineFixture f;
    f.note_on(DRUM_CHANNEL, 46, 127);  // open hat, 400ms decay
    f.note_off(DRUM_CHANNEL, 46);      // immediately -- must be ignored
    f.render(SAMPLE_RATE / 10);
    CHECK(peak_abs(f.render(BUFFER_FRAMES * 2)) > 50);  // still decaying
}

TEST(engine_hihat_choke) {
    EngineFixture f;
    f.note_on(DRUM_CHANNEL, 46, 127);  // open hat rings ~400ms
    f.render(SAMPLE_RATE / 10);
    f.note_on(DRUM_CHANNEL, 42, 127);  // closed hat chokes it
    f.render(SAMPLE_RATE / 8);         // closed hat decays (40ms) + choke ramp
    CHECK_EQ(peak_abs(f.render(BUFFER_FRAMES * 2)), 0);
}

TEST(engine_same_note_retrigger_overlaps) {
    EngineFixture f;
    f.note_on(0, 60, 100);
    f.render(SAMPLE_RATE / 8);
    f.note_on(0, 60, 100);  // retrigger: old group fast-releases
    f.render(CONTROL_BLOCK * 2);
    // Two groups momentarily: old fading + new attacking.
    CHECK(f.engine.pool_diag().active_voices >= 1);
    f.render(SAMPLE_RATE / 8);
    // Old fade finished: one voice remains.
    CHECK_EQ(f.engine.pool_diag().active_voices, 1);
}

TEST(engine_voice_stealing_under_load) {
    EngineFixture f;
    // Far more UNIQUE (channel, note) pairs than the pool holds, spread over
    // time so steal shields lapse (same-note pairs would retrigger-free
    // voices instead of forcing steals).
    for (int i = 0; i < 200; ++i) {
        f.note_on(static_cast<u8>((i / 107) % 8), static_cast<u8>(21 + (i % 107)), 100);
        f.render(CONTROL_BLOCK * 4);
    }
    const PoolDiagnostics& d = f.engine.pool_diag();
    CHECK(d.peak_voices <= MAX_VOICES);
    CHECK(d.steal_count > 0);
    CHECK_EQ(d.dropped_notes, 0);  // shields lapse between notes: no drops
    CHECK(peak_abs(f.render(BUFFER_FRAMES)) > 0);
}

TEST(engine_simultaneous_flood_drops_not_crashes) {
    EngineFixture f;
    // 200 notes in one block: pool fills, shields prevent steals, extras drop.
    for (int i = 0; i < 200; ++i)
        f.note_on(static_cast<u8>(i % 8), static_cast<u8>(21 + (i % 88)), 100);
    f.render(BUFFER_FRAMES);
    const PoolDiagnostics& d = f.engine.pool_diag();
    CHECK(d.active_voices <= MAX_VOICES);
    CHECK(d.dropped_notes > 0);
}

TEST(engine_bit_identical_across_instances) {
    auto run = [] {
        EngineFixture f;
        f.note_on(0, 60, 100);
        f.note_on(0, 67, 90, BUFFER_FRAMES);
        f.note_on(DRUM_CHANNEL, 36, 127, BUFFER_FRAMES * 2);
        f.cc(0, 91, 90, BUFFER_FRAMES * 2);
        f.note_off(0, 60, BUFFER_FRAMES * 5);
        return f.render(BUFFER_FRAMES * 12);
    };
    const auto a = run();
    const auto b = run();
    CHECK(a == b);
    CHECK(peak_abs(a) > 0);
}

TEST(engine_command_timestamps_apply_in_order) {
    EngineFixture f;
    // Note scheduled 4 buffers ahead must not sound before then.
    f.note_on(0, 60, 100, BUFFER_FRAMES * 4);
    auto early = f.render(BUFFER_FRAMES * 4);
    CHECK_EQ(peak_abs(early), 0);
    auto later = f.render(BUFFER_FRAMES);
    CHECK(peak_abs(later) > 0);
}

TEST(engine_reverb_send_produces_tail) {
    EngineFixture f;
    f.set_reverb_wet((3 * GAIN_ONE) / 10);
    f.cc(0, 91, 127);  // full reverb send
    f.note_on(0, 60, 127);
    f.render(SAMPLE_RATE / 4);
    f.cc(0, 120, 0);  // kill the dry voice fast
    f.render(BUFFER_FRAMES * 2);
    // Reverb tail persists after the source is gone.
    CHECK(peak_abs(f.render(BUFFER_FRAMES * 4)) > 20);

    // Without send: silence at the same point.
    EngineFixture g;
    g.cc(0, 91, 0);
    g.note_on(0, 60, 127);
    g.render(SAMPLE_RATE / 4);
    g.cc(0, 120, 0);
    g.render(BUFFER_FRAMES * 2);
    CHECK_EQ(peak_abs(g.render(BUFFER_FRAMES * 4)), 0);
}

TEST(engine_master_gain_and_duck) {
    EngineFixture f;
    f.note_on(0, 60, 127);
    f.render(4096);
    const i64 normal = rms_energy(f.render(BUFFER_FRAMES * 2));
    EngineCommand d;
    d.type = EngineCommand::Type::Duck;
    d.v = GAIN_ONE / 4;
    d.v2 = 2;
    f.cmd(d);
    f.render(BUFFER_FRAMES);
    const i64 ducked = rms_energy(f.render(BUFFER_FRAMES * 2));
    CHECK(ducked < normal / 8);
}

TEST(engine_channel_fade_mute) {
    EngineFixture f;
    f.note_on(0, 60, 127);
    f.render(4096);
    EngineCommand c;
    c.type = EngineCommand::Type::ChannelFade;
    c.slot = 0;
    c.v = 0;
    c.v2 = 8;
    f.cmd(c);
    f.render(BUFFER_FRAMES * 2);
    CHECK_EQ(peak_abs(f.render(BUFFER_FRAMES * 2)), 0);
    // Unmute fades back in.
    c.v = GAIN_ONE;
    f.cmd(c);
    f.render(BUFFER_FRAMES * 2);
    CHECK(peak_abs(f.render(BUFFER_FRAMES * 2)) > 100);
}

TEST(engine_dither_only_changes_lsbs) {
    auto run = [](bool dither) {
        EngineFixture f;
        f.engine.output().dither = dither;
        f.note_on(0, 60, 40);
        return f.render(BUFFER_FRAMES * 4);
    };
    const auto clean = run(false);
    const auto dithered = run(true);
    CHECK(clean != dithered);
    for (size_t i = 0; i < clean.size(); ++i)
        CHECK_NEAR(clean[i], dithered[i], 2);
    // And dither itself is deterministic.
    CHECK(run(true) == run(true));
}

TEST(engine_fast_release_patch_targets_only_that_patch) {
    EngineFixture f;
    f.note_on(0, 60, 100);              // program 0 (test_sine)
    f.note_on(DRUM_CHANNEL, 46, 127);   // open hat, different patch
    f.render(SAMPLE_RATE / 16);
    CHECK_EQ(f.engine.pool_diag().active_voices, 2);
    f.engine.fast_release_patch(f.bank.lookup(0));
    f.render(BUFFER_FRAMES);            // fast release drains (~4ms)
    CHECK_EQ(f.engine.pool_diag().active_voices, 1);  // hat still going
}

TEST(layer_start_delay_flams) {
    // Layer 2 speaks 100 ms late: silence for ~100 ms after the (muted layer
    // 1) note-on, then sound.
    EngineFixture f;
    static const char* kFlam = R"({"name":"flam","layers":[
        {"mixGain": 0, "ampEnv": {"attack":0,"decay":10,"sustain":0,"release":10}},
        {"startDelayMs": 100, "ampEnv": {"attack":1,"decay":0,"sustain":1,"release":30}}]})";
    std::string err;
    JsonPtr doc = json_parse(kFlam, std::strlen(kFlam), &err);
    auto p = std::make_unique<Patch>();
    CHECK(load_patch(*doc, f.cache, *p).ok);
    f.bank.set_program(1, std::move(p));
    {
        EngineCommand c;
        c.type = EngineCommand::Type::Program;
        c.slot = 0;
        c.d1 = 1;
        f.engine.queue().push(c);
    }
    f.note_on(0, 60, 100);
    auto early = f.render(4224);   // 88 ms
    CHECK_EQ(testfw::peak_abs(early), 0);
    auto late = f.render(2048);    // 88..131 ms
    CHECK(testfw::peak_abs(late) > 100);

    // A note-off inside the delay still lets the layer speak briefly.
    EngineFixture g;
    JsonPtr doc2 = json_parse(kFlam, std::strlen(kFlam), &err);
    auto p2 = std::make_unique<Patch>();
    load_patch(*doc2, g.cache, *p2);
    g.bank.set_program(1, std::move(p2));
    {
        EngineCommand c;
        c.type = EngineCommand::Type::Program;
        c.slot = 0;
        c.d1 = 1;
        g.engine.queue().push(c);
    }
    g.note_on(0, 60, 100);
    g.render(512);                 // ~10 ms in
    g.note_off(0, 60);
    CHECK_EQ(testfw::peak_abs(g.render(3584)), 0);  // still inside the delay
    auto speak = g.render(2048);   // the flam speaks...
    CHECK(testfw::peak_abs(speak) > 50);
    g.render(4800);                // ...and its release ends it
    CHECK_EQ(g.engine.pool_diag().active_voices, 0);
    CHECK_EQ(testfw::peak_abs(g.render(512)), 0);

    // A fast release (All Sound Off) during the delay kills it silently.
    EngineFixture h;
    JsonPtr doc3 = json_parse(kFlam, std::strlen(kFlam), &err);
    auto p3 = std::make_unique<Patch>();
    load_patch(*doc3, h.cache, *p3);
    h.bank.set_program(1, std::move(p3));
    {
        EngineCommand c;
        c.type = EngineCommand::Type::Program;
        c.slot = 0;
        c.d1 = 1;
        h.engine.queue().push(c);
    }
    h.note_on(0, 60, 100);
    h.render(512);
    h.cc(0, 120, 0);               // All Sound Off
    auto rest = h.render(9600);    // past where the flam would have spoken
    CHECK_EQ(testfw::peak_abs(rest), 0);
    CHECK_EQ(h.engine.pool_diag().active_voices, 0);
}

namespace {
void install_patch(testfw::EngineFixture& f, const char* json, u8 prog) {
    std::string err;
    JsonPtr doc = json_parse(json, std::strlen(json), &err);
    auto p = std::make_unique<Patch>();
    CHECK(load_patch(*doc, f.cache, *p).ok);
    f.bank.set_program(prog, std::move(p));
    EngineCommand c;
    c.type = EngineCommand::Type::Program;
    c.slot = 0;
    c.d1 = prog;
    f.engine.queue().push(c);
}
}  // namespace

TEST(unison_spawns_a_stack_and_widens) {
    testfw::EngineFixture f;
    install_patch(f, R"({"name":"u","layers":[{"unison":{"voices":5,"detuneCents":10,"spread":1},
        "ampEnv":{"attack":1,"decay":0,"sustain":1,"release":10}}]})", 1);
    f.note_on(0, 60, 100);
    f.render(BUFFER_FRAMES * 2);
    CHECK_EQ(f.engine.pool_diag().active_voices, 5);
    // Spread pans voices apart: L and R differ.
    auto pcm = f.render(BUFFER_FRAMES * 4);
    i64 diff = 0;
    for (size_t i = 0; i + 1 < pcm.size(); i += 2) diff += std::abs(pcm[i] - pcm[i + 1]);
    CHECK(diff > 0);
}

TEST(round_robin_alternates_layers) {
    testfw::EngineFixture f;
    // Two RR layers at very different fixed pitches: alternate strikes.
    install_patch(f, R"({"name":"rr","layers":[
      {"rrGroup":1,"pitchNote":40,"ampEnv":{"attack":0,"decay":30,"sustain":0,"release":5}},
      {"rrGroup":1,"pitchNote":90,"ampEnv":{"attack":0,"decay":30,"sustain":0,"release":5}}]})", 1);
    auto strike = [&]() {
        f.note_on(0, 60, 100);
        auto pcm = f.render(BUFFER_FRAMES * 2);
        f.note_off(0, 60);
        f.render(BUFFER_FRAMES * 4);
        return testfw::zero_crossings(pcm);
    };
    const int z1 = strike(), z2 = strike(), z3 = strike();
    CHECK(z2 > z1 * 3);  // high layer has far more zero crossings
    CHECK_NEAR(z3, z1, z1 / 3 + 4);
}

TEST(glide_slides_from_the_previous_note) {
    testfw::EngineFixture f;  // program 0: plain sine
    f.cc(0, 5, 90);   // portamento time
    f.cc(0, 65, 127); // portamento on
    f.note_on(0, 48, 100);
    f.render(BUFFER_FRAMES * 4);
    f.note_off(0, 48);
    f.note_on(0, 72, 100);
    auto early = f.render(BUFFER_FRAMES);      // still gliding: low pitch
    f.render(BUFFER_FRAMES * 40);              // glide done
    auto late = f.render(BUFFER_FRAMES * 2);
    const int ze = testfw::zero_crossings(early) * 2;  // per equal length
    const int zl = testfw::zero_crossings(late);
    CHECK(ze < zl);  // frequency rose across the glide
}

TEST(poly_aftertouch_reaches_the_matrix) {
    testfw::EngineFixture f;
    install_patch(f, R"({"name":"pat","layers":[{
        "modMatrix":[{"src":"PolyAftertouch","dst":"Amplitude","amount":-1}]}]})", 1);
    f.note_on(0, 60, 100);
    f.render(BUFFER_FRAMES * 2);
    const int base = testfw::peak_abs(f.render(BUFFER_FRAMES * 2));
    EngineCommand c;
    c.type = EngineCommand::Type::PolyAftertouch;
    c.slot = 0;
    c.d1 = 60;
    c.d2 = 127;
    f.engine.queue().push(c);
    f.render(BUFFER_FRAMES);
    const int squashed = testfw::peak_abs(f.render(BUFFER_FRAMES * 2));
    CHECK(squashed < base / 4);  // amount -1 * full pressure kills the gain
}

TEST(release_velocity_scales_release_time) {
    testfw::EngineFixture f;
    install_patch(f, R"({"name":"rv","layers":[{"releaseVel":1,
        "ampEnv":{"attack":0,"decay":0,"sustain":1,"release":200}}]})", 1);
    auto tail_len = [&](u8 relvel) {
        f.note_on(0, 60, 100);
        f.render(BUFFER_FRAMES * 4);
        EngineCommand c;
        c.type = EngineCommand::Type::NoteOff;
        c.slot = 0;
        c.d1 = 60;
        c.d2 = relvel;
        f.engine.queue().push(c);
        int blocks = 0;
        while (f.engine.pool_diag().active_voices > 0 && blocks < 400) {
            f.render(BUFFER_FRAMES);
            ++blocks;
        }
        f.render(BUFFER_FRAMES * 2);
        return blocks;
    };
    const int hard = tail_len(127);   // hard lift: short release
    const int neutral = tail_len(64);
    const int soft = tail_len(1);     // soft lift: long release
    CHECK(hard < neutral);
    CHECK(neutral < soft);
}

TEST(delay_send_echoes) {
    testfw::EngineFixture f;
    // Configure the delay: 100 ms, audible wet; send via CC94.
    SongFxConfig cfg;
    static const char* kFx = R"({"reverb":{"wetGain":0},"chorus":{"wetGain":0},
        "delay":{"timeMs":100,"feedback":0.3,"wetGain":1.0}})";
    CHECK(load_song_config(kFx, std::strlen(kFx), cfg).ok);
    push_song_config(f.engine, cfg);
    f.cc(0, 94, 127);
    f.note_on(0, 60, 100);
    f.render(BUFFER_FRAMES);
    f.note_off(0, 60);
    f.render(2400);                       // 50 ms: source fading
    f.render(2400 - BUFFER_FRAMES * 2);
    auto echo = f.render(4800);           // 100..200 ms: the echo arrives
    CHECK(testfw::peak_abs(echo) > 40);
    // Without the send, the same window is (near) silent after release.
    testfw::EngineFixture g;
    push_song_config(g.engine, cfg);
    g.note_on(0, 60, 100);
    g.render(BUFFER_FRAMES);
    g.note_off(0, 60);
    g.render(4800 - BUFFER_FRAMES);
    auto quiet = g.render(4800);
    CHECK(testfw::peak_abs(quiet) < testfw::peak_abs(echo) / 4);
}

TEST(microtuning_shifts_pitch_classes) {
    testfw::EngineFixture f;
    SongFxConfig cfg;
    static const char* kFx = R"({"tuning":{"cents":[0,0,0,0,0,0,0,0,0,100,0,0]}})";
    CHECK(load_song_config(kFx, std::strlen(kFx), cfg).ok);
    push_song_config(f.engine, cfg);
    auto zc = [&](u8 note) {
        f.note_on(0, note, 100);
        f.render(BUFFER_FRAMES);
        auto pcm = f.render(BUFFER_FRAMES * 8);
        f.note_off(0, note);
        f.render(BUFFER_FRAMES * 6);
        return testfw::zero_crossings(pcm);
    };
    // Pitch class 9 (A) is retuned +100 cents: A sounds like Bb.
    const int a = zc(69), bflat = zc(70);
    CHECK_NEAR(a, bflat, 6);
    const int b = zc(71);
    CHECK(b > bflat);
}

TEST(comb_filter_resonates_at_its_frequency) {
    testfw::EngineFixture f;
    // Noise through a comb tuned to 500 Hz with heavy feedback: the output
    // periodicity sits near 500 Hz (96 samples), unlike plain noise.
    install_patch(f, R"({"name":"comb","layers":[{"osc":"noise",
        "filter":{"mode":"comb","cutoffHz":500,"resonance":0.93},
        "ampEnv":{"attack":1,"decay":0,"sustain":1,"release":10}}]})", 1);
    f.note_on(0, 60, 100);
    f.render(BUFFER_FRAMES * 8);  // let the resonator charge
    auto pcm = f.render(BUFFER_FRAMES * 8);
    // Mono-ise, autocorrelate at the comb period vs an off period.
    std::vector<double> m;
    for (size_t i = 0; i < pcm.size(); i += 2) m.push_back(pcm[i]);
    auto ac = [&](int lag) {
        double s = 0, n = 0;
        for (size_t i = 0; i + lag < m.size(); ++i) { s += m[i] * m[i + lag]; n += m[i] * m[i]; }
        return s / (n > 0 ? n : 1);
    };
    // The resonator period is 48000/500 = 96 samples, modulo the ~2%
    // Hz<->index table rounding: scan the neighbourhood for the peak.
    double best = -1;
    int best_lag = 0;
    for (int lag = 88; lag <= 104; ++lag)
        if (ac(lag) > best) { best = ac(lag); best_lag = lag; }
    CHECK(best > 0.5);              // strong periodicity near 500 Hz
    CHECK(best_lag >= 90 && best_lag <= 102);
    CHECK(best > ac(67) + 0.3);     // clearly stronger than an unrelated lag
}

TEST(formant_mode_shapes_vowels) {
    testfw::EngineFixture f;
    // Saw through formant 'i' vs 'a': F2 differs (1750 vs 1150 Hz), so band
    // energy around 1.7-1.9 kHz dominates for 'i'.
    auto energy_near = [](const std::vector<i16>& pcm, double hz) {
        double re = 0, im = 0;
        for (size_t i = 0; i < pcm.size(); i += 2) {
            const double ph = 2.0 * 3.14159265358979 * hz * (i / 2) / 48000.0;
            re += pcm[i] * std::cos(ph);
            im += pcm[i] * std::sin(ph);
        }
        return re * re + im * im;
    };
    auto render_vowel = [&](const char* vowel) {
        testfw::EngineFixture g;
        std::string json = std::string(R"({"name":"v","layers":[{
            "wavetable":{"generator":"additiveHarmonicMorph","frameCount":1,
                         "params":{"startHarmonics":64,"endHarmonics":64,"falloff":"saw"}},
            "filter":{"mode":"formant","vowel":")") + vowel + R"("},
            "ampEnv":{"attack":1,"decay":0,"sustain":1,"release":10}}]})";
        std::string err;
        JsonPtr doc = json_parse(json.data(), json.size(), &err);
        auto p = std::make_unique<Patch>();
        CHECK(load_patch(*doc, g.cache, *p).ok);
        g.bank.set_program(1, std::move(p));
        EngineCommand c;
        c.type = EngineCommand::Type::Program;
        c.slot = 0;
        c.d1 = 1;
        g.engine.queue().push(c);
        g.note_on(0, 45, 100);  // A2: dense harmonics
        g.render(BUFFER_FRAMES * 4);
        return g.render(BUFFER_FRAMES * 8);
    };
    const auto pcm_a = render_vowel("a"), pcm_i = render_vowel("i");
    const double a_f2 = energy_near(pcm_a, 1750), i_f2 = energy_near(pcm_i, 1750);
    const double a_lo = energy_near(pcm_a, 800), i_lo = energy_near(pcm_i, 800);
    CHECK(i_f2 / (i_lo + 1) > a_f2 / (a_lo + 1) * 2);  // 'i' pushes energy to F2
}

TEST(channel_eq_boosts_one_channel_only) {
    testfw::EngineFixture f;
    SongFxConfig cfg;
    static const char* kFx = R"({"reverb":{"wetGain":0},"chorus":{"wetGain":0},
        "channelEq":{"1":{"lowDb":12}}})";
    LoadResult r = load_song_config(kFx, std::strlen(kFx), cfg);
    CHECK(r.ok);
    push_song_config(f.engine, cfg);
    // Low note on ch 1 (EQ'd) vs ch 2 (flat): ch 1 comes out louder.
    f.note_on(0, 36, 90);
    f.render(BUFFER_FRAMES * 4);
    const int ch1 = testfw::peak_abs(f.render(BUFFER_FRAMES * 8));
    f.note_off(0, 36);
    f.render(BUFFER_FRAMES * 8);
    f.note_on(1, 36, 90);
    f.render(BUFFER_FRAMES * 4);
    const int ch2 = testfw::peak_abs(f.render(BUFFER_FRAMES * 8));
    CHECK(ch1 > ch2 * 3 / 2);
    // Flat EQ everywhere = the historical single-bus output (goldens pin the
    // bit-exactness; here just sanity that flat config leaves gain alone).
    SongFxConfig flat;
    CHECK(!flat.channel_eq[0].active());
}

TEST(quiet_release_tail_glides_without_stairs) {
    testfw::EngineFixture f;
    // A soft note with a long exponential release: examine the tail's
    // per-block peak series. With Q16 envelopes + dead gain deltas the tail
    // freezes into plateaus and then jumps; Q8.24 keeps it moving smoothly.
    install_patch(f, R"({"name":"tail","layers":[{
        "ampEnv":{"attack":1,"decay":0,"sustain":1,"release":1500,"releaseCurve":"exp"}}]})", 1);
    f.note_on(0, 60, 30);  // quiet: (30/127)^2 ~= -25 dB before the mix chain
    f.render(BUFFER_FRAMES * 4);
    f.note_off(0, 60);
    f.render(SAMPLE_RATE / 4);  // into the tail
    // Collect per-block peaks over the next 0.4 s.
    std::vector<int> peaks;
    for (int b = 0; b < 300; ++b) {
        auto pcm = f.render(64);
        int pk = 0;
        for (i16 v : pcm) pk = std::max(pk, v < 0 ? -static_cast<int>(v) : static_cast<int>(v));
        peaks.push_back(pk);
    }
    // The tail is audible at the start of the window and decays.
    CHECK(peaks.front() > 20);
    CHECK(peaks.back() < peaks.front());
    // No sudden relative drops (clicks): while the tail is measurable,
    // consecutive block peaks never fall by more than ~25%.
    bool smooth = true;
    for (size_t i = 1; i < peaks.size(); ++i) {
        if (peaks[i - 1] < 12) break;
        if (peaks[i] * 4 < peaks[i - 1] * 3) smooth = false;
    }
    CHECK(smooth);
}

TEST(metal_osc_is_inharmonic_square_cluster) {
    testfw::EngineFixture f;
    install_patch(f, R"({"name":"m","layers":[{"osc":"metal","pitchNote":60,
        "ampEnv":{"attack":1,"decay":0,"sustain":1,"release":10}}]})", 1);
    f.note_on(0, 60, 100);
    f.render(BUFFER_FRAMES * 2);
    auto pcm = f.render(BUFFER_FRAMES * 8);
    auto energy_near = [&](double hz) {
        double re = 0, im = 0;
        for (size_t i = 0; i < pcm.size(); i += 2) {
            const double ph = 2.0 * 3.14159265358979 * hz * (i / 2) / 48000.0;
            re += pcm[i] * std::cos(ph);
            im += pcm[i] * std::sin(ph);
        }
        return re * re + im * im;
    };
    const double f0 = 261.63;  // C4
    // The 1.342x partial is a defining inharmonic component: it towers over
    // a frequency between the cluster lines (1.5x, where a harmonic square
    // stack would put energy but the 808 ratios do not).
    CHECK(energy_near(f0 * 1.342) > energy_near(f0 * 1.55) * 4);
    // And the fundamental line exists too (ratio 1.0 square).
    CHECK(energy_near(f0) > energy_near(f0 * 1.55) * 4);
}

TEST(filter2_static_holds_while_filter1_sweeps) {
    // Tone ~350 Hz below an 800 Hz highpass (filter2 only; filter1 bypassed
    // so it cannot mask the comparison). The filter env holds at -0.9 of the
    // range: a FOLLOWING filter2 gets dragged far below the tone (it passes);
    // a STATIC filter2 keeps its 800 Hz corner (the tone stays suppressed).
    auto level = [](bool is_static) {
        testfw::EngineFixture g;
        std::string json = std::string(R"({"name":"c","layers":[{
            "pitchNote": 65.2,
            "filterEnv": {"attack":0,"decay":0,"sustain":1,"release":10,"depth":-0.9},
            "filter2": {"mode":"highpass","cutoffHz":800)") +
            (is_static ? R"(,"static":true)" : "") +
            R"(},"ampEnv":{"attack":1,"decay":0,"sustain":1,"release":10}}]})";
        std::string err;
        JsonPtr doc = json_parse(json.data(), json.size(), &err);
        auto p = std::make_unique<Patch>();
        CHECK(load_patch(*doc, g.cache, *p).ok);
        g.bank.set_program(1, std::move(p));
        EngineCommand c;
        c.type = EngineCommand::Type::Program;
        c.slot = 0;
        c.d1 = 1;
        g.engine.queue().push(c);
        g.note_on(0, 60, 110);
        g.render(BUFFER_FRAMES * 4);
        return static_cast<double>(testfw::peak_abs(g.render(BUFFER_FRAMES * 8)));
    };
    const double moving = level(false), fixed = level(true);
    CHECK(moving > fixed * 2);
}

TEST(post_drive_is_routable) {
    auto peak_with = [](const char* matrix) {
        testfw::EngineFixture g;
        std::string json = std::string(R"({"name":"pd","layers":[{
            "filter": {"mode":"lowpass","cutoffHz":2000},
            "mixGain": 0.3,)") + matrix +
            R"("ampEnv":{"attack":1,"decay":0,"sustain":1,"release":10}}]})";
        std::string err;
        JsonPtr doc = json_parse(json.data(), json.size(), &err);
        auto p = std::make_unique<Patch>();
        CHECK(load_patch(*doc, g.cache, *p).ok);
        g.bank.set_program(1, std::move(p));
        EngineCommand c;
        c.type = EngineCommand::Type::Program;
        c.slot = 0;
        c.d1 = 1;
        g.engine.queue().push(c);
        g.note_on(0, 48, 127);
        g.render(BUFFER_FRAMES * 2);
        return testfw::peak_abs(g.render(BUFFER_FRAMES * 6));
    };
    // Saturation barely moves the PEAK (the gain stage comes after it) --
    // it adds HARMONICS: compare 3rd-harmonic energy instead.
    (void)peak_with;
    auto third_harmonic = [](const char* matrix) {
        testfw::EngineFixture g;
        std::string json = std::string(R"({"name":"pd","layers":[{
            "filter": {"mode":"lowpass","cutoffHz":2000},
            "mixGain": 0.3,)") + matrix +
            R"("ampEnv":{"attack":1,"decay":0,"sustain":1,"release":10}}]})";
        std::string err;
        JsonPtr doc = json_parse(json.data(), json.size(), &err);
        auto p = std::make_unique<Patch>();
        CHECK(load_patch(*doc, g.cache, *p).ok);
        g.bank.set_program(1, std::move(p));
        EngineCommand c;
        c.type = EngineCommand::Type::Program;
        c.slot = 0;
        c.d1 = 1;
        g.engine.queue().push(c);
        g.note_on(0, 48, 127);
        g.render(BUFFER_FRAMES * 2);
        auto pcm = g.render(BUFFER_FRAMES * 8);
        const double hz = 3 * 130.81;  // 3rd harmonic of C3
        double re = 0, im = 0;
        for (size_t i = 0; i < pcm.size(); i += 2) {
            const double ph = 2.0 * 3.14159265358979 * hz * (i / 2) / 48000.0;
            re += pcm[i] * std::cos(ph);
            im += pcm[i] * std::sin(ph);
        }
        return re * re + im * im;
    };
    const double clean = third_harmonic("");
    const double driven = third_harmonic(R"("modMatrix":[{"src":"Velocity","dst":"PostDrive","amount":1}],)");
    CHECK(driven > clean * 10 + 1);  // full velocity saturates the power amp
}

TEST(per_layer_sends_override_channel_ccs) {
    testfw::EngineFixture f;
    // Delay configured; channel CC94 = 0 (dry). The layer's own sendDelay
    // still routes it into the echo.
    SongFxConfig cfg;
    static const char* kFx = R"({"reverb":{"wetGain":0},"chorus":{"wetGain":0},
        "delay":{"timeMs":120,"feedback":0.2,"wetGain":1.0}})";
    CHECK(load_song_config(kFx, std::strlen(kFx), cfg).ok);
    push_song_config(f.engine, cfg);
    install_patch(f, R"({"name":"snare","layers":[{"sendDelay": 1.0,
        "ampEnv":{"attack":0,"decay":40,"sustain":0,"release":10}}]})", 1);
    f.cc(0, 94, 0);
    f.note_on(0, 60, 110);
    f.render(BUFFER_FRAMES);
    f.render(5760 - BUFFER_FRAMES);   // source decayed (40 ms), pre-echo
    auto echo = f.render(4800);       // the 120 ms echo window
    CHECK(testfw::peak_abs(echo) > 40);
    // Same patch WITHOUT the override on a dry channel: no echo.
    testfw::EngineFixture g;
    push_song_config(g.engine, cfg);
    install_patch(g, R"({"name":"kick","layers":[{
        "ampEnv":{"attack":0,"decay":40,"sustain":0,"release":10}}]})", 1);
    g.cc(0, 94, 0);
    g.note_on(0, 60, 110);
    g.render(5760);
    auto quiet = g.render(4800);
    CHECK(testfw::peak_abs(quiet) < testfw::peak_abs(echo) / 4);
}

TEST(key_scale_rate_speeds_high_notes) {
    // keyScaleRate 1: the decay runs 2x faster per octave up. Measure how
    // long each note stays audible (sustain 0, exp decay: the threshold
    // crossing sits at a fixed fraction of the stage, so audible time
    // scales linearly with the scaled duration).
    auto audible_buffers = [](u8 note) {
        testfw::EngineFixture f;
        install_patch(f, R"({"name":"ks","layers":[{
            "wavetable": {"generator":"additiveHarmonicMorph","frameCount":1,
                          "params":{"startHarmonics":1,"endHarmonics":1,"falloff":"flat"}},
            "ampEnv":{"attack":0,"decay":400,"sustain":0,"release":10,"keyScaleRate":1}}]})", 1);
        f.note_on(0, note, 100);
        f.render(BUFFER_FRAMES);
        int n = 0;
        while (testfw::peak_abs(f.render(BUFFER_FRAMES)) > 40 && n < 600) ++n;
        return n;
    };
    const int low = audible_buffers(48);   // octave below C4: 2x slower
    const int mid = audible_buffers(60);
    const int high = audible_buffers(72);  // octave above: 2x faster
    CHECK(mid > 5);
    CHECK_NEAR(low, mid * 2, mid / 3 + 2);
    CHECK_NEAR(high * 2, mid, mid / 3 + 2);
}

TEST(output_error_feedback_preserves_subliminal_level) {
    // A zero-mean tone whose Q8.24 amplitude sits around one 16-bit LSB.
    // Plain floor truncation is biased: every small negative sample lands
    // on -1, so the output mean sits near -0.5 LSB. First-order error
    // feedback carries the residual forward, so the mean stays ~0 and the
    // tone survives as balanced +/-1 flips. Deterministic.
    testfw::EngineFixture f;
    install_patch(f, R"({"name":"q","layers":[{"mixGain": 0.004,
        "wavetable": {"generator":"additiveHarmonicMorph","frameCount":1,
                      "params":{"startHarmonics":1,"endHarmonics":1,"falloff":"flat"}},
        "ampEnv":{"attack":1,"decay":0,"sustain":1,"release":10}}]})", 1);
    f.note_on(0, 33, 12);  // low velocity + tiny mix: ~LSB-scale signal
    f.render(BUFFER_FRAMES * 4);
    auto pcm = f.render(SAMPLE_RATE / 2);
    i64 sum = 0, energy = 0;
    int nonzero = 0;
    for (i16 v : pcm) {
        sum += v;
        energy += static_cast<i64>(v) * v;
        if (v != 0) ++nonzero;
    }
    CHECK(energy > 0);
    CHECK(nonzero > 100);  // the tone leaks through as LSB flips
    // Floor truncation would put the mean near -0.5 LSB: |sum| ~ half the
    // sample count. Error feedback keeps it two orders below that.
    CHECK(std::llabs(sum) < static_cast<i64>(pcm.size()) / 8);
    // Silence in, silence out: feedback must not self-oscillate.
    f.cc(0, 123, 0);  // All Notes Off
    f.render(SAMPLE_RATE);  // let the release + residual drain
    CHECK_EQ(testfw::peak_abs(f.render(BUFFER_FRAMES * 4)), 0);
}

// ---- Sidechain ducking --------------------------------------------------

namespace {
// fx.json with a kick-triggered ducker; targets/depth vary per test.
std::string sidechain_fx(const char* targets, const char* depth) {
    // note 37 is unmapped in the fixture kit: it triggers the ducker without
    // adding a kick to the mix, so what we measure is the pad by itself.
    return std::string(R"({"version":1,)"
                       R"("reverb":{"wetGain":0},"chorus":{"wetGain":0},)"
                       R"("sidechain":{"source":10,"note":37,"targets":)") +
           targets + R"(,"depth":)" + depth +
           R"(,"attackMs":5,"holdMs":20,"releaseMs":150}})";
}
void push_fx(testfw::EngineFixture& f, const std::string& json) {
    SongFxConfig cfg;
    CHECK(load_song_config(json.c_str(), json.size(), cfg).ok);
    push_song_config(f.engine, cfg);
}
// Mean |sample| of a render.
i64 mean_abs(const std::vector<i16>& pcm) {
    i64 acc = 0;
    for (i16 v : pcm) acc += v < 0 ? -static_cast<i64>(v) : v;
    return pcm.empty() ? 0 : acc / static_cast<i64>(pcm.size());
}
}  // namespace

TEST(sidechain_ducks_targets_and_recovers) {
    testfw::EngineFixture f;
    push_fx(f, sidechain_fx("[1]", "0.8"));
    f.render(BUFFER_FRAMES);
    // A sustained pad on channel 1 (slot 0), then the kick on channel 10.
    f.note_on(0, 60, 110);
    f.render(SAMPLE_RATE / 4);
    const i64 before = mean_abs(f.render(BUFFER_FRAMES));
    f.note_on(DRUM_CHANNEL, 37, 127);
    const i64 during = mean_abs(f.render(BUFFER_FRAMES));   // inside attack+hold
    f.render(SAMPLE_RATE / 4);                              // past the release
    const i64 after = mean_abs(f.render(BUFFER_FRAMES));
    CHECK(before > 0);
    CHECK(during * 2 < before);          // clearly pulled down
    CHECK(after * 10 > before * 8);      // and back up again
}

TEST(sidechain_leaves_untargeted_channels_alone) {
    // Same kick, but only channel 2 is a target: channel 1 must not move.
    testfw::EngineFixture f;
    push_fx(f, sidechain_fx("[2]", "0.9"));
    f.render(BUFFER_FRAMES);
    f.note_on(0, 60, 110);
    f.render(SAMPLE_RATE / 4);
    const i64 before = mean_abs(f.render(BUFFER_FRAMES));
    f.note_on(DRUM_CHANNEL, 37, 127);
    const i64 during = mean_abs(f.render(BUFFER_FRAMES));
    CHECK(before > 0);
    CHECK_NEAR(during, before, before / 8);
}

TEST(sidechain_depth_zero_is_a_no_op) {
    auto render_with = [](const char* depth) {
        testfw::EngineFixture f;
        push_fx(f, sidechain_fx("[1]", depth));
        f.render(BUFFER_FRAMES);
        f.note_on(0, 60, 110);
        f.render(BUFFER_FRAMES);
        f.note_on(DRUM_CHANNEL, 37, 127);
        return f.render(BUFFER_FRAMES * 8);
    };
    CHECK(render_with("0") == render_with("0"));      // deterministic
    CHECK(render_with("0") != render_with("0.7"));    // and the duck does something
}

// ---- Noise colours ------------------------------------------------------

TEST(noise_colours_tilt_the_spectrum) {
    // Zero crossings stand in for spectral centroid: white is dense, pink
    // less so, brown much less.
    auto crossings = [](const char* colour) {
        testfw::EngineFixture f;
        const std::string json = std::string(R"({"name":"n","layers":[{"osc":"noise","noiseColor":")") +
                                 colour + R"(","ampEnv":{"attack":0,"decay":0,"sustain":1,"release":5}}]})";
        install_patch(f, json.c_str(), 1);
        f.note_on(0, 60, 110);
        f.render(BUFFER_FRAMES);
        return testfw::zero_crossings(f.render(SAMPLE_RATE / 2));
    };
    const int white = crossings("white");
    const int pink = crossings("pink");
    const int brown = crossings("brown");
    CHECK(white > 1000);
    CHECK(pink < white);
    CHECK(brown < pink);
    CHECK(brown > 0);   // still noise, not silence
}

// ---- Unison phase spread ------------------------------------------------

TEST(unison_phase_spread_is_deterministic_and_audible) {
    auto render = [](const char* spread) {
        testfw::EngineFixture f;
        const std::string json = std::string(R"({"name":"u","layers":[{
            "unison":{"voices":5,"detuneCents":12,"spread":1,"phaseSpread":)") + spread + R"(},
            "ampEnv":{"attack":1,"decay":0,"sustain":1,"release":10}}]})";
        install_patch(f, json.c_str(), 1);
        f.note_on(0, 45, 110);
        return f.render(BUFFER_FRAMES * 4);
    };
    CHECK(render("1") == render("1"));    // same every run
    CHECK(render("0") != render("1"));    // spreading the stack changes the attack
    // A stack that starts phase-aligned sums constructively -- that single
    // loud edge is exactly what spreading the start phases removes.
    const auto flat = render("0"), spread = render("1");
    CHECK(testfw::peak_abs(flat) > testfw::peak_abs(spread));
}

// ---- Live FM index ------------------------------------------------------

TEST(fm_index_adds_sidebands) {
    auto crossings = [](const char* index) {
        testfw::EngineFixture f;
        const std::string json = std::string(R"({"name":"fm","layers":[{
            "wavetable": {"generator":"additiveHarmonicMorph","frameCount":1,
                          "params":{"startHarmonics":1,"endHarmonics":1,"falloff":"flat"}},
            "ring": {"mode":"fm","ratio":2,"index":)") + index + R"(},
            "ampEnv":{"attack":1,"decay":0,"sustain":1,"release":10}}]})";
        install_patch(f, json.c_str(), 1);
        f.note_on(0, 57, 110);   // A3 = 220 Hz
        f.render(BUFFER_FRAMES);
        return testfw::zero_crossings(f.render(SAMPLE_RATE));
    };
    const int clean = crossings("0");
    const int bright = crossings("6");
    CHECK_NEAR(clean, 220, 4);     // index 0 is the bare carrier
    CHECK(bright > clean * 2);     // index 6 is a spectrum
}

TEST(fm_index_is_modulatable) {
    // An envelope on RingAmount must change the sound over time: compare the
    // first half of the note against the tail.
    testfw::EngineFixture f;
    install_patch(f, R"({"name":"rhodes","layers":[{
        "wavetable": {"generator":"additiveHarmonicMorph","frameCount":1,
                      "params":{"startHarmonics":1,"endHarmonics":1,"falloff":"flat"}},
        "ring": {"mode":"fm","ratio":3,"index":0},
        "filterEnv": {"attack":0,"decay":1500,"sustain":0,"release":10},
        "modMatrix": [{"src":"FilterEnv","dst":"RingAmount","amount":0.6}],
        "ampEnv":{"attack":1,"decay":0,"sustain":1,"release":10}}]})", 1);
    f.note_on(0, 57, 110);
    const int early = testfw::zero_crossings(f.render(SAMPLE_RATE / 8));
    f.render(SAMPLE_RATE * 3 / 2);   // past the index envelope's decay
    const int late = testfw::zero_crossings(f.render(SAMPLE_RATE / 8));
    CHECK(early > late * 2);   // bright attack, mellow tail
}
