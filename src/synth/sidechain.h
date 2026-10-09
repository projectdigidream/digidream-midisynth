#pragma once
//
// Sidechain ducker: a note on one channel (the kick) pulls down the level of
// others (pads, bass) -- the pumping that dance music breathes with.
//
// It is TRIGGERED, not level-following: the gain is a pure function of how
// many samples have passed since the last qualifying note-on, so it needs no
// envelope follower, cannot drift, and is bit-identical on every machine.
// depth = 0 means the whole stage is skipped and lanes are untouched.
//
#include "config.h"
#include "fixmath.h"

namespace msynth {

struct SidechainParams {
    i32 source = -1;          // triggering MIDI channel, -1 = off
    i32 note = -1;            // only this note triggers, -1 = any
    u32 targets = 0;          // bitmask of channels to duck (bit n = channel n)
    gain_t depth = 0;         // 0..1 gain reduction at full duck (0 = off)
    gain_t attack_ms_q16 = 5 << GAIN_SHIFT;
    gain_t hold_ms_q16 = 15 << GAIN_SHIFT;
    gain_t release_ms_q16 = 180 << GAIN_SHIFT;

    bool active() const noexcept { return depth > 0 && source >= 0 && targets != 0; }
};

class Sidechain {
  public:
    void set_params(const SidechainParams& p) noexcept {
        p_ = p;
        attack_ = ms_to_samples(p.attack_ms_q16);
        hold_ = ms_to_samples(p.hold_ms_q16);
        release_ = ms_to_samples(p.release_ms_q16);
    }
    const SidechainParams& params() const noexcept { return p_; }
    bool active() const noexcept { return p_.active(); }
    bool ducks(int channel) const noexcept {
        return p_.active() && (p_.targets & (1u << (channel & 31))) != 0;
    }

    // A note-on the engine just dispatched; returns true if it fired us.
    bool on_note(int channel, int note) noexcept {
        if (!p_.active() || channel != p_.source) return false;
        if (p_.note >= 0 && note != p_.note) return false;
        pos_ = 0;
        return true;
    }

    void clear() noexcept { pos_ = IDLE; }

    // Gain at `offset` samples past the current position, Q16. 1.0 when idle.
    gain_t gain_at(u32 offset) const noexcept {
        if (pos_ == IDLE) return GAIN_ONE;
        const u64 t = static_cast<u64>(pos_) + offset;
        if (t < attack_)
            return GAIN_ONE - static_cast<gain_t>(
                       (static_cast<i64>(p_.depth) * static_cast<i64>(t)) / attack_);
        if (t < static_cast<u64>(attack_) + hold_) return GAIN_ONE - p_.depth;
        const u64 r = t - attack_ - hold_;
        if (r >= release_) return GAIN_ONE;
        return GAIN_ONE - static_cast<gain_t>(
                   (static_cast<i64>(p_.depth) * static_cast<i64>(release_ - r)) / release_);
    }

    // Advance past a rendered block; idle once the release has finished so a
    // silent song costs nothing.
    void advance(u32 frames) noexcept {
        if (pos_ == IDLE) return;
        pos_ += frames;
        if (pos_ >= attack_ + hold_ + release_) pos_ = IDLE;
    }

  private:
    static constexpr u32 IDLE = 0xFFFFFFFFu;
    static u32 ms_to_samples(gain_t ms_q16) noexcept {
        const i64 s = asr(static_cast<i64>(ms_q16) * SAMPLE_RATE / 1000, GAIN_SHIFT);
        return static_cast<u32>(clamp_i32(s, 1, SAMPLE_RATE * 4));  // never divide by zero
    }

    SidechainParams p_;
    u32 attack_ = 1, hold_ = 1, release_ = 1;
    u32 pos_ = IDLE;
};

}  // namespace msynth
