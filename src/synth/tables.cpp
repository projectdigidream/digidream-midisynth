#include "tables.h"

namespace msynth {

namespace baked {
#include "tables_baked.inc"
}  // namespace baked

gain_t VEL_GAIN_Q16[128];
gain_t VEL_LINEAR_Q16[128];
gain_t VEL_SOFT_Q16[128];
static gain_t CURVE_LINEAR_Q16[257];
static bool s_inited = false;

void tables_init() noexcept {
    if (s_inited) return;
    s_inited = true;
    for (int v = 0; v < 128; ++v) {
        VEL_GAIN_Q16[v] = static_cast<gain_t>((static_cast<i64>(v) * v << GAIN_SHIFT) / (127 * 127));
        VEL_LINEAR_Q16[v] = static_cast<gain_t>((static_cast<i64>(v) << GAIN_SHIFT) / 127);
        // (v/127)^1.5 = v*sqrt(v) / (127*sqrt(127)), integer sqrt in Q8.
        const i64 sv = static_cast<i64>(dnp::isqrt128(static_cast<dnp::u128>(v) << 16));
        const i64 s127 = static_cast<i64>(dnp::isqrt128(static_cast<dnp::u128>(127) << 16));
        VEL_SOFT_Q16[v] = static_cast<gain_t>((static_cast<i64>(v) * sv << GAIN_SHIFT) / (127 * s127));
    }
    for (int i = 0; i <= 256; ++i) {
        CURVE_LINEAR_Q16[i] = static_cast<gain_t>((static_cast<i64>(i) << GAIN_SHIFT) / 256);
    }
}

u32 phase_inc_for_cents(i32 cents_q16) noexcept {
    // Clamp to the representable range: cents 0 .. 13199.99 (top of MIDI+bend).
    if (cents_q16 < 0) cents_q16 = 0;
    constexpr i32 MAX_C = (13200 << GAIN_SHIFT) - 1;
    if (cents_q16 > MAX_C) cents_q16 = MAX_C;

    const i32 c_int = cents_q16 >> GAIN_SHIFT;  // non-negative
    const u32 frac  = static_cast<u32>(cents_q16) & 0xFFFFu;
    const i32 oct   = c_int / 1200;
    const i32 rem   = c_int % 1200;

    const i32 ratio = lerp16(baked::CENTS_RATIO_Q30[rem], baked::CENTS_RATIO_Q30[rem + 1], frac);
    // inc = INC0 * ratio * 2^oct; INC0 carries 16 extra fraction bits, ratio 30.
    const dnp::u128 prod = static_cast<dnp::u128>(static_cast<u64>(baked::PITCH_INC0_Q16)) *
                           static_cast<u32>(ratio);
    return static_cast<u32>(prod >> (16 + 30 - oct));
}

void pan_gains(i32 pan_q16, gain_t& out_l, gain_t& out_r) noexcept {
    if (pan_q16 < -GAIN_ONE) pan_q16 = -GAIN_ONE;
    if (pan_q16 > GAIN_ONE) pan_q16 = GAIN_ONE;
    // Map [-1,1] -> quarter-turn index [0,1024] into the 4096-entry sine table.
    const i32 idx = static_cast<i32>((static_cast<i64>(pan_q16 + GAIN_ONE) * 1024) >> 17);
    // Constant power: L = cos, R = sin of the quarter-turn angle.
    const i32 sin_v = baked::SINE_Q30[idx];               // idx in [0,1024]
    const i32 cos_v = baked::SINE_Q30[1024 - idx];
    out_r = static_cast<gain_t>(asr(sin_v, COEF_SHIFT - GAIN_SHIFT));
    out_l = static_cast<gain_t>(asr(cos_v, COEF_SHIFT - GAIN_SHIFT));
}

const gain_t* curve_table(CurveId id) noexcept {
    switch (id) {
        case CurveId::Exp: return baked::CURVE_EXP_Q16;
        case CurveId::ExpAttack: return baked::CURVE_EXPATT_Q16;
        case CurveId::SCurve: return baked::CURVE_S_Q16;
        default: return CURVE_LINEAR_Q16;
    }
}

gain_t curve_eval(const gain_t* curve, u32 phase_q24) noexcept {
    if (phase_q24 >= (1u << 24)) return GAIN_ONE;
    const u32 idx = phase_q24 >> 16;            // 0..255
    const u32 frac = phase_q24 & 0xFFFFu;
    return lerp16(curve[idx], curve[idx + 1], frac);
}

coef_t cutoff_g(i32 index_q16) noexcept {
    if (index_q16 < 0) index_q16 = 0;
    constexpr i32 MAX_I = CUTOFF_STEPS << GAIN_SHIFT;
    if (index_q16 >= MAX_I) return baked::CUTOFF_G_Q30[CUTOFF_STEPS];
    const i32 idx = index_q16 >> GAIN_SHIFT;
    const u32 frac = static_cast<u32>(index_q16) & 0xFFFFu;
    return lerp16(baked::CUTOFF_G_Q30[idx], baked::CUTOFF_G_Q30[idx + 1], frac);
}

i32 log2_q16(u32 v_q16) noexcept {
    if (v_q16 == 0) return 0;
    // Integer part: position of the MSB relative to bit 16.
    int msb = 31;
    while (!(v_q16 & (1u << msb))) --msb;
    const i32 int_part = msb - 16;
    // Mantissa in Q30, in [1, 2).
    u64 mant = (static_cast<u64>(v_q16) << 30) >> msb;
    // Binary-search the EXP2_Q30 table (257 entries over one octave).
    int lo = 0, hi = 256;
    while (hi - lo > 1) {
        const int mid = (lo + hi) / 2;
        if (static_cast<u64>(baked::EXP2_Q30[mid]) <= mant) lo = mid;
        else hi = mid;
    }
    // Linear refine between table entries: frac16 within one 1/256 step.
    const i64 span = static_cast<i64>(baked::EXP2_Q30[lo + 1]) - baked::EXP2_Q30[lo];
    const i64 off  = static_cast<i64>(mant) - baked::EXP2_Q30[lo];
    const i64 step_frac = span > 0 ? (off << 8) / span : 0;  // Q8 within step
    return (int_part << GAIN_SHIFT) + (lo << 8) + static_cast<i32>(step_frac);
}

gain_t exp2_q16(i32 x_q16) noexcept {
    x_q16 = clamp_i32(x_q16, -(16 << GAIN_SHIFT), (15 << GAIN_SHIFT) - 1);
    const i32 ip = static_cast<i32>(asr(x_q16, GAIN_SHIFT));      // floor
    const u32 fp = static_cast<u32>(x_q16) & 0xFFFFu;             // frac in [0,1)
    // Mantissa 2^frac in Q30 from the 257-entry octave table (1/256 steps).
    const i32 mant = lerp16(baked::EXP2_Q30[fp >> 8], baked::EXP2_Q30[(fp >> 8) + 1],
                            (fp & 0xFFu) << 8);
    const int sh = 14 - ip;  // Q30 -> Q16 then scale by 2^ip
    if (sh >= 0) return static_cast<gain_t>(asr(mant, sh));
    return clamp_i32(static_cast<i64>(mant) << (-sh), 0, 0x7FFFFFFF);
}

gain_t db_to_lin_q16(i32 db_q16) noexcept {
    // log2(10)/20 = 0.16609640474... in Q24 = 2786513.
    return exp2_q16(static_cast<i32>(asr(static_cast<i64>(db_q16) * 2786513LL, 24)));
}

i32 lin_to_db_q16(u32 lin_q16) noexcept {
    if (lin_q16 == 0) return -(96 << GAIN_SHIFT);
    // dB = log2(x) * 20*log10(2) = log2(x) * 6.0206; 6.0206 in Q16 = 394565.
    return static_cast<i32>(asr(static_cast<i64>(log2_q16(lin_q16)) * 394565LL, GAIN_SHIFT));
}

gain_t onepole_coef_from_ms(i32 ms_q16) noexcept {
    const i64 tau_blocks_q16 = (static_cast<i64>(ms_q16) * CONTROL_RATE) / 1000;
    if (tau_blocks_q16 <= 0) return GAIN_ONE;
    // -log2(e)/tau; log2(e) in Q16 = 94548.
    const i64 x = -((94548LL << GAIN_SHIFT) / tau_blocks_q16);
    return GAIN_ONE - exp2_q16(clamp_i32(x, -(16 << GAIN_SHIFT), 0));
}

i32 cutoff_index_from_hz(i32 hz_q16) noexcept {
    if (hz_q16 <= (20 << GAIN_SHIFT)) return 0;
    const i32 lg = log2_q16(static_cast<u32>(hz_q16)) - log2_q16(20u << GAIN_SHIFT);
    // Span = log2(12000/20); scale to [0, 256].
    static const i32 span = log2_q16(12000u << GAIN_SHIFT) - log2_q16(20u << GAIN_SHIFT);
    // idx = lg / span * 256, in Q16: (lg << 24) / span.
    i64 idx = (static_cast<i64>(lg) << 24) / span;
    if (idx > (static_cast<i64>(CUTOFF_STEPS) << GAIN_SHIFT))
        idx = static_cast<i64>(CUTOFF_STEPS) << GAIN_SHIFT;
    return static_cast<i32>(idx);
}

i32 cutoff_hz_from_index(i32 index_q16) noexcept {
    // hz = 20 * 2^(index / steps_per_octave); span is log2(12000/20) octaves.
    index_q16 = clamp_i32(index_q16, 0, CUTOFF_STEPS << GAIN_SHIFT);
    const i64 oct_q16 = (static_cast<i64>(index_q16) << GAIN_SHIFT) / cutoff_steps_per_octave_q16();
    const gain_t mul = exp2_q16(static_cast<i32>(clamp_i32(oct_q16, 0, 10 << GAIN_SHIFT)));
    return static_cast<i32>(clamp_i32(20LL * mul, 20LL << GAIN_SHIFT, 12000LL << GAIN_SHIFT));
}

coef_t resonance_k(gain_t res_q16) noexcept {
    // k = 2 - 2*res, capped below self-oscillation (res <= 0.95 enforced by
    // the loader; clamp here anyway so the audio path can trust the range).
    if (res_q16 < 0) res_q16 = 0;
    const gain_t max_res = static_cast<gain_t>((95 * GAIN_ONE) / 100);
    if (res_q16 > max_res) res_q16 = max_res;
    const i64 k_q16 = (2LL << GAIN_SHIFT) - 2LL * res_q16;
    const i64 k_q30 = k_q16 << (COEF_SHIFT - GAIN_SHIFT);
    return k_q30 > 0x7FFFFFFFLL ? 0x7FFFFFFF : static_cast<coef_t>(k_q30);
}

const i32* sine_table_4096() noexcept { return baked::SINE_Q30; }

i32 cutoff_steps_per_octave_q16() noexcept {
    // 256 steps span log2(12000/20) octaves.
    static const i32 span = log2_q16(12000u << GAIN_SHIFT) - log2_q16(20u << GAIN_SHIFT);
    return static_cast<i32>((static_cast<i64>(CUTOFF_STEPS) << 32) / span);
}

i32 sine_q30(u32 phase) noexcept {
    const u32 idx = phase >> 20;                 // 4096-entry table
    const u32 frac = (phase >> 4) & 0xFFFFu;     // 16-bit intra-step fraction
    const i32 a = baked::SINE_Q30[idx];
    const i32 b = baked::SINE_Q30[(idx + 1) & 4095u];
    return lerp16(a, b, frac);
}

}  // namespace msynth
