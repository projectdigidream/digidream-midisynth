#pragma once
//
// Freeverb-style stereo reverb (plan §8): 8 parallel lowpass-feedback combs +
// 4 series allpasses per channel, with decorrelated (offset) delay lengths on
// the right channel -- that decorrelation is what makes it wide. No
// transcendental math anywhere; deterministic for free. Fixed-point is immune
// to the denormal tail slowdowns that plague float reverbs.
//
#include "delay_line.h"
#include "../filter.h"

namespace msynth {

struct ReverbParams {
    gain_t room_size = (GAIN_ONE * 8) / 10;  // 0..1
    gain_t damping = (GAIN_ONE * 4) / 10;    // 0..1
    gain_t width = GAIN_ONE;                 // 0..1
    gain_t wet_gain = (GAIN_ONE * 3) / 10;   // output level of the return
    // Both default to an exact bypass (0 = the stage is skipped entirely).
    gain_t pre_delay_ms_q16 = 0;   // 0..200 ms before the tail starts
    gain_t high_pass_hz_q16 = 0;   // 0..1000 Hz on the send: keeps bass dry
};

class Reverb {
  public:
    Reverb() { set_params(ReverbParams{}); }

    void set_params(const ReverbParams& p) noexcept;
    void clear() noexcept;

    // Process one control block of the send bus (i64 Q8.24 accumulators),
    // adding the wet return into the master accumulators.
    void process_block(const i64* in_l, const i64* in_r, i64* out_l, i64* out_r,
                       int n) noexcept;

  private:
    static constexpr int NUM_COMBS = 8;
    static constexpr int NUM_ALLPASS = 4;

    struct Comb {
        DelayLine<11> line;  // up to 2048 samples
        i32 filter_store = 0;
        u32 len = 1;
    };
    struct Allpass {
        DelayLine<10> line;
        u32 len = 1;
    };

    Comb comb_[2][NUM_COMBS];
    Allpass allpass_[2][NUM_ALLPASS];
    gain_t feedback_ = 0;
    gain_t damp1_ = 0, damp2_ = GAIN_ONE;
    gain_t wet1_ = 0, wet2_ = 0;  // width mixing

    // Send conditioning, both on the mono sum ahead of the tail: pre-delay
    // keeps the tail off the transient, the highpass keeps sub and kick out
    // of it. Zero on either means that stage is not executed at all.
    DelayLine<14> pre_;           // 16384 samples = 341 ms
    u32 pre_len_ = 0;
    FilterCoefs hp_coefs_;
    FilterState hp_;
    bool hp_on_ = false;
};

}  // namespace msynth
