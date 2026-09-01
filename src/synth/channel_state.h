#pragma once
//
// Per-channel MIDI state (plan §8/§9). One entry per channel slot; two
// 16-channel banks so a crossfade's two songs don't fight over state.
//
#include "tables.h"

namespace msynth {

struct ChannelState {
    u8 program = 0;
    u8 bank = 0;
    bool is_drum = false;

    i32 bend_raw = 0;              // -8192..8191
    i32 bend_range_cents = 200;    // RPN 0 (default +/-2 semitones)
    i32 fine_tune_cents_q16 = 0;   // RPN 1
    i32 coarse_tune_cents = 0;     // RPN 2

    u8 volume = 100;      // CC7
    u8 expression = 127;  // CC11
    i32 pan_q16 = 0;      // CC10 -> [-1, 1]
    bool sustain = false; // CC64
    gain_t modwheel_q16 = 0;    // CC1 -> [0, 1]
    gain_t aftertouch_q16 = 0;  // channel pressure -> [0, 1]
    u8 send_reverb = 40;  // CC91 (GM default)
    u8 send_chorus = 0;   // CC93
    i32 cutoff_offset_q16 = 0;   // CC74, cutoff-index space
    gain_t res_offset_q16 = 0;   // CC71

    // RPN/NRPN state machine (CC101/100/99/98 + CC6/38).
    u8 rpn_msb = 127, rpn_lsb = 127;
    bool nrpn_active = false;

    // music_director: fade gain, ramped at control rate on the audio side.
    gain_t fade_gain_q16 = GAIN_ONE;         // current
    gain_t fade_target_q16 = GAIN_ONE;       // where it's heading
    gain_t fade_step_q16 = 0;                // per control block

    i32 bend_cents_q16() const noexcept {
        // bend_raw/8192 * range, plus static tuning offsets.
        const i64 scaled = asl(static_cast<i64>(bend_raw) * bend_range_cents, GAIN_SHIFT);
        return static_cast<i32>(scaled / 8192) + fine_tune_cents_q16 +
               static_cast<i32>(asl(static_cast<i64>(coarse_tune_cents), GAIN_SHIFT));
    }

    // CC7 * CC11 * fade, Q16.16 (squared-law tables).
    gain_t channel_gain_q16() const noexcept {
        const gain_t v = gmul(VEL_GAIN_Q16[volume], VEL_GAIN_Q16[expression]);
        return gmul(v, fade_gain_q16);
    }

    void advance_fade_block() noexcept {
        if (fade_gain_q16 == fade_target_q16) return;
        if (fade_gain_q16 < fade_target_q16) {
            fade_gain_q16 += fade_step_q16;
            if (fade_gain_q16 > fade_target_q16) fade_gain_q16 = fade_target_q16;
        } else {
            fade_gain_q16 -= fade_step_q16;
            if (fade_gain_q16 < fade_target_q16) fade_gain_q16 = fade_target_q16;
        }
    }

    void reset_controllers() noexcept {
        // CC121 per GM: controllers reset, but not program/volume/pan/sends.
        bend_raw = 0;
        expression = 127;
        sustain = false;
        modwheel_q16 = 0;
        aftertouch_q16 = 0;
        cutoff_offset_q16 = 0;
        res_offset_q16 = 0;
        rpn_msb = rpn_lsb = 127;
        nrpn_active = false;
    }

    void full_reset(bool drum) noexcept {
        *this = ChannelState{};
        is_drum = drum;
    }
};

}  // namespace msynth
