//
// Additional wavetable generators (plan §19's open decision, resolved):
//
//   pulse           -- PWM morph: duty cycle sweeps across frames
//   drawbar         -- organ drawbars: 9 harmonic levels, morphable
//   waveshaper      -- sine through a nonlinear shape, drive morphs (the
//                      plan's "waveshaping-based" second generator)
//   fm2op           -- 2-operator FM, integer ratio, index morphs
//
// Band-limiting strategy: generators that create harmonics nonlinearly
// (pulse, waveshaper, fm2op) render one high-resolution cycle, take a naive
// integer DFT (control thread, deterministic -- baked sine table, i128
// accumulation), then RESYNTHESIZE each mip additively under its harmonic
// cap. Aliasing cannot survive that path regardless of the shape. drawbar
// specifies its spectrum directly.
//
#include <vector>

#include "tables.h"
#include "wavetable_gen.h"

namespace msynth {
namespace wavetable_detail {

// Harmonics analyzed/resynthesized for DFT-based generators. Sub-bass
// fundamentals could in principle carry up to HARM_LIMIT[0]=366, but 256
// covers every audible partial of these timbres.
constexpr int MAX_ANALYSIS_H = 256;
constexpr int GEN_N = 2048;  // analysis cycle length (= mip 0 size)

struct Spectrum {
    // Q16 sine/cosine amplitudes per harmonic (index 1..max_h).
    i64 s[MAX_ANALYSIS_H + 1];
    i64 c[MAX_ANALYSIS_H + 1];
    int max_h = 0;
};

// wave: one cycle, Q2.30, GEN_N samples. DC is removed here.
void analyze(const i64* wave, Spectrum& out, int max_h) {
    const i32* sine = sine_table_4096();
    out.max_h = max_h;
    for (int h = 1; h <= max_h; ++h) {
        dnp::i128 acc_s = 0, acc_c = 0;
        u32 idx = 0;
        const u32 inc = static_cast<u32>(h) * (4096 / GEN_N);
        for (int n = 0; n < GEN_N; ++n) {
            acc_s += static_cast<dnp::i128>(wave[n]) * sine[idx & 4095u];
            acc_c += static_cast<dnp::i128>(wave[n]) * sine[(idx + 1024) & 4095u];
            idx += inc;
        }
        // wave Q30 x sine Q30 summed over N: a pure unit sine accumulates
        // 2^60 * N/2 = 2^70. Q16 amplitude = acc >> (60 + log2(N/2) - 16).
        out.s[h] = static_cast<i64>(dnp::tsr128(acc_s, 60 + 10 - 16));
        out.c[h] = static_cast<i64>(dnp::tsr128(acc_c, 60 + 10 - 16));
    }
}

// Build every mip of frame `f` from a spectrum; normalization scale comes
// from mip 0 and is applied to all mips (consistent loudness across mips,
// same rule as the additive generator).
void synthesize_frame(WaveTable& out, int f, const Spectrum& spec) {
    const i32* sine = sine_table_4096();
    std::vector<i64> sum(TABLE_SIZE);
    constexpr i64 TARGET_Q30 = (static_cast<i64>(COEF_ONE) * 98) / 100;
    i32 scale_q16 = GAIN_ONE;
    for (int m = 0; m < MIP_LEVELS; ++m) {
        const int N = mip_table_size(m);
        const int step = 4096 / N;
        int limit = spec.max_h;
        if (limit > HARM_LIMIT[m]) limit = HARM_LIMIT[m];
        if (limit > N / 2 - 1) limit = N / 2 - 1;
        if (limit < 1) limit = 1;

        for (int n = 0; n < N; ++n) sum[n] = 0;
        for (int h = 1; h <= limit; ++h) {
            const i64 as = spec.s[h], ac = spec.c[h];
            if (as == 0 && ac == 0) continue;
            u32 idx = 0;
            const u32 inc = static_cast<u32>(h) * static_cast<u32>(step);
            for (int n = 0; n < N; ++n) {
                sum[n] += as * sine[idx & 4095u] + ac * sine[(idx + 1024) & 4095u];
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
        for (int n = 0; n < N; ++n)
            dst[n] = static_cast<i32>(asr(asr(sum[n], GAIN_SHIFT) * scale_q16, GAIN_SHIFT));
    }
}

void remove_dc(i64* wave) {
    i64 mean = 0;
    for (int n = 0; n < GEN_N; ++n) mean += wave[n];
    mean = asr(mean, 11);  // / GEN_N, exact shift
    for (int n = 0; n < GEN_N; ++n) wave[n] -= mean;
}

// Frame position 0..1 in Q16 for frame f of frameCount.
i32 frame_t_q16(int f, int frames) {
    return frames > 1 ? static_cast<i32>((static_cast<i64>(f) << GAIN_SHIFT) / (frames - 1)) : 0;
}
i32 lerp_q16(i32 a, i32 b, i32 t_q16) {
    return a + static_cast<i32>(asr(static_cast<i64>(b - a) * t_q16, GAIN_SHIFT));
}

// ---------------------------------------------------------------------------
bool gen_pulse(const JsonValue* params, int frames, WaveTable& out, std::string*) {
    i32 duty0 = GAIN_ONE / 2, duty1 = GAIN_ONE / 8;
    if (params) {
        duty0 = params->get_q16("startDuty", duty0);
        duty1 = params->get_q16("endDuty", duty1);
    }
    const i32 lo = GAIN_ONE / 50, hi = GAIN_ONE - GAIN_ONE / 50;
    duty0 = clamp_i32(duty0, lo, hi);
    duty1 = clamp_i32(duty1, lo, hi);

    out.allocate(frames);
    i64 wave[GEN_N];
    for (int f = 0; f < frames; ++f) {
        const i32 duty = lerp_q16(duty0, duty1, frame_t_q16(f, frames));
        const int edge = static_cast<int>((static_cast<i64>(duty) * GEN_N) >> GAIN_SHIFT);
        for (int n = 0; n < GEN_N; ++n) wave[n] = n < edge ? COEF_ONE : -COEF_ONE;
        remove_dc(wave);
        Spectrum spec;
        analyze(wave, spec, MAX_ANALYSIS_H);
        synthesize_frame(out, f, spec);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Drawbars map to harmonics 1,2,3,4,5,6,8,10,12 (a compact take on the
// classic footages), each 0..8; level follows the organ's roughly -3dB/stop
// law approximated as (v/8)^2. Optional endDrawbars morphs across frames.
bool gen_drawbar(const JsonValue* params, int frames, WaveTable& out, std::string* err) {
    constexpr int BAR_H[9] = {1, 2, 3, 4, 5, 6, 8, 10, 12};
    i32 bars0[9] = {8, 0, 6, 0, 0, 4, 0, 0, 0};
    i32 bars1[9];
    bool has_end = false;

    auto read_bars = [&](const JsonValue* arr, i32* dst) {
        for (int i = 0; i < 9; ++i)
            dst[i] = clamp_i32(
                i < static_cast<int>(arr->arr.size()) ? arr->arr[i]->num_q16 : 0, 0,
                8 * GAIN_ONE);
    };
    if (params) {
        const JsonValue* b = params->get("drawbars");
        if (b && b->is_arr()) read_bars(b, bars0);
        else if (b) {
            if (err) *err = "drawbars must be an array";
            return false;
        }
        const JsonValue* e = params->get("endDrawbars");
        if (e && e->is_arr()) {
            read_bars(e, bars1);
            has_end = true;
        }
    }
    if (!has_end)
        for (int i = 0; i < 9; ++i) bars1[i] = bars0[i];

    out.allocate(frames);
    for (int f = 0; f < frames; ++f) {
        Spectrum spec{};
        spec.max_h = 12;
        const i32 t = frame_t_q16(f, frames);
        for (int i = 0; i < 9; ++i) {
            const i32 v = lerp_q16(bars0[i], bars1[i], t);  // 0..8 in Q16
            // (v/8)^2 in Q16: (v*v / 64) >> 16
            spec.s[BAR_H[i]] += asr(static_cast<i64>(v) * v / 64, GAIN_SHIFT);
        }
        synthesize_frame(out, f, spec);
    }
    return true;
}

// ---------------------------------------------------------------------------
bool gen_waveshaper(const JsonValue* params, int frames, WaveTable& out, std::string* err) {
    enum class Shape { Soft, Hard, Fold };
    Shape shape = Shape::Soft;
    i32 drive0 = GAIN_ONE, drive1 = 8 * GAIN_ONE;
    if (params) {
        const std::string s = params->get_str("shape", "soft");
        if (s == "soft") shape = Shape::Soft;
        else if (s == "hard") shape = Shape::Hard;
        else if (s == "fold") shape = Shape::Fold;
        else {
            if (err) *err = "unknown waveshaper shape '" + s + "'";
            return false;
        }
        drive0 = params->get_q16("startDrive", drive0);
        drive1 = params->get_q16("endDrive", drive1);
    }
    drive0 = clamp_i32(drive0, GAIN_ONE / 4, 64 * GAIN_ONE);
    drive1 = clamp_i32(drive1, GAIN_ONE / 4, 64 * GAIN_ONE);

    const i32* sine = sine_table_4096();
    out.allocate(frames);
    i64 wave[GEN_N];
    for (int f = 0; f < frames; ++f) {
        const i32 drive = lerp_q16(drive0, drive1, frame_t_q16(f, frames));
        for (int n = 0; n < GEN_N; ++n) {
            i64 x = asr(static_cast<i64>(sine[n * 2]) * drive, GAIN_SHIFT);  // Q30, +/-drive
            switch (shape) {
                case Shape::Soft:
                    // y = 1.5x - 0.5x^3 inside +/-1, +/-1 outside.
                    if (x > COEF_ONE) x = COEF_ONE;
                    else if (x < -COEF_ONE) x = -COEF_ONE;
                    else {
                        const i64 x3 = asr(asr(x * x, COEF_SHIFT) * x, COEF_SHIFT);
                        x = asr(3 * x - x3, 1);
                    }
                    break;
                case Shape::Hard:
                    x = clamp_i32(x, -COEF_ONE, COEF_ONE);
                    break;
                case Shape::Fold:
                    // Reflect into [-1, 1]; bounded iterations for any drive.
                    for (int guard = 0; guard < 64; ++guard) {
                        if (x > COEF_ONE) x = 2LL * COEF_ONE - x;
                        else if (x < -COEF_ONE) x = -2LL * COEF_ONE - x;
                        else break;
                    }
                    break;
            }
            wave[n] = x;
        }
        remove_dc(wave);
        Spectrum spec;
        analyze(wave, spec, MAX_ANALYSIS_H);
        synthesize_frame(out, f, spec);
    }
    return true;
}

// ---------------------------------------------------------------------------
// 2-operator FM: carrier at the fundamental, modulator at an INTEGER ratio
// (keeps the cycle periodic in the table), modulation index morphs across
// frames. Index 1.0 in radians = 4096/(2*pi) = 651.8986... table steps;
// baked as Q16 (42722830) so the phase math is pure integers.
bool gen_fm2op(const JsonValue* params, int frames, WaveTable& out, std::string*) {
    int ratio = 2;
    i32 idx0 = GAIN_ONE / 2, idx1 = 4 * GAIN_ONE;
    if (params) {
        ratio = static_cast<int>(params->get_int("ratio", 2));
        idx0 = params->get_q16("startIndex", idx0);
        idx1 = params->get_q16("endIndex", idx1);
    }
    ratio = static_cast<int>(clamp_i32(ratio, 1, 16));
    idx0 = clamp_i32(idx0, 0, 12 * GAIN_ONE);
    idx1 = clamp_i32(idx1, 0, 12 * GAIN_ONE);
    constexpr i64 STEPS_PER_RAD_Q16 = 42722830LL;  // 4096/(2*pi) in Q16

    const i32* sine = sine_table_4096();
    out.allocate(frames);
    i64 wave[GEN_N];
    for (int f = 0; f < frames; ++f) {
        const i32 index = lerp_q16(idx0, idx1, frame_t_q16(f, frames));
        // Phase offset amplitude in table steps, Q16.
        const i64 amp_steps_q16 = asr(static_cast<i64>(index) * STEPS_PER_RAD_Q16, GAIN_SHIFT);
        for (int n = 0; n < GEN_N; ++n) {
            const i32 mod = sine[(n * 2 * ratio) & 4095];  // Q30
            const i64 off_q16 = asr(amp_steps_q16 * mod, COEF_SHIFT);
            // Carrier phase in Q16 table steps, fractional lookup.
            const i64 phase_q16 = (static_cast<i64>(n) * 2 << GAIN_SHIFT) + off_q16;
            const u32 pi = static_cast<u32>(asr(phase_q16, GAIN_SHIFT)) & 4095u;
            const u32 frac = static_cast<u32>(phase_q16 & 0xFFFF);
            wave[n] = lerp16(sine[pi], sine[(pi + 1) & 4095u], frac);
        }
        remove_dc(wave);
        Spectrum spec;
        analyze(wave, spec, MAX_ANALYSIS_H);
        synthesize_frame(out, f, spec);
    }
    return true;
}

}  // namespace wavetable_detail
}  // namespace msynth
