#pragma once
//
// Mipmapped wavetables (plan §4, Appendix A).
//
// Layout: per mip level, frames are contiguous ([mip][frame][sample]) so the
// two adjacent frames a morphing oscillator reads stay cache-friendly. Table
// size halves per mip level: 2048, 1024, ..., 8 -- total ~2x the base size.
//
// Samples are Q2.30, normalized at generation so a frame peaks just under
// 1.0. Q2.30 leaves headroom for Hermite overshoot between samples.
//
// Mip m is safe for fundamentals up to the top of MIDI octave m+2 (notes
// 12*(m+3) and below); HARM_LIMIT below is floor(24kHz / that frequency).
//
#include <memory>
#include <vector>

#include "config.h"

namespace msynth {

// Max harmonic count per mip level, derived from the top fundamental of the
// octave each level covers (baked; see plan §4 / gen math in wavetable_gen).
inline constexpr int HARM_LIMIT[MIP_LEVELS] = {366, 183, 91, 45, 22, 11, 5, 2, 1};

struct WaveTable {
    u8 frameCount = 1;
    // mipData[m] points at frameCount * mip_table_size(m) contiguous samples.
    const i32* mipData[MIP_LEVELS] = {};
    u8 mipLog2Size[MIP_LEVELS] = {};  // log2(mip_table_size(m))

    std::vector<i32> storage;  // owns all sample data (control thread only)

    // Allocate storage for `frames` frames across all mips and wire pointers.
    void allocate(int frames);
    i32* mutable_mip(int m) { return storage.data() + mip_offset_[m]; }

  private:
    size_t mip_offset_[MIP_LEVELS] = {};
};

using WaveTablePtr = std::shared_ptr<const WaveTable>;

// Mip level for a note whose maximum reachable pitch is max_cents (from MIDI
// note 0, Q16.16): one level per octave, clamped.
constexpr int mip_for_cents(i32 max_cents_q16) noexcept {
    const i32 oct = (max_cents_q16 >> 16) / 1200;
    const i32 m = oct - 2;
    return m < 0 ? 0 : (m >= MIP_LEVELS ? MIP_LEVELS - 1 : m);
}

}  // namespace msynth
