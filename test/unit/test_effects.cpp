#include <algorithm>
#include <vector>

#include "test_framework.h"

#include "synth/effects/chorus.h"
#include "synth/effects/delay_fx.h"
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

// ---- Tempo-synced delay -------------------------------------------------

namespace {
// Where the first echo of a unit impulse lands, in samples.
int echo_offset(DelayFx& fx, int limit = 40000) {
    std::vector<i64> in_l(64, 0), in_r(64, 0), out_l(64, 0), out_r(64, 0);
    in_l[0] = 1LL << SAMPLE_SHIFT;
    in_r[0] = 1LL << SAMPLE_SHIFT;
    int n = 0;
    for (int b = 0; b < limit / 64; ++b) {
        std::fill(out_l.begin(), out_l.end(), 0);
        std::fill(out_r.begin(), out_r.end(), 0);
        fx.process_block(in_l.data(), in_r.data(), out_l.data(), out_r.data(), 64);
        for (int i = 0; i < 64; ++i)
            if (out_l[i] != 0) return n + i;
        std::fill(in_l.begin(), in_l.end(), 0);
        std::fill(in_r.begin(), in_r.end(), 0);
        n += 64;
    }
    return -1;
}
}  // namespace

TEST(delay_sync_follows_tempo) {
    // A dotted eighth at 174 BPM is 60/174 * 0.75 s = 258.6 ms = 12414 samples.
    DelayParams p;
    p.wet_gain = GAIN_ONE;
    p.sync_beats_q16 = (3 * GAIN_ONE) / 2;  // dotted 1/8 = 0.75 beats... see below
    // parse_sync_beats("1/8.") = 4/8 * 3/2 = 0.75 beats.
    p.sync_beats_q16 = (GAIN_ONE * 3) / 4;
    DelayFx fx;
    fx.set_params(p);
    fx.set_tempo(174 << GAIN_SHIFT);
    CHECK_NEAR(echo_offset(fx), 12414, 70);

    // Same spec at half the tempo is twice as long.
    DelayFx slow;
    slow.set_params(p);
    slow.set_tempo(87 << GAIN_SHIFT);
    CHECK_NEAR(echo_offset(slow), 24828, 140);

    // Tempo arriving after the params still takes effect.
    DelayFx late;
    late.set_tempo(174 << GAIN_SHIFT);
    late.set_params(p);
    CHECK_NEAR(echo_offset(late), 12414, 70);
}

TEST(delay_without_sync_is_milliseconds) {
    DelayParams p;
    p.wet_gain = GAIN_ONE;
    p.time_ms_q16 = 250 << GAIN_SHIFT;  // sync_beats_q16 stays 0
    DelayFx fx;
    fx.set_params(p);
    fx.set_tempo(174 << GAIN_SHIFT);    // must be ignored
    CHECK_NEAR(echo_offset(fx), 12000, 70);
}

// ---- Reverb send conditioning -------------------------------------------

TEST(reverb_pre_delay_holds_the_tail_back) {
    // The tail's own onset is the shortest comb (~1.2k samples), so measure
    // the SHIFT pre-delay adds rather than an absolute silence.
    auto first_output = [](gain_t pre_ms) {
        ReverbParams p;
        p.wet_gain = GAIN_ONE;
        p.pre_delay_ms_q16 = pre_ms;
        Reverb rv;
        rv.set_params(p);
        std::vector<i64> in_l(64, 0), in_r(64, 0), out_l(64), out_r(64);
        in_l[0] = in_r[0] = 8LL << SAMPLE_SHIFT;
        for (int b = 0; b < 400; ++b) {
            std::fill(out_l.begin(), out_l.end(), 0);
            std::fill(out_r.begin(), out_r.end(), 0);
            rv.process_block(in_l.data(), in_r.data(), out_l.data(), out_r.data(), 64);
            for (int i = 0; i < 64; ++i)
                if (out_l[i] != 0) return b * 64 + i;
            std::fill(in_l.begin(), in_l.end(), 0);
            std::fill(in_r.begin(), in_r.end(), 0);
        }
        return -1;
    };
    const int none = first_output(0);
    const int fifty = first_output(50 << GAIN_SHIFT);
    CHECK(none > 0);
    CHECK_NEAR(fifty - none, 2400, 70);   // 50 ms at 48 kHz
}

TEST(reverb_send_highpass_rejects_bass) {
    // A 40 Hz tone into a reverb high-passed at 400 Hz should return far less
    // than the same tone with the filter off.
    auto tail_energy = [](gain_t hp_hz) {
        ReverbParams p;
        p.wet_gain = GAIN_ONE;
        p.high_pass_hz_q16 = hp_hz;
        Reverb rv;
        rv.set_params(p);
        std::vector<i64> in_l(64), in_r(64), out_l(64), out_r(64);
        i64 acc = 0;
        u32 phase = 0;
        const u32 inc = static_cast<u32>((40ull << 32) / SAMPLE_RATE);
        for (int b = 0; b < 200; ++b) {
            for (int i = 0; i < 64; ++i) {
                const i64 s = asr(static_cast<i64>(sine_q30(phase)), 6);  // Q2.30 -> Q8.24
                in_l[i] = in_r[i] = s;
                phase += inc;
            }
            std::fill(out_l.begin(), out_l.end(), 0);
            std::fill(out_r.begin(), out_r.end(), 0);
            rv.process_block(in_l.data(), in_r.data(), out_l.data(), out_r.data(), 64);
            if (b > 100)  // let it fill
                for (int i = 0; i < 64; ++i) acc += out_l[i] < 0 ? -out_l[i] : out_l[i];
        }
        return acc;
    };
    const i64 open = tail_energy(0);
    const i64 filtered = tail_energy(400 << GAIN_SHIFT);
    CHECK(open > 0);
    CHECK(filtered * 4 < open);
}
