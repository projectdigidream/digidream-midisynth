#pragma once
//
// LFO, advanced once per control block. Output Q16.16 in [-1, 1].
//
#include "tables.h"

namespace msynth {

enum class LfoShape : u8 { Sine, Triangle, Saw, Square, SampleHold };

struct LfoParams {
    LfoShape shape = LfoShape::Sine;
    u32 inc_per_block = 0;  // phase increment per control block
    u32 delay_blocks = 0;   // silent onset delay
};

struct LfoState {
    u32 phase = 0;
    u32 delay_left = 0;
    i32 held = 0;  // SampleHold value
    XorShift32 rng{1};

    void note_on(const LfoParams& p, u32 seed) noexcept {
        phase = 0;
        delay_left = p.delay_blocks;
        rng = XorShift32(seed);
        held = sh_value();
    }

    i32 sh_value() noexcept {
        // Q16.16 in [-1, 1).
        return static_cast<i32>(asr(static_cast<i32>(rng.next()), 15));
    }

    // One control block; returns Q16.16 in [-GAIN_ONE, GAIN_ONE].
    i32 advance_block(const LfoParams& p) noexcept {
        if (delay_left > 0) {
            --delay_left;
            return 0;
        }
        const u32 prev = phase;
        phase += p.inc_per_block;
        switch (p.shape) {
            case LfoShape::Sine:
                return static_cast<i32>(asr(sine_q30(phase), COEF_SHIFT - GAIN_SHIFT));
            case LfoShape::Triangle: {
                // 0 -> +1 -> 0 -> -1 -> 0 over one cycle (sine-aligned).
                const i32 r = static_cast<i32>(phase >> 14);  // 0..4*GAIN_ONE
                if (phase < (1u << 30)) return r;
                if (phase < (3u << 30)) return 2 * GAIN_ONE - r;
                return r - 4 * GAIN_ONE;
            }
            case LfoShape::Saw:
                return static_cast<i32>(phase >> 15) - GAIN_ONE;
            case LfoShape::Square:
                return phase < (1u << 31) ? GAIN_ONE : -GAIN_ONE;
            case LfoShape::SampleHold:
                if (phase < prev) held = sh_value();  // wrapped: new sample
                return held;
        }
        return 0;
    }
};

}  // namespace msynth
