#pragma once
//
// One voice: oscillator or noise -> SVF -> panned gain into the stereo buses
// (plan §8). Audio-rate work is the 64-sample inner loop; everything else
// happens once per control block.
//
#include "channel_state.h"
#include "envelope.h"
#include "filter.h"
#include "lfo.h"
#include "mod_matrix.h"
#include "oscillator.h"
#include "patch_def.h"

namespace msynth {

// Per-control-block accumulation targets (i64, Q8.24; never clamped mid-sum).
struct VoiceBuses {
    i64* dry_l;
    i64* dry_r;
    i64* rev_l;
    i64* rev_r;
    i64* cho_l;
    i64* cho_r;
};

struct Voice {
    bool active = false;
    u8 slot = 0;  // pool index; part of the deterministic noise seed
    u8 chan_slot = 0;
    u8 note = 0, velocity = 0;
    u32 group_id = 0;
    u8 layer_index = 0;
    u32 age_blocks = 0;
    u8 steal_shield = 0;  // blocks of steal immunity after note-on
    bool released = false;
    bool pedal_held = false;  // note-off deferred by sustain pedal

    const LayerDef* layer = nullptr;
    const Patch* patch = nullptr;

    Oscillator osc;
    XorShift32 noise{1};
    EnvelopeState amp_env, filt_env, pitch_env, wt_env;
    LfoState lfo[MAX_LFOS];
    FilterState filter;
    ModValues mod;

    i32 base_cents_q16 = 0;
    gain_t vel_gain = 0;
    i32 key_track_q16 = 0;  // (note - 60) / 64, Q16.16

    // Audio-rate gain ramps (Q16.16):
    gain_t cur_gl = 0, cur_gr = 0;
    i32 d_gl = 0, d_gr = 0;
    gain_t send_rev = 0, send_cho = 0;

    void start(u8 pool_slot, u8 channel_slot, u8 midi_note, u8 vel, u32 group,
               u8 layer_idx, const Patch* p, const LayerDef* l,
               const ChannelState& ch) noexcept;
    void note_off(bool pedal_down) noexcept;
    void release_pedal() noexcept;  // pedal up: fire the deferred note-off
    void fast_release() noexcept;   // steal / choke / All Sound Off

    // Render CONTROL_BLOCK samples into the buses. Returns false once idle.
    bool render_block(const ChannelState& ch, const VoiceBuses& buses) noexcept;
};

}  // namespace msynth
