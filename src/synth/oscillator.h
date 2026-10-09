#pragma once
//
// Wavetable oscillator (plan Appendix A). Audio-rate hot path.
//
// Phase: full 32-bit accumulator; integer overflow IS the cycle wrap. The
// index shift depends on the selected mip's table size and is fixed at
// note-on -- no per-sample branching.
//
// Interpolation: Hermite (4-point Catmull-Rom) across phase, linear across
// frames. Frame morph position updates at control rate.
//
#include "wavetables.h"

namespace msynth {

// 4-point Catmull-Rom at a 16-bit fraction, samples Q2.30, result Q2.30.
inline i32 hermite4(const i32* tab, u32 idx, u32 mask, u32 frac16) noexcept {
    const i64 y0 = tab[(idx - 1) & mask];
    const i64 y1 = tab[idx & mask];
    const i64 y2 = tab[(idx + 1) & mask];
    const i64 y3 = tab[(idx + 2) & mask];
    const i64 c1 = asr(y2 - y0, 1);
    const i64 c2 = asr(2 * y0 - 5 * y1 + 4 * y2 - y3, 1);
    const i64 c3 = asr(3 * (y1 - y2) + y3 - y0, 1);
    i64 acc = c3;
    acc = c2 + asr(acc * frac16, 16);
    acc = c1 + asr(acc * frac16, 16);
    return static_cast<i32>(y1 + asr(acc * frac16, 16));
}

struct Oscillator {
    u32 phase = 0;
    u32 phase_inc = 0;
    i32 frame_pos_q16 = 0;   // morph position in frames, Q16.16
    i32 frame_delta_q16 = 0;  // per-sample ramp toward the block target

    // Fixed at note-on (mip selection):
    const i32* mip_base = nullptr;  // frames of the chosen mip, contiguous
    u32 mask = 0;
    u8 idx_shift = 21;   // 32 - log2(N)
    u8 frac_shift = 5;   // 32 - log2(N) - 16
    u8 frame_count = 1;
    u32 table_n = 0;     // samples per frame at this mip

    // Updated at control rate from frame_pos_q16:
    const i32* frame_a = nullptr;
    const i32* frame_b = nullptr;
    u32 frame_frac16 = 0;

    void set_table(const WaveTable& wt, int mip) noexcept {
        const u8 lg = wt.mipLog2Size[mip];
        mip_base = wt.mipData[mip];
        table_n = 1u << lg;
        mask = table_n - 1;
        idx_shift = static_cast<u8>(32 - lg);
        frac_shift = static_cast<u8>(32 - lg - 16);
        frame_count = wt.frameCount;
        update_frames();
    }

    void set_frame_pos(i32 pos_q16) noexcept {
        const i32 max_pos = (frame_count - 1) << 16;
        frame_pos_q16 = clamp_i32(pos_q16, 0, max_pos);
        frame_delta_q16 = 0;
        update_frames();
    }

    // Ramp toward a new morph position across the control block (64 samples)
    // instead of jumping once per block: kills wtPos zipper in slow sweeps
    // and quiet envelope tails.
    void set_frame_target(i32 pos_q16) noexcept {
        if (frame_count <= 1) return;
        const i32 max_pos = (frame_count - 1) << 16;
        const i32 tgt = clamp_i32(pos_q16, 0, max_pos);
        frame_delta_q16 = static_cast<i32>(asr(static_cast<i64>(tgt) - frame_pos_q16, 6));
        if (frame_delta_q16 == 0 && tgt != frame_pos_q16) {
            // Sub-count slope: land exactly rather than sticking.
            frame_pos_q16 = tgt;
            update_frames();
        }
    }

    void update_frames() noexcept {
        if (!mip_base) return;
        u32 fi = static_cast<u32>(frame_pos_q16) >> 16;
        if (fi >= frame_count) fi = frame_count - 1;
        const u32 fi2 = fi + 1 < frame_count ? fi + 1 : fi;
        frame_a = mip_base + static_cast<size_t>(fi) * table_n;
        frame_b = mip_base + static_cast<size_t>(fi2) * table_n;
        frame_frac16 = static_cast<u32>(frame_pos_q16) & 0xFFFFu;
    }

    // One sample, Q2.30. `phase_mod` shifts the read position without
    // touching the running phase -- that is phase modulation, i.e. FM.
    inline i32 next_q30(u32 phase_mod = 0) noexcept {
        if (frame_delta_q16 != 0) {
            const u32 old_fi = static_cast<u32>(frame_pos_q16) >> 16;
            frame_pos_q16 += frame_delta_q16;
            if ((static_cast<u32>(frame_pos_q16) >> 16) != old_fi) update_frames();
            else frame_frac16 = static_cast<u32>(frame_pos_q16) & 0xFFFFu;
        }
        const u32 read = phase + phase_mod;
        const u32 idx = read >> idx_shift;
        const u32 frac = (read >> frac_shift) & 0xFFFFu;
        phase += phase_inc;
        const i32 a = hermite4(frame_a, idx, mask, frac);
        if (frame_a == frame_b) return a;
        const i32 b = hermite4(frame_b, idx, mask, frac);
        return lerp16(a, b, frame_frac16);
    }
};

}  // namespace msynth
