#pragma once
//
// Minimal 16-bit stereo PCM WAV writer (offline render / golden tests).
//
#include <cstdio>
#include <string>
#include <vector>

#include "../synth/fixmath.h"

namespace msynth {

inline bool write_wav16(const std::string& path, const std::vector<i16>& interleaved,
                        u32 sample_rate, u16 channels) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const u32 data_bytes = static_cast<u32>(interleaved.size() * 2);
    const u32 byte_rate = sample_rate * channels * 2;
    const u16 block_align = static_cast<u16>(channels * 2);

    auto w32 = [&](u32 v) { std::fwrite(&v, 4, 1, f); };
    auto w16 = [&](u16 v) { std::fwrite(&v, 2, 1, f); };

    std::fwrite("RIFF", 1, 4, f);
    w32(36 + data_bytes);
    std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f);
    w32(16);
    w16(1);  // PCM
    w16(channels);
    w32(sample_rate);
    w32(byte_rate);
    w16(block_align);
    w16(16);
    std::fwrite("data", 1, 4, f);
    w32(data_bytes);
    std::fwrite(interleaved.data(), 2, interleaved.size(), f);
    std::fclose(f);
    return true;
}

}  // namespace msynth
