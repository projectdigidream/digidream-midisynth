#pragma once
//
// Master soft-clip limiter + Q8.24 -> int16 conversion with optional TPDF
// dither (plan §3). The accumulator is never allowed to wrap: soft-clip up to
// 1.5x full scale (smooth cubic, unity at the knee edge), hard clamp beyond.
//
#include "fixmath.h"

namespace msynth {

// Cubic soft clip: y = x - x^3/6.75 for |x| <= 1.5 (unity, zero-slope at the
// edge), +/-1.0 beyond. Input/output Q8.24.
inline i32 soft_clip_q24(i64 x) noexcept {
    constexpr i64 LIM = (3LL << SAMPLE_SHIFT) / 2;  // 1.5
    if (x >= LIM) return SAMPLE_ONE;
    if (x <= -LIM) return -SAMPLE_ONE;
    const i64 x2 = asr(x * x, SAMPLE_SHIFT);
    const i64 x3 = asr(x2 * x, SAMPLE_SHIFT);
    // 1/6.75 in Q16.16 = 9709.
    return static_cast<i32>(x - asr(x3 * 9709, GAIN_SHIFT));
}

struct OutputStage {
    gain_t master_gain = GAIN_ONE / 8;  // headroom for expected polyphony
    bool dither = false;
    // Ducking (plan §10): a master gain ramp driven by the game.
    gain_t duck_cur = GAIN_ONE;
    gain_t duck_target = GAIN_ONE;
    gain_t duck_step = 0;

    void set_duck(gain_t target, i32 blocks) noexcept {
        duck_target = clamp_gain01(target);
        i32 dist = duck_target - duck_cur;
        if (dist < 0) dist = -dist;
        duck_step = blocks > 0 ? dist / blocks : dist;
        if (duck_step == 0) duck_step = 1;
    }

    void advance_duck_block() noexcept {
        if (duck_cur == duck_target) return;
        if (duck_cur < duck_target) {
            duck_cur += duck_step;
            if (duck_cur > duck_target) duck_cur = duck_target;
        } else {
            duck_cur -= duck_step;
            if (duck_cur < duck_target) duck_cur = duck_target;
        }
    }
    // Dedicated fixed-seed stream, advanced once per output sample per
    // channel in a defined order -- dither on/off are two different but
    // equally deterministic outputs (plan §3).
    XorShift32 dither_rng{0xD17E4u};

    inline i16 convert(i64 acc_q24) noexcept {
        const gain_t mg = gmul(master_gain, duck_cur);
        const i64 scaled = asr(acc_q24 * mg, GAIN_SHIFT);
        i64 v = soft_clip_q24(scaled);
        if (dither) {
            // TPDF: sum of two uniforms, +/-1 LSB of the 16-bit output
            // (1 LSB = 2^9 in Q8.24).
            const i32 r1 = static_cast<i32>(dither_rng.next() & 0x1FFu);
            const i32 r2 = static_cast<i32>(dither_rng.next() & 0x1FFu);
            v += r1 + r2 - 0x1FF;
        }
        return static_cast<i16>(clamp_i32(asr(v, SAMPLE_SHIFT - 15), -32768, 32767));
    }
};

}  // namespace msynth
