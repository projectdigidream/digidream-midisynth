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
