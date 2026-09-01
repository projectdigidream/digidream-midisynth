#include "test_framework.h"

#include "synth/oscillator.h"
#include "synth/tables.h"
#include "synth/wavetable_cache.h"

using namespace msynth;

namespace {
WaveTablePtr make_table(const char* params_json, int frames) {
    std::string err;
    JsonPtr params = json_parse(params_json, std::strlen(params_json), &err);
    static WaveTableCache cache;
    WaveTablePtr t = cache.get_or_create("additiveHarmonicMorph", frames, params.get(), &err);
    return t;
}
}  // namespace

TEST(mip_for_cents_selection) {
    CHECK_EQ(mip_for_cents(0), 0);                        // lowest notes: all harmonics
    CHECK_EQ(mip_for_cents((2399 << 16)), 0);
    CHECK_EQ(mip_for_cents((3600 << 16)), 1);             // one level per octave
    CHECK_EQ(mip_for_cents((12700 << 16)), MIP_LEVELS - 1);
    CHECK_EQ(mip_for_cents(0x7FFFFFFF), MIP_LEVELS - 1);  // clamped
}

TEST(hermite_reproduces_linear_ramp_exactly) {
    // Catmull-Rom is exact for polynomials up to cubic; a linear ramp
    // interpolates with zero error away from the wrap.
    i32 tab[64];
    for (int i = 0; i < 64; ++i) tab[i] = i * 1000;
    for (u32 idx = 4; idx < 60; ++idx) {
        CHECK_EQ(hermite4(tab, idx, 63, 0), tab[idx]);
        CHECK_NEAR(hermite4(tab, idx, 63, 0x8000), (tab[idx] + tab[idx + 1]) / 2, 2);
    }
}

TEST(oscillator_phase_wraps_free) {
    WaveTablePtr t = make_table(R"({"startHarmonics":1,"endHarmonics":1,"falloff":"flat"})", 1);
    CHECK(t != nullptr);
    Oscillator o;
    o.set_table(*t, 0);
    o.phase = 0xFFFFFF00u;
    o.phase_inc = 0x200;
    o.next_q30();
    o.next_q30();  // crosses 2^32
    CHECK(o.phase < 0x1000u);  // wrapped, no special handling
}

TEST(oscillator_sine_frequency_accurate) {
    // Run a sine wavetable at A440 for 1s and count rising zero crossings.
    WaveTablePtr t = make_table(R"({"startHarmonics":1,"endHarmonics":1,"falloff":"flat"})", 1);
    Oscillator o;
    o.set_table(*t, 0);
    o.phase_inc = phase_inc_for_cents((69 * 100) << 16);
    int crossings = 0;
    i32 prev = o.next_q30();
    for (int i = 1; i < SAMPLE_RATE; ++i) {
        const i32 v = o.next_q30();
        if (prev < 0 && v >= 0) ++crossings;
        prev = v;
    }
    CHECK_NEAR(crossings, 440, 1);
}

TEST(oscillator_sine_output_clean) {
    // A pure-sine table through Hermite must stay near the ideal sine:
    // interpolation error at 2048 samples/cycle is far below -60 dB.
    WaveTablePtr t = make_table(R"({"startHarmonics":1,"endHarmonics":1,"falloff":"flat"})", 1);
    Oscillator o;
    o.set_table(*t, 0);
    o.phase_inc = phase_inc_for_cents((57 * 100) << 16);  // 220 Hz
    i64 peak = 0;
    for (int i = 0; i < 48000; ++i) {
        const i32 v = o.next_q30();
        const i64 av = v < 0 ? -static_cast<i64>(v) : v;
        if (av > peak) peak = av;
    }
    // Normalized to ~0.98 * Q30 at generation.
    CHECK_NEAR(peak, (static_cast<i64>(COEF_ONE) * 98) / 100, COEF_ONE / 64);
}

TEST(oscillator_frame_morph_interpolates) {
    // Two frames: 1 harmonic -> 16 harmonics. Position halfway must contain
    // mid-level upper-harmonic energy (strictly between the endpoints).
    WaveTablePtr t = make_table(R"({"startHarmonics":1,"endHarmonics":16,"falloff":"saw"})", 2);
    CHECK_EQ(t->frameCount, 2);
    auto harmonic_energy = [&](i32 pos_q16) {
        Oscillator o;
        o.set_table(*t, 0);
        o.set_frame_pos(pos_q16);
        o.phase_inc = 1u << 21;  // one cycle per 2048 samples
        // Correlate with the 5th harmonic over one cycle.
        const i32* sine = sine_table_4096();
        i64 acc = 0;
        for (int i = 0; i < 2048; ++i) {
            const i32 v = o.next_q30();
            acc += static_cast<i64>(v) * sine[(5 * i * 2) & 4095] >> 30;
        }
        return acc < 0 ? -acc : acc;
    };
    const i64 e0 = harmonic_energy(0);
    const i64 e1 = harmonic_energy(1 << 16);
    const i64 emid = harmonic_energy(1 << 15);
    CHECK(e1 > 100 * (e0 + 1));      // frame 1 has the harmonic, frame 0 doesn't
    CHECK(emid > e0 && emid < e1);   // morph sits between
}

TEST(oscillator_set_frame_pos_clamps) {
    WaveTablePtr t = make_table(R"({"startHarmonics":1,"endHarmonics":4,"falloff":"saw"})", 4);
    Oscillator o;
    o.set_table(*t, 0);
    o.set_frame_pos(100 << 16);
    CHECK_EQ(o.frame_pos_q16, 3 << 16);
    o.set_frame_pos(-(5 << 16));
    CHECK_EQ(o.frame_pos_q16, 0);
}

TEST(oscillator_mip_shift_layout) {
    WaveTablePtr t = make_table(R"({"startHarmonics":1,"endHarmonics":1,"falloff":"flat"})", 1);
    Oscillator o;
    o.set_table(*t, 0);
    CHECK_EQ(o.idx_shift, 21);  // 2048 samples
    CHECK_EQ(o.frac_shift, 5);
    o.set_table(*t, 8);
    CHECK_EQ(o.idx_shift, 26);  // 64 samples (MIN_TABLE_SIZE floor)
    CHECK_EQ(o.frac_shift, 10);
    CHECK_EQ(static_cast<i32>(o.table_n), mip_table_size(8));
}

#include <cmath>

TEST(oscillator_top_mip_interpolation_purity) {
    // Regression: 8-sample top-mip tables put Hermite interpolation spurs at
    // ~-32 dB (audible hash on top-octave notes). With the MIN_TABLE_SIZE
    // floor, residual off-harmonic energy must sit below -55 dB.
    WaveTablePtr t = make_table(R"({"startHarmonics":1,"endHarmonics":1,"falloff":"flat"})", 1);
    Oscillator o;
    o.set_table(*t, MIP_LEVELS - 1);
    o.phase_inc = phase_inc_for_cents((108 * 100) << 16);  // ~4186 Hz
    const int N = 8192;
    std::vector<double> y(N);
    for (int i = 0; i < N; ++i) y[i] = static_cast<double>(o.next_q30());
    // Remove the fundamental (and nothing else) by projecting it out.
    const double f = 4186.009 / 48000.0;
    double cr = 0, ci = 0, total = 0;
    for (int i = 0; i < N; ++i) {
        cr += y[i] * std::cos(2 * M_PI * f * i);
        ci += y[i] * std::sin(2 * M_PI * f * i);
        total += y[i] * y[i];
    }
    const double fund_power = 2 * (cr * cr + ci * ci) / N;
    const double resid = total - fund_power;
    // Residual holds windowing leakage too, so the bound is conservative.
    CHECK(resid < total / 100000);  // < -50 dB overall residual
}
