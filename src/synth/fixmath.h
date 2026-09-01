#pragma once
//
// midisynth numeric foundation.
//
// Reuses the engine's fixed-point primitives (dnp/math/fixed.hpp) -- the type
// aliases and the well-defined shift helpers -- but the audio path carries its
// own formats per the plan (v2 §3):
//
//   audio samples          Q8.24  in i32   (headroom above unity for summing)
//   mix accumulation       i64            (never clamped mid-sum)
//   phase accumulator      u32, bespoke layout, free wrap
//   envelope level / gains Q16.16 in i32
//   filter coeffs & state  Q2.30  in i32
//
// ENGINE-WIDE ROUNDING RULE (Appendix D): plain arithmetic shift right, no
// rounding offset, everywhere. dnp::asr rounds toward -inf and is exactly
// that rule. Never use `>>` directly on a signed value that can be negative.
//
#include "dnp/math/fixed.hpp"

namespace msynth {

using dnp::i8;
using dnp::i16;
using dnp::i32;
using dnp::i64;
using dnp::u8;
using dnp::u16;
using dnp::u32;
using dnp::u64;
using dnp::asr;
using dnp::asl;

using sample_t = i32;  // Q8.24
using gain_t   = i32;  // Q16.16
using coef_t   = i32;  // Q2.30

inline constexpr int SAMPLE_SHIFT = 24;
inline constexpr i32 SAMPLE_ONE   = 1 << SAMPLE_SHIFT;
inline constexpr int GAIN_SHIFT   = 16;
inline constexpr i32 GAIN_ONE     = 1 << GAIN_SHIFT;
inline constexpr int COEF_SHIFT   = 30;
inline constexpr i32 COEF_ONE     = 1 << COEF_SHIFT;

// (a * b) >> shift with the product held in 64 bits.
constexpr i32 mulshift(i32 a, i32 b, int shift) noexcept {
    return static_cast<i32>(asr(static_cast<i64>(a) * b, shift));
}

// Q16.16 * Q16.16 -> Q16.16
constexpr gain_t gmul(gain_t a, gain_t b) noexcept { return mulshift(a, b, GAIN_SHIFT); }

// Audio sample scaled by a Q16.16 gain.
constexpr sample_t sgain(sample_t s, gain_t g) noexcept { return mulshift(s, g, GAIN_SHIFT); }

constexpr i32 clamp_i32(i64 v, i64 lo, i64 hi) noexcept {
    return static_cast<i32>(v < lo ? lo : (v > hi ? hi : v));
}

constexpr gain_t clamp_gain01(i64 g) noexcept { return clamp_i32(g, 0, GAIN_ONE); }

// Linear interpolation between two i32 values by a 16-bit fraction, in i64.
constexpr i32 lerp16(i32 a, i32 b, u32 frac16) noexcept {
    return a + static_cast<i32>(asr(static_cast<i64>(b - a) * static_cast<i32>(frac16), 16));
}

// Deterministic PRNG: xorshift32. Never platform rand().
struct XorShift32 {
    u32 s;
    explicit constexpr XorShift32(u32 seed) noexcept : s(seed ? seed : 0x9E3779B9u) {}
    constexpr u32 next() noexcept {
        u32 x = s;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        s = x;
        return x;
    }
    // Signed audio-range noise, Q8.24 in roughly [-1, 1).
    constexpr sample_t next_sample() noexcept {
        return static_cast<sample_t>(asr(static_cast<i32>(next()), 7));
    }
};

}  // namespace msynth
