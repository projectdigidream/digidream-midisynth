#pragma once
//
// Per-channel 3-band EQ (mixer strip, v1): low shelf @120 Hz, bell (settable
// centre), high shelf @6 kHz — the same parallel-form bands as the mastering
// chain, on a channel's stereo Q8.24 lane. All-zero gains are an exact
// no-op and the whole stage is skipped, so untouched channels cost nothing
// and existing renders stay byte-identical.
//
#include <initializer_list>

#include "filter.h"

namespace msynth {

struct ChannelEqParams {
    i32 low_db_q16 = 0;             // [-24, 24]
    i32 mid_db_q16 = 0;
    i32 mid_hz_q16 = 1000 << GAIN_SHIFT;  // [100, 8000]
    i32 high_db_q16 = 0;
    bool active() const noexcept { return low_db_q16 != 0 || mid_db_q16 != 0 || high_db_q16 != 0; }
};

class ChannelEq {
  public:
    void set_param(u8 id, i32 v) noexcept {
        switch (id) {
            case 0: p_.low_db_q16 = clamp_i32(v, -(24 << GAIN_SHIFT), 24 << GAIN_SHIFT); break;
            case 1: p_.mid_db_q16 = clamp_i32(v, -(24 << GAIN_SHIFT), 24 << GAIN_SHIFT); break;
            case 2: p_.mid_hz_q16 = clamp_i32(v, 100 << GAIN_SHIFT, 8000 << GAIN_SHIFT); break;
            case 3: p_.high_db_q16 = clamp_i32(v, -(24 << GAIN_SHIFT), 24 << GAIN_SHIFT); break;
            default: return;
        }
        recompute();
    }
    const ChannelEqParams& params() const noexcept { return p_; }
    bool active() const noexcept { return p_.active(); }

    void clear() noexcept {
        for (Band* b : {&low_, &mid_, &high_}) {
            b->l.reset(b->coefs);
            b->r.reset(b->coefs);
        }
    }

    // In-place on a channel's stereo lane (i64 Q8.24 accumulators).
    void process_block(i64* l, i64* r, int n) noexcept {
        for (Band* b : {&low_, &mid_, &high_}) {
            b->l.set_block_target(b->coefs, 6);
            b->r.set_block_target(b->coefs, 6);
        }
        constexpr i64 LIM = 32LL << SAMPLE_SHIFT;
        for (int i = 0; i < n; ++i) {
            const i32 xl = static_cast<i32>(clamp_i32(l[i], -LIM, LIM));
            const i32 xr = static_cast<i32>(clamp_i32(r[i], -LIM, LIM));
            i64 yl = l[i], yr = r[i];
            yl += asr(static_cast<i64>(low_.l.process(xl, FilterMode::Lowpass)) * low_.delta, GAIN_SHIFT);
            yr += asr(static_cast<i64>(low_.r.process(xr, FilterMode::Lowpass)) * low_.delta, GAIN_SHIFT);
            const i32 bl = static_cast<i32>(asr(static_cast<i64>(mid_.coefs.k) *
                                                mid_.l.process(xl, FilterMode::Bandpass), COEF_SHIFT));
            const i32 br = static_cast<i32>(asr(static_cast<i64>(mid_.coefs.k) *
                                                mid_.r.process(xr, FilterMode::Bandpass), COEF_SHIFT));
            yl += asr(static_cast<i64>(bl) * mid_.delta, GAIN_SHIFT);
            yr += asr(static_cast<i64>(br) * mid_.delta, GAIN_SHIFT);
            yl += asr(static_cast<i64>(high_.l.process(xl, FilterMode::Highpass)) * high_.delta, GAIN_SHIFT);
            yr += asr(static_cast<i64>(high_.r.process(xr, FilterMode::Highpass)) * high_.delta, GAIN_SHIFT);
            l[i] = yl;
            r[i] = yr;
        }
    }

  private:
    struct Band {
        FilterCoefs coefs;
        FilterState l, r;
        gain_t delta = 0;
    };
    void recompute() noexcept {
        constexpr coef_t K_SQRT2 = 1518500250;
        low_.coefs.compute(cutoff_g(cutoff_index_from_hz(120 << GAIN_SHIFT)), K_SQRT2);
        low_.delta = db_to_lin_q16(p_.low_db_q16) - GAIN_ONE;
        mid_.coefs.compute(cutoff_g(cutoff_index_from_hz(p_.mid_hz_q16)), COEF_ONE);  // Q = 1
        mid_.delta = db_to_lin_q16(p_.mid_db_q16) - GAIN_ONE;
        high_.coefs.compute(cutoff_g(cutoff_index_from_hz(6000 << GAIN_SHIFT)), K_SQRT2);
        high_.delta = db_to_lin_q16(p_.high_db_q16) - GAIN_ONE;
    }
    ChannelEqParams p_;
    Band low_, mid_, high_;
};

}  // namespace msynth
