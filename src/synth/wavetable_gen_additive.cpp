//
// Additive harmonic-morph generator: frames sweep the harmonic count from
// startHarmonics to endHarmonics; per-harmonic amplitude follows the falloff
// recipe. Producing each mip is just capping the harmonic count (plan §4).
//
// All sine arguments are (h * n * 4096/N) mod 4096 -- exact table indices, so
// generation is pure integer math and identical everywhere.
//
#include <map>

#include "tables.h"
#include "wavetable_gen.h"

namespace msynth {
namespace {

enum class Falloff { Saw, Square, Triangle, Flat };

// Amplitude of harmonic h (1-based), Q16.16 signed.
i32 harmonic_amp(Falloff f, int h) {
    switch (f) {
        case Falloff::Saw: return GAIN_ONE / h;
        case Falloff::Square: return (h & 1) ? GAIN_ONE / h : 0;
        case Falloff::Triangle: {
            if (!(h & 1)) return 0;
            const i32 a = GAIN_ONE / (h * h);
            return ((h >> 1) & 1) ? -a : a;  // 1, -1/9, 1/25, -1/49, ...
        }
        case Falloff::Flat: return GAIN_ONE;
    }
    return 0;
}

bool gen_additive(const JsonValue* params, int frameCount, WaveTable& out, std::string* err) {
    int start = 1, end = 8;
    Falloff falloff = Falloff::Saw;
    if (params) {
        start = static_cast<int>(params->get_int("startHarmonics", 1));
        end = static_cast<int>(params->get_int("endHarmonics", 8));
        const std::string f = params->get_str("falloff", "saw");
        if (f == "saw") falloff = Falloff::Saw;
        else if (f == "square") falloff = Falloff::Square;
        else if (f == "triangle") falloff = Falloff::Triangle;
        else if (f == "flat") falloff = Falloff::Flat;
        else {
            if (err) *err = "unknown falloff '" + f + "'";
            return false;
        }
    }
    if (start < 1) start = 1;
    if (end < 1) end = 1;
    const int max_harm_req = start > end ? start : end;

    out.allocate(frameCount);
    const i32* sine = sine_table_4096();

    // Scratch: one frame of mip-0 sums in Q46 (amp Q16 * sine Q30).
    std::vector<i64> sum(TABLE_SIZE);
    std::vector<i32> amp(static_cast<size_t>(max_harm_req) + 1);

    constexpr i64 TARGET_Q30 = (static_cast<i64>(COEF_ONE) * 98) / 100;

    for (int f = 0; f < frameCount; ++f) {
        // Harmonic count for this frame: integer lerp start -> end.
        int n_f = start;
        if (frameCount > 1)
            n_f = start + static_cast<int>((static_cast<i64>(end - start) * f) / (frameCount - 1));
        if (n_f < 1) n_f = 1;
        for (int h = 1; h <= n_f; ++h) amp[h] = harmonic_amp(falloff, h);

        // Normalization scale comes from mip 0 (full harmonics) and is applied
        // to every mip of this frame, so mips keep consistent loudness.
        i32 scale_q16 = GAIN_ONE;
        for (int m = 0; m < MIP_LEVELS; ++m) {
            const int N = mip_table_size(m);
            const int step = 4096 / N;
            int limit = n_f;
            if (limit > HARM_LIMIT[m]) limit = HARM_LIMIT[m];
            if (limit > N / 2 - 1) limit = N / 2 - 1;
            if (limit < 1) limit = 1;

            for (int n = 0; n < N; ++n) sum[n] = 0;
            for (int h = 1; h <= limit; ++h) {
                if (amp[h] == 0) continue;
                const i64 a = amp[h];
                u32 idx = 0;
                const u32 inc = static_cast<u32>(h) * static_cast<u32>(step);
                for (int n = 0; n < N; ++n) {
                    sum[n] += a * sine[idx & 4095u];
                    idx += inc;
                }
            }
            if (m == 0) {
                i64 peak = 1;
                for (int n = 0; n < N; ++n) {
                    const i64 v = asr(sum[n], GAIN_SHIFT);
                    const i64 av = v < 0 ? -v : v;
                    if (av > peak) peak = av;
                }
                scale_q16 = static_cast<i32>((TARGET_Q30 << GAIN_SHIFT) / peak);
            }
            i32* dst = out.mutable_mip(m) + static_cast<size_t>(f) * N;
            for (int n = 0; n < N; ++n) {
                const i64 q30 = asr(sum[n], GAIN_SHIFT);
                dst[n] = static_cast<i32>(asr(q30 * scale_q16, GAIN_SHIFT));
            }
        }
    }
    return true;
}

}  // namespace

// Implemented in wavetable_gen_extra.cpp.
namespace wavetable_detail {
bool gen_pulse(const JsonValue*, int, WaveTable&, std::string*);
bool gen_drawbar(const JsonValue*, int, WaveTable&, std::string*);
bool gen_waveshaper(const JsonValue*, int, WaveTable&, std::string*);
bool gen_chebyshev(const JsonValue*, int, WaveTable&, std::string*);
bool gen_harmonics(const JsonValue*, int, WaveTable&, std::string*);
bool gen_hardsync(const JsonValue*, int, WaveTable&, std::string*);
bool gen_custom(const JsonValue*, int, WaveTable&, std::string*);
bool gen_fm2op(const JsonValue*, int, WaveTable&, std::string*);
}  // namespace wavetable_detail

namespace {

const std::map<std::string, GeneratorFn>& registry() {
    static const std::map<std::string, GeneratorFn> reg = {
        {"additiveHarmonicMorph", &gen_additive},
        {"pulse", &wavetable_detail::gen_pulse},
        {"drawbar", &wavetable_detail::gen_drawbar},
        {"waveshaper", &wavetable_detail::gen_waveshaper},
        {"chebyshev", &wavetable_detail::gen_chebyshev},
        {"harmonics", &wavetable_detail::gen_harmonics},
        {"hardsync", &wavetable_detail::gen_hardsync},
        {"custom", &wavetable_detail::gen_custom},
        {"fm2op", &wavetable_detail::gen_fm2op},
    };
    return reg;
}

}  // namespace

GeneratorFn wavetable_generator(const std::string& name) {
    auto it = registry().find(name);
    return it == registry().end() ? nullptr : it->second;
}

}  // namespace msynth
