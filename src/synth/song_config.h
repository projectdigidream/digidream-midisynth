#pragma once
//
// Song-level effect configuration (plan §13): reverb/chorus character is
// per-song, loaded alongside a song on the control thread and applied as
// engine commands. Defaults exist so songs without a config sound reasonable.
//
//   { "version": 1,
//     "reverb": { "roomSize": 0.8, "damping": 0.4, "width": 1.0, "wetGain": 0.3 },
//     "chorus": { "rateHz": 0.8, "depthMs": 3.5, "wetGain": 0.2 } }
//
#include <string>

#include "channel_eq.h"
#include "sidechain.h"
#include "master_chain.h"
#include "patch_loader.h"  // LoadResult
#include "synth_engine.h"

namespace msynth {

struct SongFxConfig {
    ReverbParams reverb;  // engine defaults
    ChorusParams chorus;
    DelayParams delay;    // delay/echo send (CC94); wet 0 = off
    MasterParams master;  // mastering chain; bypassed unless "master" says otherwise
    i32 tuning_cents_q16[12] = {};  // 12-tone microtuning offsets
    ChannelEqParams channel_eq[16];  // per MIDI channel (mixer strips)
    SidechainParams sidechain;       // kick-triggered ducking; depth 0 = off
};

// fx.json may carry an optional "master" object (same schema as master.json):
//   "master": { "enabled": true,
//               "eq": { "low": {"hz":120,"db":0}, "mid": {"hz":1000,"db":0,"q":1},
//                       "high": {"hz":8000,"db":0} },
//               "comp": { "thresholdDb":-12, "ratio":2, "attackMs":10,
//                         "releaseMs":150, "makeupDb":0, "kneeDb":6 },
//               "limiter": { "ceilingDb":-1, "lookaheadBlocks":2, "releaseMs":80 },
//               "width": 1.0, "gainDb": 0 }
LoadResult load_song_config(const char* json, size_t len, SongFxConfig& out);

// Queue the config onto the engine (applied at the next control block).
void push_song_config(SynthEngine& engine, const SongFxConfig& cfg);

// ---- Standalone master.json ----
// Accepts either { "version": 1, "master": {...} } or the bare master object.
LoadResult load_master_config(const char* json, size_t len, MasterParams& out);
// Queue only the mastering parameters (Enabled is pushed last so the chain
// starts clean with everything else already in place).
void push_master_config(SynthEngine& engine, const MasterParams& p);
// Serialize to the documented schema (exact Q16 round trip).
std::string master_to_json(const MasterParams& p, bool wrap_in_document = true);
// Q16.16 -> decimal text that the parser maps back exactly (shared rule).
std::string master_to_json_number(i32 q16);

}  // namespace msynth
