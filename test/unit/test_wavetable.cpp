//
// Wavetable generation: band-limiting per mip (the aliasing correctness gate,
// plan §18 step 4), normalization, cache dedupe.
//
#include "test_framework.h"

#include "synth/tables.h"
#include "synth/wavetable_cache.h"

using namespace msynth;

namespace {

JsonPtr parse(const char* s) {
    std::string err;
    JsonPtr p = json_parse(s, std::strlen(s), &err);
    return p;
}

// Correlate one frame of a mip against harmonic h; returns |energy|.
i64 harmonic_energy(const WaveTable& t, int mip, int frame, int h) {
    const int n = mip_table_size(mip);
    const int step = 4096 / n;
    const i32* data = t.mipData[mip] + static_cast<size_t>(frame) * n;
    const i32* sine = sine_table_4096();
    i64 acc = 0;
    for (int i = 0; i < n; ++i)
        acc += static_cast<i64>(data[i]) * sine[(h * i * step) & 4095] >> 30;
    return acc < 0 ? -acc : acc;
}

}  // namespace

TEST(additive_saw_has_expected_spectrum) {
    WaveTableCache cache;
    std::string err;
    JsonPtr p = parse(R"({"startHarmonics":16,"endHarmonics":16,"falloff":"saw"})");
    WaveTablePtr t = cache.get_or_create("additiveHarmonicMorph", 1, p.get(), &err);
    CHECK(t != nullptr);
    // 1/h falloff: harmonic 1 has ~2x the energy of harmonic 2, 4x of 4.
    const i64 h1 = harmonic_energy(*t, 0, 0, 1);
    const i64 h2 = harmonic_energy(*t, 0, 0, 2);
    const i64 h4 = harmonic_energy(*t, 0, 0, 4);
    CHECK(h1 > 0);
    CHECK_NEAR(h1 / (h2 ? h2 : 1), 2, 1);
    CHECK_NEAR(h1 / (h4 ? h4 : 1), 4, 1);
    // Harmonic 17 was never requested.
    CHECK(harmonic_energy(*t, 0, 0, 17) < h1 / 200);
}

TEST(additive_square_odd_harmonics_only) {
    WaveTableCache cache;
    std::string err;
    JsonPtr p = parse(R"({"startHarmonics":9,"endHarmonics":9,"falloff":"square"})");
    WaveTablePtr t = cache.get_or_create("additiveHarmonicMorph", 1, p.get(), &err);
    const i64 h1 = harmonic_energy(*t, 0, 0, 1);
    const i64 h2 = harmonic_energy(*t, 0, 0, 2);
    const i64 h3 = harmonic_energy(*t, 0, 0, 3);
    CHECK(h1 > 0 && h3 > 0);
    CHECK(h2 < h1 / 200);
}

TEST(mips_are_band_limited) {
    // The whole point of mipmapping: a 100-harmonic recipe must be capped
    // per level so high notes can't alias.
    WaveTableCache cache;
    std::string err;
    JsonPtr p = parse(R"({"startHarmonics":100,"endHarmonics":100,"falloff":"saw"})");
    WaveTablePtr t = cache.get_or_create("additiveHarmonicMorph", 1, p.get(), &err);
    for (int m = 0; m < MIP_LEVELS; ++m) {
        const int limit = HARM_LIMIT[m] < 100 ? HARM_LIMIT[m] : 100;
        const int max_h = mip_table_size(m) / 2 - 1;
        const int cap = limit < max_h ? limit : max_h;
        const i64 h1 = harmonic_energy(*t, m, 0, 1);
        CHECK(h1 > 0);
        if (cap + 1 <= mip_table_size(m) / 2 - 1) {
            // First harmonic past the cap must be absent.
            CHECK(harmonic_energy(*t, m, 0, cap + 1) < h1 / 100);
        }
        // The top mip is (nearly) a pure sine.
        if (m == MIP_LEVELS - 1) CHECK_EQ(cap, 1);
    }
}

TEST(frames_normalized_to_consistent_peak) {
    WaveTableCache cache;
    std::string err;
    JsonPtr p = parse(R"({"startHarmonics":1,"endHarmonics":24,"falloff":"saw"})");
    WaveTablePtr t = cache.get_or_create("additiveHarmonicMorph", 8, p.get(), &err);
    const i64 target = (static_cast<i64>(COEF_ONE) * 98) / 100;
    for (int f = 0; f < 8; ++f) {
        i64 peak = 0;
        for (int i = 0; i < TABLE_SIZE; ++i) {
            const i32 v = t->mipData[0][f * TABLE_SIZE + i];
            const i64 av = v < 0 ? -static_cast<i64>(v) : v;
            if (av > peak) peak = av;
        }
        // No volume jumps across the morph (plan §3).
        CHECK_NEAR(peak, target, COEF_ONE / 50);
    }
}

TEST(cache_dedupes_identical_recipes) {
    WaveTableCache cache;
    std::string err;
    JsonPtr p1 = parse(R"({"startHarmonics":4,"endHarmonics":8,"falloff":"saw"})");
    JsonPtr p2 = parse(R"({"falloff":"saw","endHarmonics":8,"startHarmonics":4})");  // reordered
    WaveTablePtr a = cache.get_or_create("additiveHarmonicMorph", 4, p1.get(), &err);
    WaveTablePtr b = cache.get_or_create("additiveHarmonicMorph", 4, p2.get(), &err);
    CHECK(a.get() == b.get());  // key order canonicalized
    CHECK_EQ(cache.size(), 1);
    JsonPtr p3 = parse(R"({"startHarmonics":4,"endHarmonics":9,"falloff":"saw"})");
    WaveTablePtr c = cache.get_or_create("additiveHarmonicMorph", 4, p3.get(), &err);
    CHECK(a.get() != c.get());
    CHECK_EQ(cache.size(), 2);
}

TEST(cache_unknown_generator_fails_cleanly) {
    WaveTableCache cache;
    std::string err;
    WaveTablePtr t = cache.get_or_create("granularDreams", 1, nullptr, &err);
    CHECK(t == nullptr);
    CHECK(!err.empty());
}

TEST(cache_clamps_frame_count) {
    WaveTableCache cache;
    std::string err;
    JsonPtr p = parse(R"({"startHarmonics":1,"endHarmonics":2,"falloff":"saw"})");
    WaveTablePtr t = cache.get_or_create("additiveHarmonicMorph", 999, p.get(), &err);
    CHECK(t != nullptr);
    CHECK_EQ(t->frameCount, MAX_FRAMES);
}

TEST(unknown_falloff_is_an_error) {
    WaveTableCache cache;
    std::string err;
    JsonPtr p = parse(R"({"startHarmonics":1,"endHarmonics":2,"falloff":"cheese"})");
    WaveTablePtr t = cache.get_or_create("additiveHarmonicMorph", 1, p.get(), &err);
    CHECK(t == nullptr);
    CHECK(!err.empty());
}

TEST(wavetable_storage_layout) {
    WaveTable t;
    t.allocate(4);
    size_t total = 0;
    for (int m = 0; m < MIP_LEVELS; ++m) total += 4u * mip_table_size(m);
    CHECK_EQ(t.storage.size(), total);
    CHECK_EQ(t.mipLog2Size[0], 11);
    CHECK_EQ(t.mipLog2Size[8], 6);  // floored at MIN_TABLE_SIZE = 64
    // Mip regions are disjoint and contiguous.
    CHECK(t.mipData[1] == t.mipData[0] + 4 * mip_table_size(0));
}
