#include "test_framework.h"

#include "synth/limiter.h"

using namespace msynth;

TEST(asr_rounds_toward_neg_inf) {
    CHECK_EQ(asr(7, 1), 3);
    CHECK_EQ(asr(-7, 1), -4);
    CHECK_EQ(asr(-1, 8), -1);
    CHECK_EQ(asr(1, 8), 0);
}

TEST(asl_handles_negative_values) {
    CHECK_EQ(asl(-3, 4), -48);
    CHECK_EQ(asl(3, 4), 48);
}

TEST(mulshift_matches_wide_math) {
    CHECK_EQ(mulshift(GAIN_ONE, GAIN_ONE, GAIN_SHIFT), GAIN_ONE);
    CHECK_EQ(mulshift(GAIN_ONE / 2, GAIN_ONE / 2, GAIN_SHIFT), GAIN_ONE / 4);
    CHECK_EQ(mulshift(-GAIN_ONE, GAIN_ONE / 2, GAIN_SHIFT), -GAIN_ONE / 2);
}

TEST(lerp16_endpoints_exact) {
    CHECK_EQ(lerp16(100, 200, 0), 100);
    CHECK_EQ(lerp16(100, 200, 0x8000), 150);
    CHECK_EQ(lerp16(-100, 100, 0x8000), 0);
    // At frac 0xFFFF we land 1 ulp short of b; entry i+1 owns the endpoint.
    CHECK_NEAR(lerp16(100, 200, 0xFFFF), 200, 1);
}

TEST(xorshift_deterministic_and_seed_dependent) {
    XorShift32 a(123), b(123), c(124);
    for (int i = 0; i < 100; ++i) {
        const u32 va = a.next();
        CHECK_EQ(va, b.next());
        (void)c.next();
    }
    XorShift32 a2(123), c2(124);
    bool diverged = false;
    for (int i = 0; i < 10; ++i)
        if (a2.next() != c2.next()) diverged = true;
    CHECK(diverged);
    // Zero seed must not lock at zero.
    XorShift32 z(0);
    CHECK(z.next() != 0);
}

TEST(noise_sample_in_audio_range) {
    XorShift32 r(42);
    for (int i = 0; i < 1000; ++i) {
        const i32 s = r.next_sample();
        CHECK(s >= -(1 << 25) && s < (1 << 25));
    }
}

TEST(soft_clip_transparent_below_knee) {
    CHECK_EQ(soft_clip_q24(0), 0);
    // Small signals pass nearly unchanged (cubic term tiny).
    const i64 x = SAMPLE_ONE / 10;
    CHECK_NEAR(soft_clip_q24(x), x, SAMPLE_ONE / 1000);
}

TEST(soft_clip_saturates_smoothly) {
    const i64 lim = (3LL << SAMPLE_SHIFT) / 2;
    CHECK_EQ(soft_clip_q24(lim), SAMPLE_ONE);
    CHECK_EQ(soft_clip_q24(-lim), -SAMPLE_ONE);
    CHECK_EQ(soft_clip_q24(100 * SAMPLE_ONE), SAMPLE_ONE);
    CHECK_EQ(soft_clip_q24(-100 * SAMPLE_ONE), -SAMPLE_ONE);
    // Monotonic through the knee.
    i32 prev = soft_clip_q24(0);
    for (i64 v = 0; v <= lim; v += SAMPLE_ONE / 64) {
        const i32 y = soft_clip_q24(v);
        CHECK(y >= prev);
        prev = y;
    }
}

TEST(output_stage_dither_deterministic) {
    OutputStage a, b;
    a.dither = b.dither = true;
    for (int i = 0; i < 256; ++i)
        CHECK_EQ(a.convert(i * 1000), b.convert(i * 1000));
}

TEST(output_stage_duck_ramps_to_target) {
    OutputStage s;
    s.set_duck(GAIN_ONE / 2, 4);
    for (int i = 0; i < 10; ++i) s.advance_duck_block();
    CHECK_EQ(s.duck_cur, GAIN_ONE / 2);
    s.set_duck(GAIN_ONE, 4);
    for (int i = 0; i < 10; ++i) s.advance_duck_block();
    CHECK_EQ(s.duck_cur, GAIN_ONE);
}

TEST(clamp_gain01_bounds) {
    CHECK_EQ(clamp_gain01(-5), 0);
    CHECK_EQ(clamp_gain01(GAIN_ONE + 5), GAIN_ONE);
    CHECK_EQ(clamp_gain01(1234), 1234);
}
