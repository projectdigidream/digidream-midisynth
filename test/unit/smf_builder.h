#pragma once
//
// In-memory SMF construction for parser/sequencer tests.
//
#include <string>
#include <vector>

#include "synth/fixmath.h"

namespace testfw {

using msynth::u8;
using msynth::u32;

struct TrackBuilder {
    std::vector<u8> data;

    TrackBuilder& vlq(u32 v) {
        u8 buf[4];
        int n = 0;
        buf[n++] = v & 0x7F;
        while (v >>= 7) buf[n++] = static_cast<u8>(0x80 | (v & 0x7F));
        while (n) data.push_back(buf[--n]);
        return *this;
    }
    TrackBuilder& raw(std::initializer_list<int> bytes) {
        for (int b : bytes) data.push_back(static_cast<u8>(b));
        return *this;
    }
    TrackBuilder& ev(u32 delta, std::initializer_list<int> bytes) {
        vlq(delta);
        return raw(bytes);
    }
    TrackBuilder& note_on(u32 delta, int ch, int note, int vel) {
        return ev(delta, {0x90 | ch, note, vel});
    }
    TrackBuilder& note_off(u32 delta, int ch, int note) {
        return ev(delta, {0x80 | ch, note, 0});
    }
    TrackBuilder& cc(u32 delta, int ch, int num, int val) {
        return ev(delta, {0xB0 | ch, num, val});
    }
    TrackBuilder& program(u32 delta, int ch, int prog) {
        return ev(delta, {0xC0 | ch, prog});
    }
    TrackBuilder& bend(u32 delta, int ch, int value14) {  // 0..16383, 8192 = center
        return ev(delta, {0xE0 | ch, value14 & 0x7F, (value14 >> 7) & 0x7F});
    }
    TrackBuilder& tempo(u32 delta, u32 us_per_quarter) {
        vlq(delta);
        raw({0xFF, 0x51, 3});
        data.push_back(static_cast<u8>(us_per_quarter >> 16));
        data.push_back(static_cast<u8>(us_per_quarter >> 8));
        data.push_back(static_cast<u8>(us_per_quarter));
        return *this;
    }
    TrackBuilder& marker(u32 delta, const std::string& name) {
        vlq(delta);
        raw({0xFF, 0x06});
        vlq(static_cast<u32>(name.size()));
        for (char c : name) data.push_back(static_cast<u8>(c));
        return *this;
    }
    TrackBuilder& time_sig(u32 delta, int num, int denom_log2) {
        return ev(delta, {0xFF, 0x58, 4, num, denom_log2, 24, 8});
    }
    TrackBuilder& end(u32 delta = 0) { return ev(delta, {0xFF, 0x2F, 0}); }
};

inline std::vector<u8> build_smf(int format, u32 division,
                                 const std::vector<TrackBuilder>& tracks) {
    std::vector<u8> out;
    auto be16 = [&](u32 v) {
        out.push_back(static_cast<u8>(v >> 8));
        out.push_back(static_cast<u8>(v));
    };
    auto be32 = [&](u32 v) {
        out.push_back(static_cast<u8>(v >> 24));
        out.push_back(static_cast<u8>(v >> 16));
        out.push_back(static_cast<u8>(v >> 8));
        out.push_back(static_cast<u8>(v));
    };
    out.insert(out.end(), {'M', 'T', 'h', 'd'});
    be32(6);
    be16(static_cast<u32>(format));
    be16(static_cast<u32>(tracks.size()));
    be16(division);
    for (const auto& t : tracks) {
        out.insert(out.end(), {'M', 'T', 'r', 'k'});
        be32(static_cast<u32>(t.data.size()));
        out.insert(out.end(), t.data.begin(), t.data.end());
    }
    return out;
}

}  // namespace testfw
