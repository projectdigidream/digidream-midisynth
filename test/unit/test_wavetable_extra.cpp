//
// New generators: spectra behave as designed, every mip stays band-limited
// (they all pass through the DFT -> capped-resynthesis path), generation is
// deterministic, velocity curves order correctly.
//
#include "test_framework.h"

#include "synth/tables.h"
#include "synth/wavetable_cache.h"

using namespace msynth;

namespace {

JsonPtr parse(const char* s) {
    std::string err;
    return json_parse(s, std::strlen(s), &err);
}

WaveTablePtr gen(WaveTableCache& cache, const char* name, int frames, const char* params,
                 std::string* err_out = nullptr) {
    std::string err;
    JsonPtr p = params ? parse(params) : nullptr;
    WaveTablePtr t = cache.get_or_create(name, frames, p.get(), &err);
    if (err_out) *err_out = err;
    return t;
}

i64 harmonic_energy(const WaveTable& t, int mip, int frame, int h) {
    const int n = mip_table_size(mip);
    const int step = 4096 / n;
    const i32* data = t.mipData[mip] + static_cast<size_t>(frame) * n;
    const i32* sine = sine_table_4096();
    i64 acc_s = 0, acc_c = 0;
    for (int i = 0; i < n; ++i) {
        acc_s += static_cast<i64>(data[i]) * sine[(h * i * step) & 4095] >> 30;
        acc_c += static_cast<i64>(data[i]) * sine[((h * i * step) + 1024) & 4095] >> 30;
    }
    const i64 as = acc_s < 0 ? -acc_s : acc_s;
    const i64 ac = acc_c < 0 ? -acc_c : acc_c;
    return as + ac;
}

}  // namespace

TEST(pulse_square_duty_kills_even_harmonics) {
    WaveTableCache cache;
    WaveTablePtr t = gen(cache, "pulse", 1, R"({"startDuty":0.5,"endDuty":0.5})");
    CHECK(t != nullptr);
    const i64 h1 = harmonic_energy(*t, 0, 0, 1);
    CHECK(h1 > 0);
    CHECK(harmonic_energy(*t, 0, 0, 2) < h1 / 100);  // 50% duty = square: no evens
    CHECK(harmonic_energy(*t, 0, 0, 3) > h1 / 10);
}

TEST(pulse_narrow_duty_has_even_harmonics_and_morphs) {
    WaveTableCache cache;
    WaveTablePtr t = gen(cache, "pulse", 4, R"({"startDuty":0.5,"endDuty":0.15})");
    CHECK_EQ(t->frameCount, 4);
    const i64 h1 = harmonic_energy(*t, 0, 3, 1);
    CHECK(harmonic_energy(*t, 0, 3, 2) > h1 / 20);  // 15% duty: strong evens
    // Frame 0 is the square: evens absent there.
    CHECK(harmonic_energy(*t, 0, 0, 2) < harmonic_energy(*t, 0, 0, 1) / 100);
}

TEST(drawbar_places_exact_harmonics) {
    WaveTableCache cache;
    // Only bars 1 (h=1) and 7 (h=8).
    WaveTablePtr t = gen(cache, "drawbar", 1, R"({"drawbars":[8,0,0,0,0,0,8,0,0]})");
    CHECK(t != nullptr);
    const i64 h1 = harmonic_energy(*t, 0, 0, 1);
    const i64 h8 = harmonic_energy(*t, 0, 0, 8);
    CHECK(h1 > 0);
    CHECK_NEAR(h8 * 100 / (h1 ? h1 : 1), 100, 8);  // equal bars = equal level
    for (int h : {2, 3, 4, 5, 6, 7, 9, 10, 12})
        CHECK(harmonic_energy(*t, 0, 0, h) < h1 / 100);
}

TEST(drawbar_morphs_to_end_bars) {
    WaveTableCache cache;
    WaveTablePtr t = gen(cache, "drawbar", 2,
                         R"({"drawbars":[8,0,0,0,0,0,0,0,0],
                             "endDrawbars":[0,8,0,0,0,0,0,0,0]})");
    CHECK(harmonic_energy(*t, 0, 0, 1) > 100 * (harmonic_energy(*t, 0, 0, 2) + 1));
    CHECK(harmonic_energy(*t, 0, 1, 2) > 100 * (harmonic_energy(*t, 0, 1, 1) + 1));
}

TEST(waveshaper_symmetric_shapes_make_odd_harmonics) {
    WaveTableCache cache;
    for (const char* shape : {"soft", "hard", "fold"}) {
        const std::string params = std::string(R"({"shape":")") + shape +
                                   R"(","startDrive":6,"endDrive":6})";
        WaveTablePtr t = gen(cache, "waveshaper", 1, params.c_str());
        CHECK(t != nullptr);
        const i64 h1 = harmonic_energy(*t, 0, 0, 1);
        const i64 h3 = harmonic_energy(*t, 0, 0, 3);
        const i64 h2 = harmonic_energy(*t, 0, 0, 2);
        CHECK(h1 > 0);
        CHECK(h3 > h1 / 100);   // driven: real 3rd harmonic
        CHECK(h2 < h1 / 100);   // symmetric shaping: no evens
    }
}

TEST(waveshaper_drive_morph_brightens) {
    WaveTableCache cache;
    WaveTablePtr t = gen(cache, "waveshaper", 4, R"({"shape":"soft","startDrive":1,"endDrive":10})");
    const i64 lo = harmonic_energy(*t, 0, 0, 5);
    const i64 hi = harmonic_energy(*t, 0, 3, 5);
    CHECK(hi > 4 * (lo + 1));  // more drive = more upper harmonic energy
}

TEST(waveshaper_unknown_shape_errors) {
    WaveTableCache cache;
    std::string err;
    CHECK(gen(cache, "waveshaper", 1, R"({"shape":"quantum"})", &err) == nullptr);
    CHECK(!err.empty());
}

TEST(fm2op_index_grows_sidebands) {
    WaveTableCache cache;
    // Index 0: pure carrier. High index at ratio 2: strong odd partials.
    WaveTablePtr t0 = gen(cache, "fm2op", 1, R"({"ratio":2,"startIndex":0,"endIndex":0})");
    const i64 p1 = harmonic_energy(*t0, 0, 0, 1);
    CHECK(p1 > 0);
    CHECK(harmonic_energy(*t0, 0, 0, 3) < p1 / 100);

    WaveTablePtr t1 = gen(cache, "fm2op", 1, R"({"ratio":2,"startIndex":3,"endIndex":3})");
    const i64 q1 = harmonic_energy(*t1, 0, 0, 1);
    const i64 q3 = harmonic_energy(*t1, 0, 0, 3);
    CHECK(q3 > q1 / 10);  // sidebands at c +/- 2m land on odd harmonics
}

TEST(new_generators_mips_band_limited) {
    // The correctness gate again, for the DFT path: no energy above each
    // mip's harmonic cap.
    WaveTableCache cache;
    const char* cases[][2] = {
        {"pulse", R"({"startDuty":0.1,"endDuty":0.1})"},
        {"waveshaper", R"({"shape":"fold","startDrive":9,"endDrive":9})"},
        {"fm2op", R"({"ratio":7,"startIndex":6,"endIndex":6})"},
    };
    for (const auto& c : cases) {
        WaveTablePtr t = gen(cache, c[0], 1, c[1]);
        CHECK(t != nullptr);
        for (int m = 0; m < MIP_LEVELS; ++m) {
            const int max_h = mip_table_size(m) / 2 - 1;
            const int cap = HARM_LIMIT[m] < max_h ? HARM_LIMIT[m] : max_h;
            const i64 h1 = harmonic_energy(*t, m, 0, 1) + 1;
            if (cap + 1 <= max_h) CHECK(harmonic_energy(*t, m, 0, cap + 1) < h1 / 20);
        }
    }
}

TEST(new_generators_deterministic_and_cached) {
    WaveTableCache cache;
    const char* params = R"({"ratio":3,"startIndex":1,"endIndex":5})";
    WaveTablePtr a = gen(cache, "fm2op", 8, params);
    WaveTablePtr b = gen(cache, "fm2op", 8, params);
    CHECK(a.get() == b.get());  // cache dedupe covers new generators too
    WaveTableCache cache2;
    WaveTablePtr c = gen(cache2, "fm2op", 8, params);
    CHECK(a->storage == c->storage);  // regeneration is bit-identical
}

TEST(velocity_curve_tables) {
    CHECK_EQ(VEL_LINEAR_Q16[0], 0);
    CHECK_EQ(VEL_LINEAR_Q16[127], GAIN_ONE);
    CHECK_EQ(VEL_SOFT_Q16[0], 0);
    CHECK_NEAR(VEL_SOFT_Q16[127], GAIN_ONE, 2);
    for (int v = 1; v < 127; ++v) {
        // linear > soft > squared strictly between the endpoints.
        CHECK(VEL_LINEAR_Q16[v] > VEL_SOFT_Q16[v]);
        CHECK(VEL_SOFT_Q16[v] > VEL_GAIN_Q16[v]);
        CHECK(VEL_LINEAR_Q16[v] >= VEL_LINEAR_Q16[v - 1]);
        CHECK(VEL_SOFT_Q16[v] >= VEL_SOFT_Q16[v - 1]);
    }
}

TEST(waveshaper_bias_adds_even_harmonics) {
    WaveTableCache cache;
    WaveTablePtr sym = gen(cache, "waveshaper", 1, R"({"shape":"soft","startDrive":3,"endDrive":3})");
    WaveTablePtr asym = gen(cache, "waveshaper", 1, R"({"shape":"soft","startDrive":3,"endDrive":3,"bias":0.4})");
    const i64 h1 = harmonic_energy(*asym, 0, 0, 1);
    CHECK(harmonic_energy(*sym, 0, 0, 2) < h1 / 100);   // symmetric: no evens
    CHECK(harmonic_energy(*asym, 0, 0, 2) > h1 / 20);   // biased: 2nd appears
}

TEST(waveshaper_input_wave_selects_source) {
    WaveTableCache cache;
    // A clean saw input at drive 1 keeps the 1/h series (evens included);
    // the sine input at the same drive stays close to a sine.
    WaveTablePtr saw = gen(cache, "waveshaper", 1, R"({"shape":"soft","input":"saw","startDrive":1,"endDrive":1})");
    WaveTablePtr sine = gen(cache, "waveshaper", 1, R"({"shape":"soft","input":"sine","startDrive":1,"endDrive":1})");
    CHECK(saw != nullptr && sine != nullptr);
    CHECK(harmonic_energy(*saw, 0, 0, 2) > harmonic_energy(*saw, 0, 0, 1) / 6);
    CHECK(harmonic_energy(*sine, 0, 0, 2) < harmonic_energy(*sine, 0, 0, 1) / 50);
    std::string err;
    CHECK(gen(cache, "waveshaper", 1, R"({"input":"nope"})", &err) == nullptr);
    CHECK(err.find("input") != std::string::npos);
}

TEST(chebyshev_weight_n_is_harmonic_n_at_full_drive) {
    WaveTableCache cache;
    WaveTablePtr t = gen(cache, "chebyshev", 1, R"({"weights":[0,0,0,0,1,0,0,0]})");
    CHECK(t != nullptr);
    const i64 h5 = harmonic_energy(*t, 0, 0, 5);
    CHECK(h5 > 0);
    for (int h = 1; h <= 8; ++h)
        if (h != 5) CHECK(harmonic_energy(*t, 0, 0, h) < h5 / 50);
}

TEST(chebyshev_drive_develops_partials_nonlinearly) {
    WaveTableCache cache;
    // T5 at low drive barely develops its 5th harmonic; at drive 1 it is pure.
    WaveTablePtr lo = gen(cache, "chebyshev", 1, R"({"weights":[0,0,0,0,1,0,0,0],"startDrive":0.3,"endDrive":0.3})");
    WaveTablePtr hi = gen(cache, "chebyshev", 1, R"({"weights":[0,0,0,0,1,0,0,0],"startDrive":1,"endDrive":1})");
    const i64 lo5 = harmonic_energy(*lo, 0, 0, 5), lo1 = harmonic_energy(*lo, 0, 0, 1);
    CHECK(lo1 > lo5);  // partial development leaves mostly low content
    CHECK(harmonic_energy(*hi, 0, 0, 5) > harmonic_energy(*hi, 0, 0, 1) * 20);
    // Drive morph across frames changes the spectrum.
    WaveTablePtr sweep = gen(cache, "chebyshev", 4, R"({"weights":[0,0,0,0,1,0,0,0],"startDrive":0.3,"endDrive":1})");
    CHECK(harmonic_energy(*sweep, 0, 3, 5) > harmonic_energy(*sweep, 0, 0, 5) * 2);
}

TEST(chebyshev_defaults_and_bad_params) {
    WaveTableCache cache;
    // No params -> T1 = plain sine; all-zero weights fall back to a sine too.
    WaveTablePtr d = gen(cache, "chebyshev", 1, nullptr);
    CHECK(d != nullptr);
    CHECK(harmonic_energy(*d, 0, 0, 2) < harmonic_energy(*d, 0, 0, 1) / 100);
    WaveTablePtr z = gen(cache, "chebyshev", 1, R"({"weights":[0,0,0,0,0,0,0,0]})");
    CHECK(z != nullptr);
    CHECK(harmonic_energy(*z, 0, 0, 1) > 0);
    std::string err;
    CHECK(gen(cache, "chebyshev", 1, R"({"weights":3})", &err) == nullptr);
    CHECK(!err.empty());
}

TEST(waveshaper_crush_and_stairs_shapes) {
    WaveTableCache cache;
    WaveTablePtr crush = gen(cache, "waveshaper", 1, R"({"shape":"crush","levels":3,"startDrive":1,"endDrive":1})");
    WaveTablePtr soft = gen(cache, "waveshaper", 1, R"({"shape":"soft","startDrive":1,"endDrive":1})");
    CHECK(crush != nullptr && soft != nullptr);
    // Quantizing to 3 levels creates far more high-harmonic energy than the
    // smooth curve at the same drive.
    i64 hi_crush = 0, hi_soft = 0;
    for (int h = 7; h <= 15; h += 2) {
        hi_crush += harmonic_energy(*crush, 0, 0, h);
        hi_soft += harmonic_energy(*soft, 0, 0, h);
    }
    CHECK(hi_crush > hi_soft * 4);
    WaveTablePtr stairs = gen(cache, "waveshaper", 1, R"({"shape":"stairs","steps":8,"startDrive":1,"endDrive":1})");
    CHECK(stairs != nullptr);
    CHECK(harmonic_energy(*stairs, 0, 0, 7) > harmonic_energy(*soft, 0, 0, 7) * 2);
    std::string err;
    CHECK(gen(cache, "waveshaper", 1, R"({"shape":"nope2"})", &err) == nullptr);
}

TEST(harmonics_places_exact_partials_and_morphs) {
    WaveTableCache cache;
    WaveTablePtr t = gen(cache, "harmonics", 1, R"({"amps":[0,0,1,0,0,0.5]})");
    CHECK(t != nullptr);
    const i64 h3 = harmonic_energy(*t, 0, 0, 3);
    const i64 h6 = harmonic_energy(*t, 0, 0, 6);
    CHECK(h3 > 0);
    CHECK(h6 * 3 > h3 && h6 < h3);          // 0.5 vs 1.0 (post-normalization ratio kept)
    CHECK(harmonic_energy(*t, 0, 0, 1) < h3 / 50);
    CHECK(harmonic_energy(*t, 0, 0, 2) < h3 / 50);
    // Morph to a different recipe.
    WaveTablePtr m = gen(cache, "harmonics", 4, R"({"amps":[1],"endAmps":[0,0,0,1]})");
    CHECK(harmonic_energy(*m, 0, 0, 1) > harmonic_energy(*m, 0, 0, 4) * 10);
    CHECK(harmonic_energy(*m, 0, 3, 4) > harmonic_energy(*m, 0, 3, 1) * 10);
    // Defaults / silence fall back to a sine; bad params error.
    WaveTablePtr z = gen(cache, "harmonics", 1, R"({"amps":[0,0]})");
    CHECK(harmonic_energy(*z, 0, 0, 1) > 0);
    std::string err;
    CHECK(gen(cache, "harmonics", 1, R"({"amps":7})", &err) == nullptr);
}

TEST(hardsync_ratio_one_is_plain_slave_and_sweep_brightens) {
    WaveTableCache cache;
    // ratio 1 saw == a saw: strong 1/h series.
    WaveTablePtr saw = gen(cache, "hardsync", 1, R"({"startRatio":1,"endRatio":1})");
    CHECK(saw != nullptr);
    const i64 h1 = harmonic_energy(*saw, 0, 0, 1);
    CHECK(harmonic_energy(*saw, 0, 0, 2) > h1 / 6);
    // A fractional ratio shifts energy toward the partials around the slave
    // frequency: harmonic 3 dominates harmonic 1 at ratio 2.6.
    WaveTablePtr sync = gen(cache, "hardsync", 1, R"({"startRatio":2.6,"endRatio":2.6})");
    CHECK(harmonic_energy(*sync, 0, 0, 3) > harmonic_energy(*sync, 0, 0, 1));
    // The classic sweep changes the spectrum across frames.
    WaveTablePtr sweep = gen(cache, "hardsync", 4, R"({"startRatio":1,"endRatio":4})");
    CHECK(harmonic_energy(*sweep, 0, 3, 4) > harmonic_energy(*sweep, 0, 0, 4) * 3);
    // Pulse slave + bad slave name.
    CHECK(gen(cache, "hardsync", 1, R"({"slave":"pulse","startRatio":2,"endRatio":2})") != nullptr);
    std::string err;
    CHECK(gen(cache, "hardsync", 1, R"({"slave":"square"})", &err) == nullptr);
}

TEST(custom_cycle_generator) {
    WaveTableCache cache;
    // A drawn ramp: behaves like a saw (evens present), band-limited mips.
    std::string ramp = "[";
    for (int i = 0; i < 64; ++i) ramp += (i ? "," : "") + std::to_string((i - 32) / 32.0).substr(0, 8);
    ramp += "]";
    WaveTablePtr t = gen(cache, "custom", 1, ("{\"samples\":" + ramp + "}").c_str());
    CHECK(t != nullptr);
    CHECK(harmonic_energy(*t, 0, 0, 2) > harmonic_energy(*t, 0, 0, 1) / 8);
    std::string err;
    CHECK(gen(cache, "custom", 1, R"({"samples": 4})", &err) == nullptr);
    CHECK(gen(cache, "custom", 1, nullptr, &err) == nullptr);  // needs samples
}
