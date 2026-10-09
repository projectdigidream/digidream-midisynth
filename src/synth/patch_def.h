#pragma once
//
// Resolved patch definitions -- the JSON schema (plan §13) after the loader
// has validated, clamped and converted everything to fixed point. The audio
// thread only ever sees these; JSON never crosses the thread boundary.
//
#include <string>

#include "envelope.h"
#include "filter.h"
#include "lfo.h"
#include "mod_matrix.h"
#include "wavetables.h"

namespace msynth {

// Metal = six detuned square waves at the TR-808's inharmonic ratios,
// tracked to the voice pitch: the classic cymbal/hat source (bandpass or
// highpass it). Like noise, it is deliberately not band-limited.
enum class OscType : u8 { Wavetable, Noise, Metal };
// Spectral tilt for OscType::Noise. Pink (-3 dB/oct) is the body of snares
// and cymbal wash; brown (-6 dB/oct) is wind, rumble and soft atmospheres.
enum class NoiseColor : u8 { White, Pink, Brown };
enum class VelCurve : u8 { Squared, Linear, Soft };  // velocity -> gain law

struct LayerDef {
    OscType osc = OscType::Wavetable;
    NoiseColor noise_color = NoiseColor::White;
    WaveTablePtr wavetable;  // required for OscType::Wavetable

    i32 detune_cents_q16 = 0;
    gain_t mix_gain = GAIN_ONE;
    u8 vel_low = 0, vel_high = 127;
    VelCurve vel_curve = VelCurve::Squared;
    i32 pan_q16 = 0;  // [-GAIN_ONE, GAIN_ONE]

    // Fixed synthesis pitch in cents from MIDI note 0, or -1 to follow the
    // played note. Drum layers use this (the MIDI note selects the drum, not
    // the pitch).
    i32 fixed_pitch_cents_q16 = -1;

    EnvParams amp_env, filter_env, pitch_env, wt_env;
    // Implicit routing depths (resolved from JSON envelope depth fields):
    gain_t filter_env_depth = 0;   // fraction of full cutoff range, signed
    i32 pitch_env_cents_q16 = 0;   // depthSemis * 100, Q16.16 cents
    gain_t wt_env_depth = 0;       // fraction of full frame range

    FilterMode filter_mode = FilterMode::Bypass;
    i32 cutoff_index_q16 = 0;      // cutoff-table index space, Q16.16
    gain_t resonance_q16 = 0;
    gain_t key_track_filter = 0;   // 0..1
    bool filter_24db = false;      // two cascaded SVF stages (24 dB/oct)
    gain_t filter_drive_q16 = GAIN_ONE;  // pre-filter saturation gain, 1..8
    i32 start_delay_blocks = 0;    // layer speaks this many control blocks late
    // Per-layer send overrides (drums share one channel; a snare can be
    // wetter than the kick). -1 = inherit the channel's CC91/93/94.
    i32 send_reverb_q16 = -1;
    i32 send_chorus_q16 = -1;
    i32 send_delay_q16 = -1;
    gain_t post_drive_q16 = GAIN_ONE;    // post-filter saturation (NOT band-limited)
    // Unison stack: extra voices with symmetric detune/pan spread.
    u8 unison_voices = 1;                // 1..7
    i32 unison_detune_cents_q16 = 0;     // +/- widest voice detune
    gain_t unison_spread_q16 = 0;        // 0..1 stereo spread
    // 0..1: how far apart the stack's oscillators START. At 0 they all begin
    // at phase 0 and a detuned stack only beats once the phases drift; a
    // Reese wants them spread from the first sample.
    gain_t unison_phase_spread_q16 = 0;
    // Ring modulation against an internal sine at ratio x the voice pitch.
    gain_t ring_ratio_q16 = 0;           // 0 = off; 0.25..16
    // Ring: 0 dry .. 1 full ring. FM ("mode": "fm"): the modulation index in
    // radians, 0..12 -- an envelope on it is what makes a Rhodes bark then
    // mellow, which a baked fm2op table can never do.
    gain_t ring_amount_q16 = 0;
    bool ring_is_fm = false;
    // Independent second filter (serial by default, or parallel).
    FilterMode filter2_mode = FilterMode::Bypass;
    i32 filter2_cutoff_index_q16 = 0;
    gain_t filter2_resonance_q16 = 0;
    bool filter_parallel = false;        // filter + filter2 side by side
    bool filter2_static = false;         // filter2 ignores every modulation (fixed corner)
    // Release velocity -> release-time scaling (0 = off).
    gain_t release_vel_amount_q16 = 0;
    // Round robin: consecutive note-ons alternate layers sharing a group.
    u8 rr_group = 0;                     // 0 = none
    u8 rr_index = 0, rr_size = 1;        // filled by the loader
    // Formant mode: cutoff-table indexes of F1..F3 for the vowel pair
    // (morphed by the filter envelope). Baked by the loader.
    i32 formant_idx_q16[2][3] = {};

    LfoParams lfos[MAX_LFOS];
    u8 lfo_count = 0;
    ModRouting routings[MAX_ROUTINGS];
    u8 routing_count = 0;
};

struct Patch {
    std::string name;
    u8 layer_count = 0;
    LayerDef layers[MAX_LAYERS];

    // Drum behavior (melodic patches leave defaults):
    bool one_shot = false;
    i8 exclusive_group = -1;  // choke group, -1 = none
};

}  // namespace msynth
