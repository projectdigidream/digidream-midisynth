#include "test_framework.h"

#include "synth/effects/chorus.h"
#include "synth/effects/delay_line.h"
#include "synth/effects/reverb.h"

using namespace msynth;

TEST(delay_line_basic) {
    DelayLine<4> d;  // 16 samples
    for (int i = 1; i <= 16; ++i) d.push(i * 100);
    CHECK_EQ(d.read(1), 1600);   // most recent
    CHECK_EQ(d.read(16), 100);   // oldest
    d.push(1700);                // wraps
    CHECK_EQ(d.read(1), 1700);
    CHECK_EQ(d.read(16), 200);
}

TEST(delay_line_lerp_read) {
    DelayLine<4> d;
    for (int i = 1; i <= 16; ++i) d.push(i * 100);
    CHECK_EQ(d.read_lerp(2u << 16), d.read(2));
    // Halfway between delay 2 (1500) and 3 (1400): 1450.
    CHECK_NEAR(d.read_lerp((2u << 16) | 0x8000), 1450, 1);
}

TEST(delay_line_clear) {
    DelayLine<4> d;
    d.push(123);
    d.clear();
    for (u32 i = 1; i <= 16; ++i) CHECK_EQ(d.read(i), 0);
}

TEST(reverb_impulse_decays) {
    Reverb r;
    ReverbParams p;
    p.wet_gain = GAIN_ONE;
    r.set_params(p);
    i64 in_l[64], in_r[64], out_l[64], out_r[64];
    // Impulse.
    for (int i = 0; i < 64; ++i) {
        in_l[i] = in_r[i] = 0;
        out_l[i] = out_r[i] = 0;
    }
    in_l[0] = in_r[0] = SAMPLE_ONE;
    r.process_block(in_l, in_r, out_l, out_r, 64);
    in_l[0] = in_r[0] = 0;

    i64 energy_early = 0, energy_late = 0, energy_mid = 0;
    for (int block = 0; block < 3000; ++block) {  // 4s tail
        for (int i = 0; i < 64; ++i) out_l[i] = out_r[i] = 0;
        r.process_block(in_l, in_r, out_l, out_r, 64);
        for (int i = 0; i < 64; ++i) {
            const i64 v = out_l[i] < 0 ? -out_l[i] : out_l[i];
            if (block < 100) energy_early += v;
            else if (block < 1000) energy_mid += v;
            else energy_late += v;
        }
    }
    CHECK(energy_early > 0);  // it reverberates
    CHECK(energy_mid > 0);
    CHECK(energy_late < energy_early);  // and decays
}

TEST(reverb_deterministic) {
    auto run = [] {
        Reverb r;
        i64 in_l[64], in_r[64], out_l[64], out_r[64];
        XorShift32 rng(5);
        i64 sum = 0;
        for (int block = 0; block < 200; ++block) {
            for (int i = 0; i < 64; ++i) {
                in_l[i] = rng.next_sample();
                in_r[i] = rng.next_sample();
                out_l[i] = out_r[i] = 0;
            }
            r.process_block(in_l, in_r, out_l, out_r, 64);
            for (int i = 0; i < 64; ++i) sum += out_l[i] * 31 + out_r[i];
        }
        return sum;
    };
    CHECK_EQ(run(), run());
}

TEST(reverb_stereo_decorrelated) {
    Reverb r;
    i64 in_l[64], in_r[64], out_l[64], out_r[64];
    for (int i = 0; i < 64; ++i) in_l[i] = in_r[i] = 0;
    in_l[0] = in_r[0] = SAMPLE_ONE;
    bool differs = false;
    for (int block = 0; block < 200; ++block) {
        for (int i = 0; i < 64; ++i) out_l[i] = out_r[i] = 0;
        r.process_block(in_l, in_r, out_l, out_r, 64);
        in_l[0] = in_r[0] = 0;
        for (int i = 0; i < 64; ++i)
            if (out_l[i] != out_r[i]) differs = true;
    }
    CHECK(differs);  // decorrelated comb lengths = width
}

TEST(reverb_silence_stays_silent) {
    Reverb r;
    i64 z[64] = {}, out_l[64] = {}, out_r[64] = {};
    for (int block = 0; block < 100; ++block) r.process_block(z, z, out_l, out_r, 64);
    for (int i = 0; i < 64; ++i) {
        CHECK_EQ(out_l[i], 0);
        CHECK_EQ(out_r[i], 0);
    }
}

TEST(chorus_wet_output_and_determinism) {
    auto run = [] {
        Chorus c;
        ChorusParams p;
        p.wet_gain = GAIN_ONE;
        c.set_params(p);
        i64 in_l[64], in_r[64], out_l[64], out_r[64];
        u32 ph = 0;
        i64 sum = 0, energy = 0;
        for (int block = 0; block < 400; ++block) {
            for (int i = 0; i < 64; ++i) {
                const i32 s = sine_q30(ph) >> 6;  // Q24 sine
                ph += 39370533;                   // 440 Hz
                in_l[i] = in_r[i] = s;
                out_l[i] = out_r[i] = 0;
            }
            c.process_block(in_l, in_r, out_l, out_r, 64);
            for (int i = 0; i < 64; ++i) {
                sum += out_l[i] * 31 + out_r[i];
                energy += out_l[i] < 0 ? -out_l[i] : out_l[i];
            }
        }
        CHECK(energy > 0);
        return sum;
    };
    CHECK_EQ(run(), run());
}
