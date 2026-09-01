#pragma once
//
// Standard MIDI File parser (plan §9, Appendix E). Formats 0/1 fully, format
// 2 loads track 0 with a warning. Salvage-don't-crash: a corrupt track keeps
// its events up to the corruption point; nothing salvageable = empty song +
// error report.
//
#include <string>
#include <vector>

#include "event.h"
#include "tempo_map.h"

namespace msynth {

struct ParsedSong {
    std::vector<MidiEvent> events;  // merged, sorted by (tick, track, seq),
                                    // sample positions baked
    TempoMap tempo;
    u32 division = 480;
    u32 end_tick = 0;      // last event / end-of-track tick
    u64 end_sample = 0;
    // Loop points (plan §9 priority): markers "loopStart"/"loopEnd" or CC111.
    bool has_loop_start = false, has_loop_end = false;
    u32 loop_start_tick = 0, loop_end_tick = 0;
};

struct ParseResult {
    bool ok = false;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

ParseResult smf_parse(const u8* data, size_t len, ParsedSong& out);

}  // namespace msynth
