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
#include "patch_loader.h"  // LoadResult
#include "synth_engine.h"

namespace msynth {

struct SongFxConfig {
    ReverbParams reverb;  // engine defaults
    ChorusParams chorus;
};

LoadResult load_song_config(const char* json, size_t len, SongFxConfig& out);

// Queue the config onto the engine (applied at the next control block).
void push_song_config(SynthEngine& engine, const SongFxConfig& cfg);

}  // namespace msynth
