#pragma once
//
// MidiEvent: one merged, time-ordered event. Sample positions are baked at
// load time by the canonical tempo-map conversion (plan Appendix E, option A)
// -- runtime playback is a cursor over a sorted-by-sample list.
//
#include "../synth/fixmath.h"

namespace msynth {

struct MidiEvent {
    u64 sample = 0;    // absolute sample position at load tempo
    u32 tick = 0;
    u8 status = 0;     // channel status byte (0x80..0xEF), or meta marker below
    u8 d1 = 0, d2 = 0;
    u16 track = 0;
    u32 seq = 0;       // position within track (merge tiebreak)

    // Meta events the sequencer cares about are folded into pseudo-status
    // values (real MIDI status bytes never take these values):
    static constexpr u8 META_TEMPO = 0x01;      // d1..: tempo handled by TempoMap
    static constexpr u8 META_LOOP_START = 0x02;
    static constexpr u8 META_LOOP_END = 0x03;
    static constexpr u8 META_TIME_SIG = 0x04;   // d1 = numerator, d2 = denom log2
    static constexpr u8 META_END = 0x05;        // end of song

    bool is_channel() const noexcept { return status >= 0x80; }
    u8 channel() const noexcept { return status & 0x0F; }
    u8 kind() const noexcept { return status & 0xF0; }
};

}  // namespace msynth
