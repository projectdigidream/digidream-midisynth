#include "test_framework.h"

#include "synth/tables.h"

using namespace msynth;

TEST(sine_table_key_points) {
    const i32* s = sine_table_4096();
    CHECK_EQ(s[0], 0);
    CHECK_EQ(s[1024], COEF_ONE);  // sin(90 deg) = 1.0 exactly at Q30
    CHECK_EQ(s[2048], 0);
    CHECK_EQ(s[3072], -COEF_ONE);
    // Quarter symmetry.
    for (int i = 0; i < 1024; i += 37) CHECK_EQ(s[i], s[2048 - i]);
    for (int i = 1; i < 2048; i += 61) CHECK_NEAR(s[2048 + i], -s[i], 1);
}

TEST(sine_q30_interpolates) {
    // Exactly on entries: matches the table.
    const i32* s = sine_table_4096();
    CHECK_EQ(sine_q30(0), s[0]);
    CHECK_EQ(sine_q30(1u << 20), s[1]);
    // Halfway between entries 0 and 1: midpoint.
    CHECK_NEAR(sine_q30(1u << 19), s[1] / 2, 2);
}

TEST(pitch_a440_accurate) {
    // MIDI note 69 = 440 Hz -> phase_inc = 440/48000 * 2^32 = 39370533.5
    const u32 inc = phase_inc_for_cents((69 * 100) << GAIN_SHIFT);
    CHECK_NEAR(inc, 39370533, 40);  // within ~1 ppm
}

TEST(pitch_octave_doubles) {
    for (int note = 12; note <= 108; note += 12) {
        const u32 lo = phase_inc_for_cents((note * 100) << GAIN_SHIFT);
        const u32 hi = phase_inc_for_cents(((note + 12) * 100) << GAIN_SHIFT);
        CHECK_NEAR(hi, 2 * lo, 8);
    }
}

TEST(pitch_monotonic_across_cents) {
    u32 prev = 0;
    for (i32 c = 0; c < 12700; c += 13) {
        const u32 inc = phase_inc_for_cents(c << GAIN_SHIFT);
        CHECK(inc > prev);
        prev = inc;
    }
}

TEST(pitch_fractional_cents_between_neighbors) {
    const i32 base = (6900 << GAIN_SHIFT);
    const u32 a = phase_inc_for_cents(base);
    const u32 b = phase_inc_for_cents(base + GAIN_ONE);
    const u32 mid = phase_inc_for_cents(base + GAIN_ONE / 2);
    CHECK(mid > a && mid < b);
}

TEST(pitch_clamps_out_of_range) {
    CHECK_EQ(phase_inc_for_cents(-(100 << GAIN_SHIFT)), phase_inc_for_cents(0));
    CHECK(phase_inc_for_cents(0x7FFFFFFF) > 0);
}

TEST(velocity_curve_squared) {
    CHECK_EQ(VEL_GAIN_Q16[0], 0);
    CHECK_EQ(VEL_GAIN_Q16[127], GAIN_ONE);
    // Squared: v=64 -> (64/127)^2 = 0.254
    CHECK_NEAR(VEL_GAIN_Q16[64], 16644, 20);
    for (int v = 1; v < 128; ++v) CHECK(VEL_GAIN_Q16[v] > VEL_GAIN_Q16[v - 1]);
}

TEST(pan_constant_power) {
    gain_t l, r;
    pan_gains(0, l, r);
    // Center: both ~= 0.7071.
    CHECK_NEAR(l, 46341, 40);
    CHECK_NEAR(r, 46341, 40);
    pan_gains(-GAIN_ONE, l, r);
    CHECK_EQ(l, GAIN_ONE);
    CHECK_EQ(r, 0);
    pan_gains(GAIN_ONE, l, r);
    CHECK_EQ(l, 0);
    CHECK_EQ(r, GAIN_ONE);
    // l^2 + r^2 ~= 1 across the range.
    for (i32 p = -GAIN_ONE; p <= GAIN_ONE; p += GAIN_ONE / 8) {
        pan_gains(p, l, r);
        const i64 pow2 = (static_cast<i64>(l) * l + static_cast<i64>(r) * r) >> GAIN_SHIFT;
        CHECK_NEAR(pow2, GAIN_ONE, 80);
    }
}

TEST(curve_tables_valid) {
    for (auto id : {CurveId::Linear, CurveId::Exp, CurveId::ExpAttack, CurveId::SCurve}) {
        const gain_t* c = curve_table(id);
        CHECK_EQ(c[0], 0);
        CHECK_EQ(c[256], GAIN_ONE);
        for (int i = 1; i <= 256; ++i) CHECK(c[i] >= c[i - 1]);
    }
}

TEST(curve_eval_endpoints_and_interp) {
    const gain_t* lin = curve_table(CurveId::Linear);
    CHECK_EQ(curve_eval(lin, 0), 0);
    CHECK_EQ(curve_eval(lin, 1u << 24), GAIN_ONE);
    CHECK_NEAR(curve_eval(lin, 1u << 23), GAIN_ONE / 2, 2);
    // Exp curve is above linear mid-phase (fast start).
    const gain_t* exp = curve_table(CurveId::Exp);
    CHECK(curve_eval(exp, 1u << 23) > curve_eval(lin, 1u << 23));
    // ExpAttack is below linear mid-phase (slow start).
    const gain_t* ea = curve_table(CurveId::ExpAttack);
    CHECK(curve_eval(ea, 1u << 23) < curve_eval(lin, 1u << 23));
}

TEST(log2_q16_known_values) {
    CHECK_NEAR(log2_q16(1u << GAIN_SHIFT), 0, 4);           // log2(1) = 0
    CHECK_NEAR(log2_q16(2u << GAIN_SHIFT), GAIN_ONE, 4);    // log2(2) = 1
    CHECK_NEAR(log2_q16(1024u << GAIN_SHIFT), 10 * GAIN_ONE, 4);
    CHECK_NEAR(log2_q16(3u << GAIN_SHIFT), 103872, 300);    // log2(3) = 1.585
}

TEST(cutoff_index_roundtrip) {
    CHECK_EQ(cutoff_index_from_hz(20 << GAIN_SHIFT), 0);
    CHECK_NEAR(cutoff_index_from_hz(12000 << GAIN_SHIFT), 256 << GAIN_SHIFT, 3000);
    // Monotonic.
    i32 prev = -1;
    for (i32 hz = 20; hz <= 12000; hz += 100) {
        const i32 idx = cutoff_index_from_hz(hz << GAIN_SHIFT);
        CHECK(idx >= prev);
        prev = idx;
    }
}

TEST(cutoff_g_monotonic_and_capped) {
    coef_t prev = 0;
    for (i32 i = 0; i <= 256; ++i) {
        const coef_t g = cutoff_g(i << GAIN_SHIFT);
        CHECK(g >= prev);
        prev = g;
    }
    // g at fs/4 cap = tan(pi/4) = 1.0.
    CHECK_NEAR(cutoff_g(256 << GAIN_SHIFT), COEF_ONE, 1 << 12);
    // Out-of-range clamps.
    CHECK_EQ(cutoff_g(400 << GAIN_SHIFT), cutoff_g(256 << GAIN_SHIFT));
    CHECK_EQ(cutoff_g(-5), cutoff_g(0));
}

TEST(resonance_k_range) {
    CHECK_EQ(resonance_k(0), 0x7FFFFFFF);  // k=2.0 clamped to Q2.30 max
    // res=0.5 -> k=1.0
    CHECK_EQ(resonance_k(GAIN_ONE / 2), COEF_ONE);
    // Cap below self-oscillation: res=1.0 treated as 0.95 -> k=0.1.
    CHECK_EQ(resonance_k(GAIN_ONE), resonance_k((95 * GAIN_ONE) / 100));
    CHECK(resonance_k(GAIN_ONE) > 0);
}

TEST(exp2_and_db_helpers) {
    CHECK_EQ(exp2_q16(0), GAIN_ONE);
    CHECK_EQ(exp2_q16(1 << GAIN_SHIFT), 2 * GAIN_ONE);
    CHECK_EQ(exp2_q16(-(1 << GAIN_SHIFT)), GAIN_ONE / 2);
    CHECK_NEAR(exp2_q16(GAIN_ONE / 2), 92682, 3);           // sqrt(2)
    CHECK_NEAR(exp2_q16(10 << GAIN_SHIFT), 1024 * GAIN_ONE, 2);
    CHECK_EQ(db_to_lin_q16(0), GAIN_ONE);
    CHECK_NEAR(db_to_lin_q16(6 * GAIN_ONE + 1350), 2 * GAIN_ONE, 40);  // +6.0206 dB
    CHECK_NEAR(db_to_lin_q16(-20 * GAIN_ONE), GAIN_ONE / 10, 10);
    CHECK_NEAR(db_to_lin_q16(-6 * GAIN_ONE), 32860, 40);
    // Monotonic across a wide range.
    gain_t prev = 0;
    for (i32 db = -60; db <= 24; ++db) {
        const gain_t v = db_to_lin_q16(db << GAIN_SHIFT);
        CHECK(v > prev);
        prev = v;
    }
    CHECK_NEAR(lin_to_db_q16(GAIN_ONE), 0, 20);
    CHECK_NEAR(lin_to_db_q16(2 * GAIN_ONE), 394565, 400);   // +6.02 dB
    CHECK_NEAR(lin_to_db_q16(GAIN_ONE / 2), -394565, 400);
    // One-pole coefficient: 0 ms = instant; longer = smaller; ~63% after tau.
    CHECK_EQ(onepole_coef_from_ms(0), GAIN_ONE);
    const gain_t a10 = onepole_coef_from_ms(10 << GAIN_SHIFT);
    const gain_t a100 = onepole_coef_from_ms(100 << GAIN_SHIFT);
    CHECK(a10 > a100);
    CHECK(a100 > 0);
    // Apply a100 for 75 blocks (=100 ms): value reaches ~1 - 1/e = 0.632.
    i64 v = 0;
    for (int i = 0; i < 75; ++i) v += asr((GAIN_ONE - v) * a100, GAIN_SHIFT);
    CHECK_NEAR(v, 41427, 1500);
}
