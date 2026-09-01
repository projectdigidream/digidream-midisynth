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

enum class OscType : u8 { Wavetable, Noise };
enum class VelCurve : u8 { Squared, Linear, Soft };  // velocity -> gain law

struct LayerDef {
    OscType osc = OscType::Wavetable;
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
