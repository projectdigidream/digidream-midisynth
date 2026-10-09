#pragma once
//
// Flat shared voice pool with group-aware stealing (plan §8). Channel is a
// tag, not a partition.
//
#include <memory>

#include "voice.h"

namespace msynth {

struct PoolDiagnostics {
    i32 active_voices = 0;
    i32 peak_voices = 0;
    i64 steal_count = 0;
    i64 dropped_notes = 0;
};

class VoicePool {
  public:
    VoicePool() {
        for (int i = 0; i < MAX_VOICES; ++i) voices_[i].slot = static_cast<u8>(i);
    }

    u32 new_group_id() noexcept { return next_group_id_++; }

    // A free voice, or a stolen one (fast-released peers, envelope-continuous
    // reuse). Null if everything is steal-shielded (the note is dropped
    // rather than stuttering a fresh attack).
    Voice* alloc(u8 outgoing_bank) noexcept;

    Voice* begin() noexcept { return voices_.get(); }
    Voice* end() noexcept { return voices_.get() + MAX_VOICES; }
    const Voice* begin() const noexcept { return voices_.get(); }
    const Voice* end() const noexcept { return voices_.get() + MAX_VOICES; }

    // Group / channel operations.
    void note_off_group(u8 chan_slot, u8 note, bool pedal_down, u8 release_vel = 64) noexcept;
    // Per-note (polyphonic) aftertouch onto every sounding voice of the note.
    void poly_aftertouch(u8 chan_slot, u8 note, gain_t pressure_q16) noexcept;
    void release_pedal(u8 chan_slot) noexcept;
    void fast_release_channel(u8 chan_slot) noexcept;   // CC120 All Sound Off
    void release_channel(u8 chan_slot, bool pedal_down) noexcept;  // CC123
    void fast_release_all() noexcept;
    void fast_release_patch(const Patch* p) noexcept;  // hot-swap prep
    void retrigger_same_note(u8 chan_slot, u8 note) noexcept;
    void choke_group(u8 chan_slot, i8 exclusive_group, u32 keep_group) noexcept;

    PoolDiagnostics& diag() noexcept { return diag_; }
    void update_active_count() noexcept;

  private:
    // Heap-owned: a Voice now carries a 16 KB comb delay line, so the pool
    // is ~2.2 MB -- far too big to live inside stack-allocated engines.
    // Allocated once at construction (control thread), addresses fixed.
    std::unique_ptr<Voice[]> voices_ = std::make_unique<Voice[]>(MAX_VOICES);
    u32 next_group_id_ = 1;
    PoolDiagnostics diag_;
};

}  // namespace msynth
