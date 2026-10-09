#pragma once
//
// Stereo delay/echo send effect: a feedback delay line per channel with a
// gentle damping one-pole in the loop and optional ping-pong cross-feedback.
// Wet-only return (it lives on a send bus, CC94), like reverb and chorus.
//
#include "delay_line.h"
#include "../tables.h"

namespace msynth {

struct DelayParams {
    gain_t time_ms_q16 = 320 << GAIN_SHIFT;   // 1..640 ms (used when sync = 0)
    gain_t feedback_q16 = (GAIN_ONE * 35) / 100;  // 0..0.9
    gain_t wet_gain = 0;                      // 0..2 (0 = effect off)
    bool ping_pong = false;                   // feedback crosses channels
    // Cycle length in beats (Q16) from a "1/8." style spec; 0 = free ms.
    // A dotted-eighth at 174 BPM is 259 ms -- the liquid DnB echo.
    i32 sync_beats_q16 = 0;
};

class DelayFx {
  public:
    DelayFx() { set_params(DelayParams{}); }

    void set_params(const DelayParams& p) noexcept {
        p_ = p;
        feedback_ = clamp_i32(p.feedback_q16, 0, (GAIN_ONE * 9) / 10);
        wet_ = clamp_i32(p.wet_gain, 0, 2 * GAIN_ONE);
        ping_pong_ = p.ping_pong;
        recompute_delay();
    }

    // The sequencer pushes tempo on every change; a synced delay follows it.
    void set_tempo(gain_t bpm_q16) noexcept {
        if (bpm_q16 == bpm_q16_) return;
        bpm_q16_ = bpm_q16;
        recompute_delay();
    }

    void clear() noexcept {
        line_l_.clear();
        line_r_.clear();
        damp_l_ = damp_r_ = 0;
    }

    void process_block(const i64* in_l, const i64* in_r, i64* out_l, i64* out_r,
                       int n) noexcept {
        if (wet_ == 0) return;  // silent send: keep the lines cold
        constexpr i64 LIM = 8LL << SAMPLE_SHIFT;
        for (int i = 0; i < n; ++i) {
            const i32 tap_l = line_l_.read(delay_);
            const i32 tap_r = line_r_.read(delay_);
            // One-pole damping in the loop so repeats darken naturally.
            damp_l_ += asr(static_cast<i64>(tap_l - damp_l_) * DAMP_COEF, GAIN_SHIFT);
            damp_r_ += asr(static_cast<i64>(tap_r - damp_r_) * DAMP_COEF, GAIN_SHIFT);
            const i64 fb_l = asr(static_cast<i64>(ping_pong_ ? damp_r_ : damp_l_) * feedback_, GAIN_SHIFT);
            const i64 fb_r = asr(static_cast<i64>(ping_pong_ ? damp_l_ : damp_r_) * feedback_, GAIN_SHIFT);
            line_l_.push(static_cast<i32>(clamp_i32(in_l[i] + fb_l, -LIM, LIM)));
            line_r_.push(static_cast<i32>(clamp_i32(in_r[i] + fb_r, -LIM, LIM)));
            out_l[i] += asr(static_cast<i64>(tap_l) * wet_, GAIN_SHIFT);
            out_r[i] += asr(static_cast<i64>(tap_r) * wet_, GAIN_SHIFT);
        }
    }

  private:
    // samples = SAMPLE_RATE * 60 * beats / bpm; the Q16s cancel. Clamped to
    // the line, so a slow tempo on a long division just stops getting longer.
    void recompute_delay() noexcept {
        i64 samples;
        if (p_.sync_beats_q16 > 0) {
            samples = (static_cast<i64>(SAMPLE_RATE) * 60 * p_.sync_beats_q16) / bpm_q16_;
        } else {
            samples = asr(static_cast<i64>(p_.time_ms_q16) * SAMPLE_RATE / 1000, GAIN_SHIFT);
        }
        delay_ = static_cast<u32>(clamp_i32(samples, 48, (1 << 15) - 2));
    }

    static constexpr gain_t DAMP_COEF = (GAIN_ONE * 55) / 100;  // ~4 kHz-ish tilt
    DelayLine<15> line_l_, line_r_;  // 32768 samples = 682 ms max
    i32 damp_l_ = 0, damp_r_ = 0;
    u32 delay_ = 48 * 320;
    gain_t feedback_ = 0, wet_ = 0;
    bool ping_pong_ = false;
    DelayParams p_;
    gain_t bpm_q16_ = 120 << GAIN_SHIFT;
};

}  // namespace msynth
