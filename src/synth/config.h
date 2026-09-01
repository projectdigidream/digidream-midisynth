#pragma once
//
// Confirmed defaults (plan §16). Fixed on all platforms -- resampling, if a
// device demands another rate, happens at the OS boundary, outside the
// deterministic path.
//
#include "fixmath.h"

namespace msynth {

inline constexpr int SAMPLE_RATE   = 48000;
inline constexpr int BUFFER_FRAMES = 512;  // multiple of AudioWorklet's 128
inline constexpr int CONTROL_BLOCK = 64;   // control-rate granularity (~1.3ms)
inline constexpr int BLOCKS_PER_BUFFER = BUFFER_FRAMES / CONTROL_BLOCK;
inline constexpr int CONTROL_RATE  = SAMPLE_RATE / CONTROL_BLOCK;  // 750 Hz

inline constexpr int TABLE_SIZE = 2048;  // base mip level; halves per level
inline constexpr int MIP_LEVELS = 9;     // ~one per octave of MIDI range
inline constexpr int MAX_FRAMES = 32;    // wavetable morph resolution
inline constexpr int MAX_LAYERS = 4;     // oscillator layers per note
inline constexpr int MAX_NOTES  = 32;    // target simultaneous notes
inline constexpr int MAX_LFOS   = 2;     // per voice
inline constexpr int MAX_VOICES = MAX_NOTES * MAX_LAYERS;  // 128

inline constexpr int NUM_MIDI_CHANNELS = 16;
// Two sequencer instances (crossfade) each own 16 channel slots.
inline constexpr int NUM_CHANNEL_SLOTS = 32;
inline constexpr int DRUM_CHANNEL = 9;  // per 16-channel bank

// Table size halves per mip but never below MIN_TABLE_SIZE: 4-point Hermite
// on an 8-sample cycle leaves interpolation spurs at only ~-32 dB (audible
// hash on top-octave notes); 64 samples/cycle keeps them below ~-68 dB.
// Total per frame: 2048+1024+512+256+128+64+64+64+64 = 4234 (~2x base).
inline constexpr int MIN_TABLE_SIZE = 64;
constexpr int mip_table_size(int mip) noexcept {
    const int n = TABLE_SIZE >> mip;
    return n < MIN_TABLE_SIZE ? MIN_TABLE_SIZE : n;
}

}  // namespace msynth
