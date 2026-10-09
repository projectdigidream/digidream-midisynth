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
// Input cycle for the shaper family, Q2.30 at sample n of GEN_N.
inline i64 shaper_input(int kind /*0 sine, 1 triangle, 2 saw*/, const i32* sine, int n) {
    switch (kind) {
        case 1: {  // triangle: -1 at n=0, +1 at N/2, back to -1
            const i64 q = (static_cast<i64>(n) * 4 * COEF_ONE) / GEN_N;
            return q < 2 * COEF_ONE ? q - COEF_ONE : 3LL * COEF_ONE - q;
        }
        case 2:  // rising saw -1..1 (the DFT resynthesis band-limits it)
            return (static_cast<i64>(n) * 2 * COEF_ONE) / GEN_N - COEF_ONE;
        default:
            return sine[n * 2];
    }
}

bool gen_waveshaper(const JsonValue* params, int frames, WaveTable& out, std::string* err) {
    enum class Shape { Soft, Hard, Fold, Crush, Stairs };
    Shape shape = Shape::Soft;
    i32 drive0 = GAIN_ONE, drive1 = 8 * GAIN_ONE;
    i32 bias = 0;
    int input = 0;
    int levels = 6;   // crush: output amplitude levels per side
    int steps = 16;   // stairs: phase sample-holds per cycle
    if (params) {
        const std::string s = params->get_str("shape", "soft");
        if (s == "soft") shape = Shape::Soft;
        else if (s == "hard") shape = Shape::Hard;
        else if (s == "fold") shape = Shape::Fold;
        else if (s == "crush") shape = Shape::Crush;
        else if (s == "stairs") shape = Shape::Stairs;
        else {
            if (err) *err = "unknown waveshaper shape '" + s + "'";
            return false;
        }
        levels = static_cast<int>(clamp_i32(params->get_int("levels", 6), 2, 32));
        steps = static_cast<int>(clamp_i32(params->get_int("steps", 16), 2, 64));
        const std::string in = params->get_str("input", "sine");
        if (in == "sine") input = 0;
        else if (in == "triangle") input = 1;
        else if (in == "saw") input = 2;
        else {
            if (err) *err = "unknown waveshaper input '" + in + "'";
            return false;
        }
        drive0 = params->get_q16("startDrive", drive0);
        drive1 = params->get_q16("endDrive", drive1);
        bias = clamp_i32(params->get_q16("bias", 0), -GAIN_ONE, GAIN_ONE);
    }
    drive0 = clamp_i32(drive0, GAIN_ONE / 4, 64 * GAIN_ONE);
    drive1 = clamp_i32(drive1, GAIN_ONE / 4, 64 * GAIN_ONE);
    const i64 bias_q30 = static_cast<i64>(bias) << (COEF_SHIFT - GAIN_SHIFT);

    const i32* sine = sine_table_4096();
    out.allocate(frames);
    i64 wave[GEN_N];
    for (int f = 0; f < frames; ++f) {
        const i32 drive = lerp_q16(drive0, drive1, frame_t_q16(f, frames));
        for (int n = 0; n < GEN_N; ++n) {
            // Stairs: sample-hold the phase before anything else (a digital
            // "steppy" read of the input wave).
            const int sn = shape == Shape::Stairs ? (n / (GEN_N / steps)) * (GEN_N / steps) : n;
            // Bias shifts the operating point into the curve's asymmetric
            // region: even harmonics appear (DC is removed afterwards).
            i64 x = asr(shaper_input(input, sine, sn) * drive, GAIN_SHIFT) + bias_q30;  // Q30
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
                case Shape::Crush: {
                    // Hard clip, then quantize to `levels` amplitude steps
                    // per side (bit-crusher transfer, heavy digital buzz).
                    x = clamp_i32(x, -COEF_ONE, COEF_ONE);
                    const i64 q = COEF_ONE / levels;
                    x = (x >= 0 ? (x + q / 2) / q : -((-x + q / 2) / q)) * q;
                    break;
                }
                case Shape::Stairs:
                    x = clamp_i32(x, -COEF_ONE, COEF_ONE);  // held phase + clip
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

// ---------------------------------------------------------------------------
// Chebyshev waveshaping: sum of weights on T1..T8 applied to a driven cosine.
// T_n(cos t) = cos(n t), so at drive 1.0 weight n IS harmonic n exactly; at
// drive < 1 the polynomials only partially develop and the upper partials
// fade nonlinearly -- the classic waveshaping-synthesis "index" control.
// drive morphs startDrive -> endDrive across frames (weights optionally too).
bool gen_chebyshev(const JsonValue* params, int frames, WaveTable& out, std::string* err) {
    constexpr int NW = 8;
    i32 w0[NW] = {GAIN_ONE, 0, 0, 0, 0, 0, 0, 0};
    i32 w1[NW];
    bool has_end = false;
    i32 drive0 = GAIN_ONE, drive1 = GAIN_ONE;
    auto read_weights = [&](const JsonValue* arr, i32* dst) {
        for (int i = 0; i < NW; ++i)
            dst[i] = clamp_i32(i < static_cast<int>(arr->arr.size()) ? arr->arr[i]->num_q16 : 0,
                               -GAIN_ONE, GAIN_ONE);
    };
    if (params) {
        const JsonValue* w = params->get("weights");
        if (w && w->is_arr()) read_weights(w, w0);
        else if (w) {
            if (err) *err = "chebyshev weights must be an array";
            return false;
        }
        const JsonValue* e = params->get("endWeights");
        if (e && e->is_arr()) {
            read_weights(e, w1);
            has_end = true;
        }
        drive0 = clamp_i32(params->get_q16("startDrive", GAIN_ONE), 0, GAIN_ONE);
        drive1 = clamp_i32(params->get_q16("endDrive", drive0), 0, GAIN_ONE);
    }
    if (!has_end)
        for (int i = 0; i < NW; ++i) w1[i] = w0[i];
    bool any = false;
    for (int i = 0; i < NW; ++i) any = any || w0[i] != 0 || w1[i] != 0;
    if (!any) w0[0] = w1[0] = GAIN_ONE;  // all-zero recipe: fall back to a sine

    const i32* sine = sine_table_4096();
    out.allocate(frames);
    i64 wave[GEN_N];
    for (int f = 0; f < frames; ++f) {
        const i32 t = frame_t_q16(f, frames);
        const i32 drive = lerp_q16(drive0, drive1, t);
        i32 w[NW];
        for (int i = 0; i < NW; ++i) w[i] = lerp_q16(w0[i], w1[i], t);
        for (int n = 0; n < GEN_N; ++n) {
            // x = drive * cos(theta), Q2.30; recurrence T_{k+1} = 2 x T_k - T_{k-1}.
            const i64 x = asr(static_cast<i64>(sine[(n * 2 + 1024) & 4095]) * drive, GAIN_SHIFT);
            i64 tkm1 = COEF_ONE;  // T_0
            i64 tk = x;           // T_1
            i64 acc = 0;          // Q46 (Q16 weight x Q30 value)
            for (int k = 0; k < NW; ++k) {
                if (w[k] != 0) acc += static_cast<i64>(w[k]) * tk;
                const i64 tn = asr(2 * x * tk, COEF_SHIFT) - tkm1;
                tkm1 = tk;
                tk = tn;
            }
            wave[n] = asr(acc, GAIN_SHIFT);  // back to Q30
        }
        remove_dc(wave);
        Spectrum spec;
        analyze(wave, spec, MAX_ANALYSIS_H);
        synthesize_frame(out, f, spec);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Direct additive recipe: amplitudes for harmonics 1..64 (the general tool
// the drawbar is a preset of). Spectrum-direct: no analysis pass needed.
bool gen_harmonics(const JsonValue* params, int frames, WaveTable& out, std::string* err) {
    constexpr int NH = 64;
    i32 a0[NH] = {};
    i32 a1[NH];
    bool has_end = false;
    a0[0] = GAIN_ONE;
    auto read_amps = [&](const JsonValue* arr, i32* dst) {
        for (int i = 0; i < NH; ++i)
            dst[i] = clamp_i32(i < static_cast<int>(arr->arr.size()) ? arr->arr[i]->num_q16 : 0,
                               0, GAIN_ONE);
    };
    if (params) {
        const JsonValue* a = params->get("amps");
        if (a && a->is_arr()) read_amps(a, a0);
        else if (a) {
            if (err) *err = "harmonics amps must be an array";
            return false;
        }
        const JsonValue* e = params->get("endAmps");
        if (e && e->is_arr()) {
            read_amps(e, a1);
            has_end = true;
        }
    }
    if (!has_end)
        for (int i = 0; i < NH; ++i) a1[i] = a0[i];
    bool any = false;
    for (int i = 0; i < NH; ++i) any = any || a0[i] != 0 || a1[i] != 0;
    if (!any) a0[0] = a1[0] = GAIN_ONE;  // silence recipe -> sine

    out.allocate(frames);
    for (int f = 0; f < frames; ++f) {
        Spectrum spec{};
        spec.max_h = NH;
        const i32 t = frame_t_q16(f, frames);
        for (int i = 0; i < NH; ++i) spec.s[i + 1] = lerp_q16(a0[i], a1[i], t);
        synthesize_frame(out, f, spec);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Hard sync: a slave oscillator running at `ratio` x the fundamental, phase-
// reset every master period. The reset discontinuity is what screams; the
// DFT resynthesis keeps it band-limited. Ratio morphs across frames (the
// classic sync sweep); fractional ratios are fine -- the cycle is periodic
// at the master period by construction.
bool gen_hardsync(const JsonValue* params, int frames, WaveTable& out, std::string* err) {
    i32 ratio0 = GAIN_ONE, ratio1 = 3 * GAIN_ONE;
    int slave = 0;  // 0 saw, 1 pulse
    if (params) {
        ratio0 = params->get_q16("startRatio", ratio0);
        ratio1 = params->get_q16("endRatio", ratio1);
        const std::string s = params->get_str("slave", "saw");
        if (s == "saw") slave = 0;
        else if (s == "pulse") slave = 1;
        else {
            if (err) *err = "unknown hardsync slave '" + s + "'";
            return false;
        }
    }
    ratio0 = clamp_i32(ratio0, GAIN_ONE, 16 * GAIN_ONE);
    ratio1 = clamp_i32(ratio1, GAIN_ONE, 16 * GAIN_ONE);

    out.allocate(frames);
    i64 wave[GEN_N];
    for (int f = 0; f < frames; ++f) {
        const i32 ratio = lerp_q16(ratio0, ratio1, frame_t_q16(f, frames));
        for (int n = 0; n < GEN_N; ++n) {
            // Slave phase in Q16 cycles: frac(n/GEN_N * ratio).
            const i64 ph = (static_cast<i64>(n) * ratio / GEN_N) & 0xFFFF;
            wave[n] = slave == 0 ? (ph * 2 * COEF_ONE >> GAIN_SHIFT) - COEF_ONE
                                 : (ph < GAIN_ONE / 2 ? COEF_ONE : -COEF_ONE);
        }
        remove_dc(wave);
        Spectrum spec;
        analyze(wave, spec, MAX_ANALYSIS_H);
        synthesize_frame(out, f, spec);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Custom single cycle: raw samples (up to 2048, -1..1) resampled to the
// analysis cycle and band-limited like every nonlinear generator -- the
// "draw it / import it" recipe. `endSamples` morphs across frames.
bool gen_custom(const JsonValue* params, int frames, WaveTable& out, std::string* err) {
    std::vector<i64> a, b;
    auto read_samples = [&](const JsonValue* arr, std::vector<i64>& dst) {
        dst.clear();
        const size_t n = arr->arr.size() > 2048 ? 2048 : arr->arr.size();
        dst.reserve(n);
        for (size_t i = 0; i < n; ++i)
            dst.push_back(asl(static_cast<i64>(clamp_i32(arr->arr[i]->num_q16, -GAIN_ONE, GAIN_ONE)),
                              COEF_SHIFT - GAIN_SHIFT));  // Q16 -> Q30
    };
    if (params) {
        const JsonValue* s = params->get("samples");
        if (s && s->is_arr()) read_samples(s, a);
        else if (s) {
            if (err) *err = "custom samples must be an array";
            return false;
        }
        const JsonValue* e = params->get("endSamples");
        if (e && e->is_arr()) read_samples(e, b);
    }
    if (a.size() < 2) {
        if (err) *err = "custom generator needs a samples array (2..2048 values)";
        return false;
    }
    if (b.size() < 2) b = a;

    // Linear resample a source cycle to GEN_N points.
    auto at = [](const std::vector<i64>& src, int n) -> i64 {
        const u64 pos = (static_cast<u64>(n) * src.size() << GAIN_SHIFT) / GEN_N;
        const size_t i0 = static_cast<size_t>(pos >> GAIN_SHIFT) % src.size();
        const size_t i1 = (i0 + 1) % src.size();
        const i64 frac = static_cast<i64>(pos & 0xFFFF);
        return src[i0] + asr((src[i1] - src[i0]) * frac, GAIN_SHIFT);
    };
    out.allocate(frames);
    i64 wave[GEN_N];
    for (int f = 0; f < frames; ++f) {
        const i32 t = frame_t_q16(f, frames);
        for (int n = 0; n < GEN_N; ++n) {
            const i64 va = at(a, n), vb = at(b, n);
            wave[n] = va + asr((vb - va) * t, GAIN_SHIFT);
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
