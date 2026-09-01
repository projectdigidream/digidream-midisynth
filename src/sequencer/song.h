#pragma once
//
// A loaded song: the parsed, merged event list plus everything precomputed
// for playback -- loop sample positions, loop cursor index, time-signature
// segments for bar/beat queries (plan §9/§10).
//
#include "../midi/smf_parser.h"

namespace msynth {

struct TimeSigSegment {
    u32 start_tick = 0;
    u32 start_bar = 0;        // bars completed before this segment
    u32 numerator = 4;
    u32 ticks_per_beat = 480; // division * 4 / denominator
};

struct Song {
    ParsedSong parsed;
    bool loaded = false;

    u64 loop_start_sample = 0;
    u64 loop_end_sample = 0;
    size_t loop_start_index = 0;  // first event at/after loop start

    std::vector<TimeSigSegment> timesigs;

    // Build derived data after parsing; loop overrides (from the game) are
    // ticks, applied before this (plan §9 priority order).
    void finalize();

    // Bar/beat at a song tick (both 0-based).
    void bar_beat_at_tick(u32 tick, u32& bar, u32& beat) const noexcept;
    // Tick where the next bar begins (for musical-boundary transitions).
    u32 next_bar_tick(u32 tick) const noexcept;
};

}  // namespace msynth
