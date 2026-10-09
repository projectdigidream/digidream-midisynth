#pragma once
//
// LFO, advanced once per control block. Output Q16.16 in [-1, 1].
//
#include "tables.h"

namespace msynth {

enum class LfoShape : u8 { Sine, Triangle, Saw, Square, SampleHold };

struct LfoParams {
    LfoShape shape = LfoShape::Sine;
    u32 inc_per_block = 0;  // phase increment per control block (free-running)
    u32 delay_blocks = 0;   // silent onset delay
    // Tempo sync: cycle length in beats (Q16.16), 0 = free-running rateHz.
    i32 sync_beats_q16 = 0;
    // false = the phase is NOT reset at note-on; the LFO picks up where one
    // running since the song started would be, so held chords keep moving
    // instead of restarting their sweep on every new note.
    bool retrigger = true;
};

// Phase increment for a tempo-synced LFO: cycles/s = bpm / 60 / beats.
inline u32 lfo_sync_inc(i32 sync_beats_q16, gain_t bpm_q16) noexcept {
    if (sync_beats_q16 <= 0 || bpm_q16 <= 0) return 0;
    // inc = 2^32 * (bpm/2^16) / (60 * CONTROL_RATE * beats/2^16): the two
    // Q16 factors cancel -> (bpm_q16 << 32) / (45000 * beats_q16), via i128.
    const dnp::i128 num = static_cast<dnp::i128>(static_cast<u32>(bpm_q16)) << 32;
    const i64 den = static_cast<i64>(60) * CONTROL_RATE * sync_beats_q16;
    return static_cast<u32>(static_cast<u64>(num / den));
}

struct LfoState {
    u32 phase = 0;
    u32 delay_left = 0;
    i32 held = 0;  // SampleHold value
    XorShift32 rng{1};

    // `blocks_elapsed` is the engine's block counter: where a free-running
    // LFO would stand now. Deterministic -- it depends on song position, not
    // on which pool slot the voice happened to get.
    void note_on(const LfoParams& p, u32 seed, u64 blocks_elapsed = 0,
                 gain_t bpm_q16 = 120 << GAIN_SHIFT) noexcept {
        const u32 inc = p.sync_beats_q16 > 0 ? lfo_sync_inc(p.sync_beats_q16, bpm_q16)
                                             : p.inc_per_block;
        phase = p.retrigger ? 0 : static_cast<u32>(blocks_elapsed * inc);
        delay_left = p.delay_blocks;
        rng = XorShift32(seed);
        held = sh_value();
    }

    i32 sh_value() noexcept {
        // Q16.16 in [-1, 1).
        return static_cast<i32>(asr(static_cast<i32>(rng.next()), 15));
    }

    // One control block; returns Q16.16 in [-GAIN_ONE, GAIN_ONE].
    // bpm_q16 drives tempo-synced rates (ignored for free-running LFOs).
    i32 advance_block(const LfoParams& p, gain_t bpm_q16 = 120 << GAIN_SHIFT) noexcept {
        if (delay_left > 0) {
            --delay_left;
            return 0;
        }
        const u32 prev = phase;
        phase += p.sync_beats_q16 > 0 ? lfo_sync_inc(p.sync_beats_q16, bpm_q16) : p.inc_per_block;
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
