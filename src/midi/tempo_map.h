#pragma once
//
// Tempo map with precomputed cumulative sample anchors (plan Appendix E).
// ONE canonical tick->sample function, used by load, seek, loop and beat
// queries alike. Pure 64-bit integer math, multiply before divide.
//
#include <vector>

#include "../synth/config.h"

namespace msynth {

struct TempoSegment {
    u32 start_tick = 0;
    u64 start_sample = 0;        // cumulative anchor, computed at load
    u32 us_per_quarter = 500000; // default 120 BPM
    u64 samples_per_tick_fp = 0; // 32.32 fixed-point
};

class TempoMap {
  public:
    // division: PPQ ticks per quarter (SMPTE handled by the parser, which
    // converts to an equivalent PPQ before building the map).
    void build(u32 division, const std::vector<std::pair<u32, u32>>& tempo_events);

    // The canonical conversion. Binary-search the segment, then anchor +
    // ((tick - start_tick) * samples_per_tick_fp >> 32).
    u64 tick_to_sample(u32 tick) const noexcept;

    // Inverse (for seek-by-time): last tick whose sample position <= sample.
    u32 sample_to_tick(u64 sample) const noexcept;

    const std::vector<TempoSegment>& segments() const noexcept { return segments_; }
    u32 division() const noexcept { return division_; }

  private:
    std::vector<TempoSegment> segments_;
    u32 division_ = 480;
};

}  // namespace msynth
