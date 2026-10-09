#pragma once
//
// Runtime lookup tables. Everything transcendental-derived is baked by
// scripts/gen_tables.py (tables_baked.inc); the rest is plain integer
// arithmetic computed once by tables_init(). No libm anywhere.
//
#include "config.h"

namespace msynth {

// Call once before any synthesis. Idempotent, control-thread only.
void tables_init() noexcept;

// ---- Pitch -----------------------------------------------------------------
// cents_q16: MIDI pitch in cents from note 0 (note*100 in Q16.16).
// Returns the 32-bit phase increment for SAMPLE_RATE.
u32 phase_inc_for_cents(i32 cents_q16) noexcept;

// ---- Velocity / volume curves ----------------------------------------------
extern gain_t VEL_GAIN_Q16[128];    // (v/127)^2 (default; also used for CC7/11)
extern gain_t VEL_LINEAR_Q16[128];  // v/127
extern gain_t VEL_SOFT_Q16[128];    // (v/127)^1.5 -- between the two

// ---- Pan (constant power) --------------------------------------------------
// pan_q16 in [-GAIN_ONE, +GAIN_ONE]; outputs Q16.16 gains.
void pan_gains(i32 pan_q16, gain_t& out_l, gain_t& out_r) noexcept;

// ---- Envelope curves -------------------------------------------------------
enum class CurveId : u8 { Linear = 0, Exp, ExpAttack, SCurve, COUNT };
// 257-entry Q16.16 tables, curve[0] == 0, curve[256] == GAIN_ONE.
const gain_t* curve_table(CurveId id) noexcept;

// Evaluate a curve at phase_q24 in [0, 1<<24].
gain_t curve_eval(const gain_t* curve, u32 phase_q24) noexcept;

// ---- Filter coefficient tables ---------------------------------------------
// Cutoff index space: Q16.16 over [0, 256]; exponentially spaced 20Hz..12kHz.
inline constexpr int CUTOFF_STEPS = 256;
coef_t cutoff_g(i32 index_q16) noexcept;         // g = tan(pi*fc/fs), Q2.30
i32 cutoff_index_from_hz(i32 hz_q16) noexcept;   // load-time mapping, Q16.16
// Resonance res_q16 in [0, GAIN_ONE) -> k = 2 - 2*res, Q2.30.
coef_t resonance_k(gain_t res_q16) noexcept;

// ---- Misc ------------------------------------------------------------------
// sin of a full-turn phase (u32) in Q2.30 -- LFO sine, table-interpolated.
i32 sine_q30(u32 phase) noexcept;

// Deterministic log2 of a Q16.16 value (v > 0), result Q16.16.
i32 log2_q16(u32 v_q16) noexcept;

// 2^x for x in Q16.16, x clamped to [-16, 15]; result Q16.16 (table + lerp).
gain_t exp2_q16(i32 x_q16) noexcept;
// dB -> linear gain, Q16.16 (10^(dB/20) = 2^(dB * log2(10)/20)).
gain_t db_to_lin_q16(i32 db_q16) noexcept;
// Linear gain (Q16.16, > 0) -> dB, Q16.16.
i32 lin_to_db_q16(u32 lin_q16) noexcept;
// One-pole smoothing coefficient per CONTROL_BLOCK for a time constant in ms
// (Q16.16): a = 1 - 2^(-log2(e) / tau_blocks). 0 ms -> GAIN_ONE (instant).
gain_t onepole_coef_from_ms(i32 ms_q16) noexcept;

// Raw baked sine table (4096 entries, Q2.30, full cycle) -- wavetable
// generation reads it directly: harmonic*index arguments land exactly on
// table entries, so additive synthesis needs no interpolation at all.
const i32* sine_table_4096() noexcept;

// Cutoff-table steps per octave of frequency, Q16.16 (key-tracking scale).
i32 cutoff_steps_per_octave_q16() noexcept;
// Inverse of cutoff_index_from_hz: table index (Q16, 0..256) -> Hz (Q16).
i32 cutoff_hz_from_index(i32 index_q16) noexcept;

}  // namespace msynth
