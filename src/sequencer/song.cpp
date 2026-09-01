#include "song.h"

#include <algorithm>

namespace msynth {

void Song::finalize() {
    const ParsedSong& p = parsed;

    loop_start_sample = p.tempo.tick_to_sample(p.loop_start_tick);
    loop_end_sample = p.tempo.tick_to_sample(p.loop_end_tick);
    if (loop_end_sample <= loop_start_sample) {
        loop_start_sample = 0;
        loop_end_sample = p.end_sample;
    }

    loop_start_index = static_cast<size_t>(
        std::lower_bound(p.events.begin(), p.events.end(), loop_start_sample,
                         [](const MidiEvent& e, u64 s) { return e.sample < s; }) -
        p.events.begin());

    // Time-signature segments (default 4/4 until the first meta).
    timesigs.clear();
    TimeSigSegment cur;
    cur.ticks_per_beat = p.tempo.division();
    for (const MidiEvent& e : p.events) {
        if (e.status != MidiEvent::META_TIME_SIG) continue;
        const u32 num = e.d1 ? e.d1 : 4;
        const u32 denom = 1u << (e.d2 > 6 ? 6 : e.d2);
        u32 tpb = (p.tempo.division() * 4) / (denom ? denom : 4);
        if (tpb == 0) tpb = 1;
        if (e.tick == cur.start_tick) {
            cur.numerator = num;
            cur.ticks_per_beat = tpb;
            continue;
        }
        // Close current segment: count the bars it spans.
        const u32 span = e.tick - cur.start_tick;
        const u32 ticks_per_bar = cur.ticks_per_beat * cur.numerator;
        TimeSigSegment next;
        next.start_tick = e.tick;
        next.start_bar = cur.start_bar + span / (ticks_per_bar ? ticks_per_bar : 1);
        next.numerator = num;
        next.ticks_per_beat = tpb;
        timesigs.push_back(cur);
        cur = next;
    }
    timesigs.push_back(cur);
    loaded = true;
}

void Song::bar_beat_at_tick(u32 tick, u32& bar, u32& beat) const noexcept {
    bar = beat = 0;
    if (timesigs.empty()) return;
    size_t lo = 0;
    for (size_t i = 0; i < timesigs.size(); ++i)
        if (timesigs[i].start_tick <= tick) lo = i;
        else break;
    const TimeSigSegment& s = timesigs[lo];
    const u32 dt = tick - s.start_tick;
    const u32 ticks_per_bar = s.ticks_per_beat * s.numerator;
    if (ticks_per_bar == 0) return;
    bar = s.start_bar + dt / ticks_per_bar;
    beat = (dt % ticks_per_bar) / (s.ticks_per_beat ? s.ticks_per_beat : 1);
}

u32 Song::next_bar_tick(u32 tick) const noexcept {
    if (timesigs.empty()) return tick;
    size_t lo = 0;
    for (size_t i = 0; i < timesigs.size(); ++i)
        if (timesigs[i].start_tick <= tick) lo = i;
        else break;
    const TimeSigSegment& s = timesigs[lo];
    const u32 ticks_per_bar = s.ticks_per_beat * s.numerator;
    if (ticks_per_bar == 0) return tick;
    const u32 dt = tick - s.start_tick;
    return s.start_tick + (dt / ticks_per_bar + 1) * ticks_per_bar;
}

}  // namespace msynth
