#pragma once
//
// Controller/note state at a point in a song -- the data behind the seek
// chase (§10), bake-time cutting and live song replacement. Walks the merged
// event list once; nothing here touches the audio thread.
//
#include "../midi/smf_parser.h"

namespace msynth {

struct ChannelChase {
    int program = -1;            // -1 = never set
    int cc[128];                 // -1 = never set (RPN plumbing CCs excluded)
    int bend = -1;               // raw 14-bit, -1 = never set
    int bend_range_semis = -1;   // RPN 0,0 data entry
    ChannelChase() { for (int& v : cc) v = -1; }
};

struct SongChase {
    ChannelChase ch[16];
    int open[16][128] = {};      // note-on depth per (channel, note)
    int timesig_num = -1, timesig_denom_log2 = 2;
};

inline bool chase_is_plumbing_cc(u8 cc) {
    return cc == 6 || cc == 38 || cc == 98 || cc == 99 || cc == 100 || cc == 101 ||
           cc == 120 || cc == 121 || cc == 123;
}

// State after all events with (by_sample ? sample < upto_sample : tick < upto_tick).
inline void collect_chase(const ParsedSong& song, bool by_sample, u64 upto_sample,
                          u32 upto_tick, SongChase& out) {
    out = SongChase{};
    int rpn_msb[16], rpn_lsb[16];
    for (int i = 0; i < 16; ++i) rpn_msb[i] = rpn_lsb[i] = 127;
    for (const MidiEvent& e : song.events) {
        if (by_sample ? e.sample >= upto_sample : e.tick >= upto_tick) break;
        if (e.status == MidiEvent::META_TIME_SIG) {
            out.timesig_num = e.d1;
            out.timesig_denom_log2 = e.d2;
            continue;
        }
        if (!e.is_channel()) continue;
        const u8 ch = e.channel();
        ChannelChase& c = out.ch[ch];
        switch (e.kind()) {
            case 0x90: out.open[ch][e.d1]++; break;
            case 0x80: if (out.open[ch][e.d1]) out.open[ch][e.d1]--; break;
            case 0xB0:
                if (e.d1 == 101) rpn_msb[ch] = e.d2;
                else if (e.d1 == 100) rpn_lsb[ch] = e.d2;
                else if (e.d1 == 6) {
                    if (rpn_msb[ch] == 0 && rpn_lsb[ch] == 0) c.bend_range_semis = e.d2;
                } else if (e.d1 == 120 || e.d1 == 123) {
                    for (int n = 0; n < 128; ++n) out.open[ch][n] = 0;
                } else if (!chase_is_plumbing_cc(e.d1)) {
                    c.cc[e.d1] = e.d2;
                }
                break;
            case 0xC0: c.program = e.d1; break;
            case 0xE0: c.bend = (static_cast<int>(e.d2) << 7) | e.d1; break;
            default: break;
        }
    }
}

}  // namespace msynth
