#pragma once
//
// Modulation routing (plan §8). Evaluated once per control block: sources are
// gathered into a small array, routings accumulate into per-destination sums.
//
#include "fixmath.h"

namespace msynth {

enum class ModSource : u8 {
    LFO1, LFO2, AmpEnv, FilterEnv, PitchEnv, WtPosEnv,
    Velocity, KeyTrack, ModWheel, ChannelAftertouch, COUNT
};

enum class ModDest : u8 {
    Pitch,         // amount 1.0 = +/- one octave (1200 cents)
    FilterCutoff,  // amount 1.0 = full cutoff-table range (256 steps)
    FilterRes,     // amount 1.0 = +/- full resonance range
    Amplitude,     // gain multiplier (1 + sum), clamped [0, 1]
    WtPosition,    // amount 1.0 = full frame range
    Pan,           // amount 1.0 = full right
    COUNT
};

struct ModRouting {
    ModSource src = ModSource::LFO1;
    ModDest dst = ModDest::Pitch;
    gain_t amount = 0;  // Q16.16, signed
};

inline constexpr int MAX_ROUTINGS = 12;
inline constexpr int NUM_MOD_SOURCES = static_cast<int>(ModSource::COUNT);
inline constexpr int NUM_MOD_DESTS = static_cast<int>(ModDest::COUNT);

struct ModValues {
    i32 src[NUM_MOD_SOURCES] = {};  // Q16.16
    i64 dst[NUM_MOD_DESTS] = {};    // Q16.16 accumulated

    void evaluate(const ModRouting* routings, int count) noexcept {
        for (int d = 0; d < NUM_MOD_DESTS; ++d) dst[d] = 0;
        for (int i = 0; i < count; ++i) {
            const ModRouting& r = routings[i];
            dst[static_cast<int>(r.dst)] +=
                asr(static_cast<i64>(src[static_cast<int>(r.src)]) * r.amount, GAIN_SHIFT);
        }
    }
};

}  // namespace msynth
