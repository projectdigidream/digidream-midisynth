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
