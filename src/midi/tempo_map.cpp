#include "tempo_map.h"

namespace msynth {

void TempoMap::build(u32 division, const std::vector<std::pair<u32, u32>>& tempo_events) {
    division_ = division == 0 ? 480 : division;
    segments_.clear();

    auto samples_per_tick = [&](u32 us_per_quarter) -> u64 {
        // Multiply before divide, 128-bit intermediate: exact and identical
        // everywhere (Appendix E determinism rules).
        const dnp::u128 num = static_cast<dnp::u128>(static_cast<u64>(us_per_quarter) *
                                                     SAMPLE_RATE)
                              << 32;
        const u64 den = static_cast<u64>(division_) * 1000000ull;
        return static_cast<u64>(num / den);
    };

    // Leading default segment (120 BPM) if the song doesn't set tempo at 0.
    TempoSegment cur;
    cur.start_tick = 0;
    cur.start_sample = 0;
    cur.us_per_quarter = 500000;
    cur.samples_per_tick_fp = samples_per_tick(500000);

    for (const auto& [tick, us] : tempo_events) {
        if (us == 0) continue;  // corrupt tempo: ignore
        if (tick == cur.start_tick) {
            // Replaces the segment starting here (incl. tick-0 default).
            cur.us_per_quarter = us;
            cur.samples_per_tick_fp = samples_per_tick(us);
            continue;
        }
        // Close the current segment at `tick` and anchor the next one.
        const u64 span = static_cast<u64>(
            (static_cast<dnp::u128>(tick - cur.start_tick) * cur.samples_per_tick_fp) >> 32);
        segments_.push_back(cur);
        TempoSegment next;
        next.start_tick = tick;
        next.start_sample = cur.start_sample + span;
        next.us_per_quarter = us;
        next.samples_per_tick_fp = samples_per_tick(us);
        cur = next;
    }
    segments_.push_back(cur);
}

u64 TempoMap::tick_to_sample(u32 tick) const noexcept {
    if (segments_.empty()) return 0;
    // Binary search: last segment with start_tick <= tick.
    size_t lo = 0, hi = segments_.size() - 1;
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        if (segments_[mid].start_tick <= tick) lo = mid;
        else hi = mid - 1;
    }
    const TempoSegment& s = segments_[lo];
    const u64 dt = tick - s.start_tick;
    return s.start_sample +
           static_cast<u64>((static_cast<dnp::u128>(dt) * s.samples_per_tick_fp) >> 32);
}

u32 TempoMap::sample_to_tick(u64 sample) const noexcept {
    if (segments_.empty()) return 0;
    size_t lo = 0, hi = segments_.size() - 1;
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        if (segments_[mid].start_sample <= sample) lo = mid;
        else hi = mid - 1;
    }
    const TempoSegment& s = segments_[lo];
    if (s.samples_per_tick_fp == 0) return s.start_tick;
    const dnp::u128 dt = (static_cast<dnp::u128>(sample - s.start_sample) << 32) /
                         s.samples_per_tick_fp;
    return s.start_tick + static_cast<u32>(dt);
}

}  // namespace msynth
