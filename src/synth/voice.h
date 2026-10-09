#pragma once
//
// One voice: oscillator or noise -> SVF -> panned gain into the stereo buses
// (plan §8). Audio-rate work is the 64-sample inner loop; everything else
// happens once per control block.
//
#include "channel_state.h"
#include "effects/delay_line.h"
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
    i64* del_l;
    i64* del_r;
    gain_t tempo_bpm_q16;      // for tempo-synced LFOs
    const i32* tuning_cents;   // 12-entry microtuning table (Q16 cents)
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
    FilterState filter, filter2;  // filter2: 24 dB cascade OR the independent second filter
    FilterState filter3;          // formant mode: third parallel bandpass
    // Comb mode: a per-voice tunable delay line (the accepted-memory cost:
    // 16 KB x 128 voices = 2 MB static). Delay ramps per sample to the
    // block target so pitch sweeps stay clickless.
    DelayLine<12> comb_line;      // 4096 samples: down to ~12 Hz
    u32 comb_delay_q16 = 0;       // current fractional delay
    i32 comb_delta_q16 = 0;       // per-sample ramp toward the block target
    ModValues mod;

    // Unison / glide / ring / poly-AT state (set by the engine at note-on).
    i32 unison_extra_cents_q16 = 0;
    gain_t unison_pan_q16 = 0;
    i32 glide_offset_cents_q16 = 0;  // decays linearly to 0
    i32 glide_step_cents_q16 = 0;
    u32 ring_phase = 0;
    u32 metal_phase[6] = {};  // OscType::Metal square-cluster accumulators
    gain_t poly_at_q16 = 0;
    gain_t rel_scale_q16 = GAIN_ONE;  // release-velocity scaling for this lift
    i32 delay_blocks = 0;         // startDelayMs countdown; silent while > 0
    i32 pending_off_blocks = 0;   // deferred note-off fires after the attack
    bool pending_note_off = false;  // note-off arrived during the delay
    bool pending_pedal = false;
    i32 base_cents_q16 = 0;
    gain_t vel_gain = 0;
    i32 key_track_q16 = 0;  // (note - 60) / 64, Q16.16

    // Audio-rate gain ramps (Q8.24 -- fine enough that quiet exponential
    // tails glide instead of stair-stepping):
    i32 cur_gl = 0, cur_gr = 0;
    i32 d_gl = 0, d_gr = 0;
    gain_t send_rev = 0, send_cho = 0, send_del = 0;
    i32 noise_b0 = 0, noise_b1 = 0, noise_b2 = 0;  // coloured-noise filter state

    // One noise sample, Q8.24, tilted by colour. White is the raw generator
    // (bit-identical to before this existed); pink is Kellet's three-pole
    // economy filter at roughly -3 dB/oct; brown is a one-pole integrator at
    // -6 dB/oct. The output scalings only keep the three roughly level.
    sample_t coloured_noise(NoiseColor c) noexcept {
        const sample_t w = noise.next_sample();
        if (c == NoiseColor::White) return w;
        if (c == NoiseColor::Brown) {
            noise_b2 += asr(w - noise_b2, 6);   // ~120 Hz one-pole
            return noise_b2 * 4;
        }
        noise_b0 = static_cast<i32>(asr(static_cast<i64>(noise_b0) * 65381 +
                                        static_cast<i64>(w) * 6491, GAIN_SHIFT));
        noise_b1 = static_cast<i32>(asr(static_cast<i64>(noise_b1) * 63111 +
                                        static_cast<i64>(w) * 19432, GAIN_SHIFT));
        noise_b2 = static_cast<i32>(asr(static_cast<i64>(noise_b2) * 37356 +
                                        static_cast<i64>(w) * 68989, GAIN_SHIFT));
        const i64 sum = static_cast<i64>(noise_b0) + noise_b1 + noise_b2 +
                        asr(static_cast<i64>(w) * 12112, GAIN_SHIFT);
        return static_cast<sample_t>(asr(sum, 2));
    }

    // blocks_elapsed / bpm_q16 place a free-running ("retrigger": false) LFO
    // where the song clock says it should be; they are unused otherwise.
    void start(u8 pool_slot, u8 channel_slot, u8 midi_note, u8 vel, u32 group,
               u8 layer_idx, const Patch* p, const LayerDef* l,
               const ChannelState& ch, u64 blocks_elapsed = 0,
               gain_t bpm_q16 = 120 << GAIN_SHIFT) noexcept;
    void note_off(bool pedal_down, u8 release_vel = 64) noexcept;
    void release_pedal() noexcept;  // pedal up: fire the deferred note-off
    void fast_release() noexcept;   // steal / choke / All Sound Off

    // Render CONTROL_BLOCK samples into the buses. Returns false once idle.
    bool render_block(const ChannelState& ch, const VoiceBuses& buses) noexcept;
};

}  // namespace msynth
