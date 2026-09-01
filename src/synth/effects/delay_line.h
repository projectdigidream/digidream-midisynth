#pragma once
//
// Reusable fixed-point delay line: preallocated power-of-two ring buffer.
// Shared by reverb combs/allpasses and chorus (plan §8). Q8.24 samples.
//
#include "../fixmath.h"

namespace msynth {

template <int LOG2_CAP>
struct DelayLine {
    static constexpr u32 CAP = 1u << LOG2_CAP;
    static constexpr u32 MASK = CAP - 1;
    i32 buf[CAP] = {};
    u32 write_pos = 0;

    void clear() noexcept {
        for (u32 i = 0; i < CAP; ++i) buf[i] = 0;
        write_pos = 0;
    }
    void push(i32 v) noexcept {
        buf[write_pos & MASK] = v;
        ++write_pos;
    }
    // Sample delayed by `d` (1..CAP), relative to the value just pushed.
    i32 read(u32 d) const noexcept { return buf[(write_pos - d) & MASK]; }
    // Linear-interpolated read at delay d_q16 (Q16.16 samples).
    i32 read_lerp(u32 d_q16) const noexcept {
        const u32 d = d_q16 >> 16;
        const u32 frac = d_q16 & 0xFFFFu;
        return lerp16(read(d), read(d + 1), frac);
    }
};

}  // namespace msynth
