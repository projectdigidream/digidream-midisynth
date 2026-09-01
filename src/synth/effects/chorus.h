#pragma once
//
// Stereo chorus: modulated delay with opposed LFO phases per channel
// (plan §5/§8). Wet-only return (it lives on a send bus).
//
#include "delay_line.h"
#include "../tables.h"

namespace msynth {

struct ChorusParams {
    gain_t rate_hz_q16 = (GAIN_ONE * 8) / 10;   // 0.8 Hz
    gain_t depth_ms_q16 = (GAIN_ONE * 7) / 2;   // 3.5 ms
    gain_t wet_gain = (GAIN_ONE * 2) / 10;
};

class Chorus {
  public:
    Chorus() { set_params(ChorusParams{}); }

    void set_params(const ChorusParams& p) noexcept {
        // Phase increment per sample.
        inc_ = static_cast<u32>((static_cast<u64>(static_cast<u32>(p.rate_hz_q16)) << 16) /
                                SAMPLE_RATE);
        // Depth in samples, Q16.16: ms * 48.
        depth_q16_ = static_cast<u32>(static_cast<i64>(p.depth_ms_q16) * 48);
        wet_ = p.wet_gain;
    }

    void clear() noexcept {
        line_l_.clear();
        line_r_.clear();
        phase_ = 0;
    }

    void process_block(const i64* in_l, const i64* in_r, i64* out_l, i64* out_r,
                       int n) noexcept {
        // Base delay 15 ms; modulation swings around it.
        constexpr u32 BASE_Q16 = (15 * 48) << 16;
        for (int i = 0; i < n; ++i) {
            line_l_.push(clamp_i32(in_l[i], -(8LL << SAMPLE_SHIFT), 8LL << SAMPLE_SHIFT));
            line_r_.push(clamp_i32(in_r[i], -(8LL << SAMPLE_SHIFT), 8LL << SAMPLE_SHIFT));
            phase_ += inc_;
            // Opposed LFO phases decorrelate the channels.
            const i32 m_l = sine_q30(phase_);
            const i32 m_r = sine_q30(phase_ + 0x80000000u);
            const u32 d_l = BASE_Q16 + static_cast<u32>(
                asr(static_cast<i64>(m_l) * static_cast<i64>(depth_q16_), COEF_SHIFT));
            const u32 d_r = BASE_Q16 + static_cast<u32>(
                asr(static_cast<i64>(m_r) * static_cast<i64>(depth_q16_), COEF_SHIFT));
            out_l[i] += asr(static_cast<i64>(line_l_.read_lerp(d_l)) * wet_, GAIN_SHIFT);
            out_r[i] += asr(static_cast<i64>(line_r_.read_lerp(d_r)) * wet_, GAIN_SHIFT);
        }
    }

  private:
    DelayLine<11> line_l_, line_r_;  // 2048 samples ≈ 42 ms
    u32 phase_ = 0;
    u32 inc_ = 0;
    u32 depth_q16_ = 0;
    gain_t wet_ = 0;
};

}  // namespace msynth
