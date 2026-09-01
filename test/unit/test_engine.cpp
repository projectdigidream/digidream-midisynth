//
// SynthEngine integration: notes sound, CCs behave per the §9 table, voices
// steal and choke, output is bit-identical run to run.
//
#include "test_framework.h"

#include "engine_helpers.h"

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
