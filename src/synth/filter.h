#pragma once
//
// Trapezoidal SVF (Cytomic/Simper form), Q2.30 coefficients and state
// (plan Appendix B/D). Stable across the whole cutoff range and under
// per-block coefficient modulation; lowpass/bandpass/highpass are taps on the
// same structure.
//
// Cutoff is capped at fs/4 (12 kHz) so g <= 1.0 and everything fits Q2.30.
// State saturation is always on: instability becomes brief bounded
// distortion, never full-scale wrap garbage.
//
#include "tables.h"

namespace msynth {

enum class FilterMode : u8 { Bypass, Lowpass, Bandpass, Highpass };

// k = 2.0 exactly does not fit Q2.30's [-2, 2); the max representable value
// is a Q of 0.5 plus one ulp -- indistinguishable.
inline constexpr coef_t COEF_MAX = 0x7FFFFFFF;

struct FilterCoefs {
    coef_t g = 0, k = COEF_MAX, a1 = COEF_ONE, a2 = 0;

    // One 64-bit division per voice per control block (Appendix D).
    void compute(coef_t g_in, coef_t k_in) noexcept {
        g = g_in;
        k = k_in;
        const i64 gk = asr(static_cast<i64>(g) * (static_cast<i64>(g) + k), COEF_SHIFT);
        a1 = static_cast<coef_t>((static_cast<i64>(COEF_ONE) << COEF_SHIFT) /
                                 (static_cast<i64>(COEF_ONE) + gk));
        a2 = static_cast<coef_t>(asr(static_cast<i64>(g) * a1, COEF_SHIFT));
    }
};

// Bound the recirculating states just under +/-2.0; bounding them bounds
// everything else.
inline i32 filt_sat(i64 v) noexcept {
    constexpr i64 LIM = (static_cast<i64>(COEF_ONE) << 1) - 1;
    return clamp_i32(v, -LIM, LIM);
}

struct FilterState {
    i32 ic1 = 0, ic2 = 0;
    // Current coefficients plus per-sample interpolation deltas toward the
    // block's target (kills zipper noise on fast sweeps).
    coef_t g = 0, k = COEF_MAX, a1 = COEF_ONE, a2 = 0;
    coef_t dg = 0, dk = 0, da1 = 0, da2 = 0;

    void reset(const FilterCoefs& c) noexcept {
        ic1 = ic2 = 0;
        g = c.g; k = c.k; a1 = c.a1; a2 = c.a2;
        dg = dk = da1 = da2 = 0;
    }

    // Per-sample deltas integrate on every process() call: the caller MUST
    // re-target once per control block (as Voice does) or coefficients drift
    // past the target.
    void set_block_target(const FilterCoefs& c, int block_samples_log2) noexcept {
        dg = static_cast<coef_t>(asr(static_cast<i64>(c.g) - g, block_samples_log2));
        dk = static_cast<coef_t>(asr(static_cast<i64>(c.k) - k, block_samples_log2));
        da1 = static_cast<coef_t>(asr(static_cast<i64>(c.a1) - a1, block_samples_log2));
        da2 = static_cast<coef_t>(asr(static_cast<i64>(c.a2) - a2, block_samples_log2));
    }

    // One sample. Input/output Q2.30. `mode` selects the output tap.
    inline i32 process(i32 x, FilterMode mode) noexcept {
        g += dg; k += dk; a1 += da1; a2 += da2;
        const i32 v3 = x - ic2;
        const i32 v1 = static_cast<i32>(
            asr(static_cast<i64>(a1) * ic1 + static_cast<i64>(a2) * v3, COEF_SHIFT));
        const i32 v2 = ic2 + static_cast<i32>(asr(static_cast<i64>(g) * v1, COEF_SHIFT));
        ic1 = filt_sat((static_cast<i64>(v1) << 1) - ic1);
        ic2 = filt_sat((static_cast<i64>(v2) << 1) - ic2);
        switch (mode) {
            case FilterMode::Lowpass: return v2;
            case FilterMode::Bandpass: return v1;
            case FilterMode::Highpass:
                return x - static_cast<i32>(asr(static_cast<i64>(k) * v1, COEF_SHIFT)) - v2;
            default: return x;
        }
    }
};

}  // namespace msynth
