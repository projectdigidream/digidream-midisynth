//
// Trapezoidal SVF: stability across the cutoff range, under extreme
// resonance, and under fast modulation; saturation bounds (Appendix B/D).
//
#include "test_framework.h"

#include "synth/filter.h"

using namespace msynth;

namespace {
FilterCoefs coefs_for(i32 cutoff_index, gain_t res) {
    FilterCoefs c;
    c.compute(cutoff_g(cutoff_index << GAIN_SHIFT), resonance_k(res));
    return c;
}
}  // namespace

TEST(filter_lowpass_passes_dc) {
    FilterState f;
    const FilterCoefs c = coefs_for(128, 0);
    f.reset(c);
    const i32 x = COEF_ONE / 2;
    i32 y = 0;
    for (int i = 0; i < 20000; ++i) y = f.process(x, FilterMode::Lowpass);
    CHECK_NEAR(y, x, COEF_ONE / 100);  // DC gain ~= 1
}

TEST(filter_highpass_blocks_dc) {
    FilterState f;
    const FilterCoefs c = coefs_for(128, 0);
    f.reset(c);
    const i32 x = COEF_ONE / 2;
    i32 y = 0;
    for (int i = 0; i < 20000; ++i) y = f.process(x, FilterMode::Highpass);
    CHECK_NEAR(y, 0, COEF_ONE / 100);
}

TEST(filter_bandpass_blocks_dc) {
    FilterState f;
    f.reset(coefs_for(128, 0));
    i32 y = 0;
    for (int i = 0; i < 20000; ++i) y = f.process(COEF_ONE / 2, FilterMode::Bandpass);
    CHECK_NEAR(y, 0, COEF_ONE / 100);
}

TEST(filter_silence_in_silence_out) {
    FilterState f;
    f.reset(coefs_for(200, (90 * GAIN_ONE) / 100));
    for (int i = 0; i < 1000; ++i) CHECK_EQ(f.process(0, FilterMode::Lowpass), 0);
}

TEST(filter_stable_at_extremes) {
    // Full cutoff range x max resonance, driven by a worst-case square wave:
    // states must stay inside the saturation bound, output bounded.
    for (int idx = 0; idx <= 256; idx += 16) {
        FilterState f;
        f.reset(coefs_for(idx, (95 * GAIN_ONE) / 100));
        i64 peak = 0;
        for (int i = 0; i < 48000; ++i) {
            const i32 x = (i / 24) % 2 ? COEF_ONE : -COEF_ONE;
            const i32 y = f.process(x, FilterMode::Lowpass);
            const i64 ay = y < 0 ? -static_cast<i64>(y) : y;
            if (ay > peak) peak = ay;
        }
        // Resonant peak exists but stays bounded: state saturation converts
        // any would-be blowup into bounded distortion (i32 states cannot
        // exceed +/-2.0 in Q2.30 by construction of filt_sat).
        CHECK(peak < 8LL * COEF_ONE);
    }
}

TEST(filter_survives_fast_coefficient_sweeps) {
    // Worst-case modulation: jump cutoff wildly every block under max
    // resonance and noise drive, then verify the filter did not end up in a
    // limit cycle -- with silent input it must ring down to (near) zero.
    FilterState f;
    f.reset(coefs_for(0, (95 * GAIN_ONE) / 100));
    XorShift32 rng(7);
    for (int block = 0; block < 500; ++block) {
        const i32 idx = static_cast<i32>(rng.next() % 257);
        f.set_block_target(coefs_for(idx, (95 * GAIN_ONE) / 100), 6);
        for (int i = 0; i < 64; ++i) {
            const i32 x = static_cast<i32>(asl(rng.next_sample(), 5));  // ~0.5 Q30
            (void)f.process(x, FilterMode::Lowpass);
        }
    }
    // Feed silence, re-targeting each block exactly as a voice does
    // (deltas are per-block: the caller re-targets every CONTROL_BLOCK).
    const FilterCoefs frozen = coefs_for(128, (95 * GAIN_ONE) / 100);
    for (int block = 0; block < SAMPLE_RATE / 64; ++block) {
        f.set_block_target(frozen, 6);
        for (int i = 0; i < 64; ++i) (void)f.process(0, FilterMode::Lowpass);
    }
    i64 residual = 0;
    for (int block = 0; block < 4800 / 64; ++block) {
        f.set_block_target(frozen, 6);
        for (int i = 0; i < 64; ++i) {
            const i32 y = f.process(0, FilterMode::Lowpass);
            residual += y < 0 ? -static_cast<i64>(y) : y;
        }
    }
    // A high-Q fixed-point filter may retain a tiny limit cycle; Q2.30 puts
    // it far below audibility (plan Appendix B), which is what we pin here:
    // average residual under ~2^10 Q30-ulps = below -120 dBFS.
    CHECK(residual < 4800LL * 1024);
}

TEST(filter_saturation_recovers) {
    // Slam the states with a huge impulse; after input stops, output decays
    // instead of oscillating forever or wrapping.
    FilterState f;
    f.reset(coefs_for(64, (95 * GAIN_ONE) / 100));
    for (int i = 0; i < 64; ++i) f.process(0x7FFFFFFF / 2, FilterMode::Lowpass);
    i64 energy_early = 0, energy_late = 0;
    for (int i = 0; i < 4800; ++i) {
        const i32 y = f.process(0, FilterMode::Lowpass);
        const i64 ay = y < 0 ? -static_cast<i64>(y) : y;
        if (i < 480) energy_early += ay;
        if (i >= 4320) energy_late += ay;
    }
    CHECK(energy_late < energy_early / 4 + 1);
}

TEST(filter_sat_helper_bounds) {
    CHECK_EQ(filt_sat(0), 0);
    CHECK_EQ(filt_sat(COEF_ONE), COEF_ONE);
    const i64 lim = (static_cast<i64>(COEF_ONE) << 1) - 1;
    CHECK_EQ(filt_sat(lim + 12345), static_cast<i32>(lim));
    CHECK_EQ(filt_sat(-lim - 12345), static_cast<i32>(-lim));
}

TEST(filter_coefs_match_appendix_math) {
    // g = 1.0 (fs/4), k = 1.0: a1 = 1/(1+g(g+k)) = 1/3, a2 = g*a1 = 1/3.
    FilterCoefs c;
    c.compute(COEF_ONE, COEF_ONE);
    CHECK_NEAR(c.a1, COEF_ONE / 3, 4);
    CHECK_NEAR(c.a2, COEF_ONE / 3, 4);
    // g = 0: a1 = 1, a2 = 0.
    c.compute(0, COEF_ONE);
    CHECK_EQ(c.a1, COEF_ONE);
    CHECK_EQ(c.a2, 0);
}

TEST(filter_lowpass_attenuates_high_freq_more_than_low) {
    // Feed two sines through the same 500Hz-ish lowpass; compare RMS.
    const i32 idx = cutoff_index_from_hz(500 << GAIN_SHIFT) >> GAIN_SHIFT;
    auto rms_for = [&](u32 inc) {
        FilterState f;
        f.reset(coefs_for(idx, 0));
        u32 ph = 0;
        i64 acc = 0;
        for (int i = 0; i < 48000; ++i) {
            const i32 x = sine_q30(ph);
            ph += inc;
            const i32 y = f.process(x, FilterMode::Lowpass);
            acc += static_cast<i64>(y) * y >> 30;
        }
        return acc;
    };
    const i64 low = rms_for(phase_inc_for_cents((3600) << GAIN_SHIFT));   // ~65 Hz
    const i64 high = rms_for(phase_inc_for_cents((10800) << GAIN_SHIFT)); // ~4.2 kHz
    CHECK(low > 8 * high);
}

TEST(filter_notch_rejects_centre_passes_edges) {
    // A sine at the cutoff comes out much weaker than one two octaves below
    // or above it -- the notch is the complement of the bandpass.
    auto rms_after = [](i32 hz_q16, i32 tone_hz) {
        FilterCoefs c;
        c.compute(cutoff_g(cutoff_index_from_hz(hz_q16)), resonance_k(GAIN_ONE / 2));
        FilterState f;
        f.reset(c);
        double acc = 0;
        u32 phase = 0;
        const u32 inc = static_cast<u32>((static_cast<u64>(tone_hz) << 32) / SAMPLE_RATE);
        for (int i = 0; i < 48000; ++i) {
            const i32 x = sine_q30(phase) >> 1;
            phase += inc;
            const i32 y = f.process(x, FilterMode::Notch);
            if (i > 24000) acc += static_cast<double>(y) * y;
        }
        return acc;
    };
    const i32 fc = 1000 << GAIN_SHIFT;
    const double centre = rms_after(fc, 1000), low = rms_after(fc, 250), high = rms_after(fc, 4000);
    CHECK(centre * 20 < low);
    CHECK(centre * 20 < high);
}

TEST(filter_drive_is_identity_at_one_and_saturates_above) {
    for (i32 x : {0, 1000, -1000, COEF_ONE / 2, -COEF_ONE / 2, COEF_ONE - 1})
        CHECK_EQ(drive_q30(x, GAIN_ONE), x);  // exact no-op at drive 1 (goldens)
    // Drive 4 on a half-scale input lands on the soft-clip curve, never past 1.0.
    const i32 y = drive_q30(COEF_ONE / 2, 4 * GAIN_ONE);
    CHECK(y > COEF_ONE / 2);
    CHECK(y <= COEF_ONE);
    CHECK_EQ(drive_q30(COEF_ONE, 8 * GAIN_ONE), COEF_ONE);
    CHECK_EQ(drive_q30(-COEF_ONE, 8 * GAIN_ONE), -COEF_ONE);
    // Odd symmetry (floor rounding allows 1 ulp of asymmetry).
    CHECK_NEAR(drive_q30(-(COEF_ONE / 3), 3 * GAIN_ONE), -drive_q30(COEF_ONE / 3, 3 * GAIN_ONE), 1);
}

TEST(filter_24db_rolls_off_steeper_than_12db) {
    // Two cascaded stages attenuate a tone two octaves above cutoff more than one.
    auto energy = [](bool two_stages) {
        FilterCoefs c;
        c.compute(cutoff_g(cutoff_index_from_hz(500 << GAIN_SHIFT)), resonance_k(0));
        FilterState a, b;
        a.reset(c);
        b.reset(c);
        double acc = 0;
        u32 phase = 0;
        const u32 inc = static_cast<u32>((static_cast<u64>(2000) << 32) / SAMPLE_RATE);
        for (int i = 0; i < 48000; ++i) {
            const i32 x = sine_q30(phase) >> 1;
            phase += inc;
            i32 y = a.process(x, FilterMode::Lowpass);
            if (two_stages) y = b.process(y, FilterMode::Lowpass);
            if (i > 24000) acc += static_cast<double>(y) * y;
        }
        return acc;
    };
    const double one = energy(false), two = energy(true);
    CHECK(two * 4 < one);  // at least ~6 dB more attenuation (ideal: 12 dB more)
}

TEST(cutoff_hz_from_index_inverts_the_mapping) {
    for (i32 hz : {30, 100, 440, 1000, 4000, 11000}) {
        const i32 idx = cutoff_index_from_hz(hz << GAIN_SHIFT);
        const i32 back = cutoff_hz_from_index(idx) >> GAIN_SHIFT;
        CHECK_NEAR(back, hz, hz / 50 + 2);  // 2% (table + exp2 rounding)
    }
    CHECK_EQ(cutoff_hz_from_index(0) >> GAIN_SHIFT, 20);
}
