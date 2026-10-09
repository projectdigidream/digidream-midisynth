#include "test_framework.h"

#include "synth/lfo.h"
#include "synth/mod_matrix.h"

using namespace msynth;

namespace {
LfoParams make_lfo(LfoShape shape, u32 inc, u32 delay = 0) {
    LfoParams p;
    p.shape = shape;
    p.inc_per_block = inc;
    p.delay_blocks = delay;
    return p;
}
}  // namespace

TEST(lfo_shapes_stay_in_range) {
    for (auto shape : {LfoShape::Sine, LfoShape::Triangle, LfoShape::Saw, LfoShape::Square,
                       LfoShape::SampleHold}) {
        LfoParams p = make_lfo(shape, 0x03000000u);
        LfoState s;
        s.note_on(p, 42);
        for (int i = 0; i < 1000; ++i) {
            const i32 v = s.advance_block(p);
            CHECK(v >= -GAIN_ONE && v <= GAIN_ONE);
        }
    }
}

TEST(lfo_sine_full_swing) {
    LfoParams p = make_lfo(LfoShape::Sine, 1u << 26);  // 64 blocks per cycle
    LfoState s;
    s.note_on(p, 1);
    i32 mn = GAIN_ONE, mx = -GAIN_ONE;
    for (int i = 0; i < 64; ++i) {
        const i32 v = s.advance_block(p);
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    CHECK(mx > GAIN_ONE - GAIN_ONE / 50);
    CHECK(mn < -GAIN_ONE + GAIN_ONE / 50);
}

TEST(lfo_triangle_shape) {
    LfoParams p = make_lfo(LfoShape::Triangle, 1u << 26);
    LfoState s;
    s.note_on(p, 1);
    // Rises for the first quarter cycle.
    i32 prev = 0;
    for (int i = 0; i < 15; ++i) {
        const i32 v = s.advance_block(p);
        CHECK(v >= prev);
        prev = v;
    }
}

TEST(lfo_square_two_levels) {
    LfoParams p = make_lfo(LfoShape::Square, 1u << 26);
    LfoState s;
    s.note_on(p, 1);
    for (int i = 0; i < 200; ++i) {
        const i32 v = s.advance_block(p);
        CHECK(v == GAIN_ONE || v == -GAIN_ONE);
    }
}

TEST(lfo_delay_holds_zero) {
    LfoParams p = make_lfo(LfoShape::Sine, 1u << 26, /*delay=*/10);
    LfoState s;
    s.note_on(p, 1);
    for (int i = 0; i < 10; ++i) CHECK_EQ(s.advance_block(p), 0);
    bool nonzero = false;
    for (int i = 0; i < 30; ++i)
        if (s.advance_block(p) != 0) nonzero = true;
    CHECK(nonzero);
}

TEST(lfo_sample_hold_deterministic_per_seed) {
    LfoParams p = make_lfo(LfoShape::SampleHold, 1u << 28);
    LfoState a, b, c;
    a.note_on(p, 42);
    b.note_on(p, 42);
    c.note_on(p, 43);
    bool differs = false;
    for (int i = 0; i < 100; ++i) {
        const i32 va = a.advance_block(p);
        CHECK_EQ(va, b.advance_block(p));
        if (va != c.advance_block(p)) differs = true;
    }
    CHECK(differs);
}

TEST(lfo_sample_hold_holds_between_wraps) {
    LfoParams p = make_lfo(LfoShape::SampleHold, 1u << 26);  // wraps every 64 blocks
    LfoState s;
    s.note_on(p, 7);
    const i32 first = s.advance_block(p);
    for (int i = 0; i < 60; ++i) CHECK_EQ(s.advance_block(p), first);
}

TEST(mod_matrix_accumulates_routings) {
    ModValues m;
    m.src[static_cast<int>(ModSource::LFO1)] = GAIN_ONE / 2;
    m.src[static_cast<int>(ModSource::Velocity)] = GAIN_ONE;
    ModRouting r[3];
    r[0] = {ModSource::LFO1, ModDest::Pitch, GAIN_ONE / 4};
    r[1] = {ModSource::Velocity, ModDest::Pitch, GAIN_ONE / 8};
    r[2] = {ModSource::LFO1, ModDest::Pan, -GAIN_ONE};
    m.evaluate(r, 3);
    // Pitch: 0.5*0.25 + 1.0*0.125 = 0.25
    CHECK_EQ(m.dst[static_cast<int>(ModDest::Pitch)], GAIN_ONE / 4);
    CHECK_EQ(m.dst[static_cast<int>(ModDest::Pan)], -GAIN_ONE / 2);
    CHECK_EQ(m.dst[static_cast<int>(ModDest::Amplitude)], 0);
}

TEST(mod_matrix_reevaluation_resets) {
    ModValues m;
    m.src[static_cast<int>(ModSource::LFO1)] = GAIN_ONE;
    ModRouting r{ModSource::LFO1, ModDest::FilterCutoff, GAIN_ONE};
    m.evaluate(&r, 1);
    m.evaluate(&r, 1);  // must not double-accumulate
    CHECK_EQ(m.dst[static_cast<int>(ModDest::FilterCutoff)], GAIN_ONE);
}

TEST(mod_matrix_negative_amounts) {
    ModValues m;
    m.src[static_cast<int>(ModSource::ModWheel)] = GAIN_ONE / 2;
    ModRouting r{ModSource::ModWheel, ModDest::Amplitude, -GAIN_ONE};
    m.evaluate(&r, 1);
    CHECK_EQ(m.dst[static_cast<int>(ModDest::Amplitude)], -GAIN_ONE / 2);
}

TEST(lfo_tempo_sync_follows_bpm) {
    LfoParams p;
    p.shape = LfoShape::Saw;
    p.sync_beats_q16 = GAIN_ONE;  // one beat per cycle
    LfoState s;
    s.note_on(p, 1);
    // At 120 BPM one beat = 0.5 s = 375 blocks: the saw wraps ~twice/sec.
    int wraps = 0;
    i32 prev = -GAIN_ONE;
    for (int i = 0; i < 750; ++i) {
        const i32 v = s.advance_block(p, 120 << GAIN_SHIFT);
        if (v < prev - GAIN_ONE) ++wraps;
        prev = v;
    }
    CHECK_NEAR(wraps, 2, 1);
    // Double tempo -> double rate.
    LfoState f;
    f.note_on(p, 1);
    wraps = 0;
    prev = -GAIN_ONE;
    for (int i = 0; i < 750; ++i) {
        const i32 v = f.advance_block(p, 240 << GAIN_SHIFT);
        if (v < prev - GAIN_ONE) ++wraps;
        prev = v;
    }
    CHECK_NEAR(wraps, 4, 1);
}

TEST(lfo_free_run_keeps_its_phase_across_notes) {
    LfoParams p;
    p.shape = LfoShape::Saw;
    p.inc_per_block = 1u << 24;   // slow sweep
    p.retrigger = false;

    // Two voices started 100 blocks apart must be 100 blocks apart in phase,
    // not both back at zero.
    LfoState a, b;
    a.note_on(p, 1, 0);
    b.note_on(p, 2, 100);
    CHECK_EQ(a.phase, 0u);
    CHECK_EQ(b.phase, 100u * (1u << 24));

    // ...and the default still restarts every note (goldens depend on it).
    LfoParams r = p;
    r.retrigger = true;
    LfoState c;
    c.note_on(r, 3, 100);
    CHECK_EQ(c.phase, 0u);
}

TEST(lfo_free_run_uses_the_synced_rate) {
    // A tempo-synced free-running LFO must place itself with the SYNCED
    // increment, not the unused rateHz one.
    LfoParams p;
    p.shape = LfoShape::Sine;
    p.inc_per_block = 12345;                 // must be ignored
    p.sync_beats_q16 = 4 * GAIN_ONE;         // one bar at 4/4
    p.retrigger = false;
    LfoState s;
    s.note_on(p, 1, 50, 174 << GAIN_SHIFT);
    CHECK_EQ(s.phase, static_cast<u32>(50ull * lfo_sync_inc(p.sync_beats_q16, 174 << GAIN_SHIFT)));
}
