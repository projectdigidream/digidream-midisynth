#include "voice_pool.h"

namespace msynth {

namespace {

constexpr gain_t QUIET_LEVEL = GAIN_ONE / 50;  // -34 dB-ish

// Steal priority (plan §8): released-and-quiet -> oldest released -> quietest
// sustaining -> oldest sustaining. Lower key steals first. Voices from the
// outgoing crossfade bank get their class halved -- they're fading anyway.
u64 steal_key(const Voice& v, bool group_fully_released, u8 outgoing_bank) noexcept {
    u64 cls;
    const gain_t lvl = v.amp_env.level;
    if (v.released && lvl < QUIET_LEVEL) cls = 2;
    else if (v.released) cls = 4;
    else cls = 6;
    // Group-aware: stripping a layer from a still-active group is worse than
    // taking a voice whose whole group is on the way out.
    if (!group_fully_released) cls += 1;
    if (outgoing_bank != 0xFF && (v.chan_slot >> 4) == outgoing_bank && cls > 2) cls -= 1;

    u64 key = cls << 56;
    if (cls >= 6) key |= static_cast<u64>(static_cast<u32>(lvl)) << 24;  // quietest first
    key |= static_cast<u64>(0xFFFFFFFFu - (v.age_blocks > 0xFFFFFFFEu ? 0xFFFFFFFEu : v.age_blocks));
    return key;
}

}  // namespace

Voice* VoicePool::alloc(u8 outgoing_bank) noexcept {
    for (auto& v : voices_)
        if (!v.active) return &v;

    // Precompute per-group "fully released" in one pass.
    bool group_released[MAX_VOICES];
    for (int i = 0; i < MAX_VOICES; ++i) {
        const Voice& v = voices_[i];
        bool all = true;
        for (const auto& w : voices_)
            if (w.active && w.group_id == v.group_id && !w.released) { all = false; break; }
        group_released[i] = all;
    }

    Voice* best = nullptr;
    u64 best_key = ~0ull;
    for (int i = 0; i < MAX_VOICES; ++i) {
        Voice& v = voices_[i];
        if (v.steal_shield > 0) continue;  // never steal a just-born voice
        const u64 key = steal_key(v, group_released[i], outgoing_bank);
        if (key < best_key) { best_key = key; best = &v; }
    }
    if (!best) {
        ++diag_.dropped_notes;
        return nullptr;
    }
    ++diag_.steal_count;
    // Peers of the victim's group fade out fast (never a hard cut); the victim
    // itself is reused directly -- its envelope re-enters Attack from the
    // current level (Appendix C rule 3), which is the click-free path.
    for (auto& w : voices_)
        if (w.active && &w != best && w.group_id == best->group_id && w.released) w.fast_release();
    best->active = false;
    return best;
}

void VoicePool::note_off_group(u8 chan_slot, u8 note, bool pedal_down) noexcept {
    for (auto& v : voices_)
        if (v.active && v.chan_slot == chan_slot && v.note == note && !v.released)
            v.note_off(pedal_down);
}

void VoicePool::release_pedal(u8 chan_slot) noexcept {
    for (auto& v : voices_)
        if (v.active && v.chan_slot == chan_slot) v.release_pedal();
}

void VoicePool::fast_release_channel(u8 chan_slot) noexcept {
    for (auto& v : voices_)
        if (v.active && v.chan_slot == chan_slot) v.fast_release();
}

void VoicePool::release_channel(u8 chan_slot, bool pedal_down) noexcept {
    for (auto& v : voices_)
        if (v.active && v.chan_slot == chan_slot && !v.released) v.note_off(pedal_down);
}

void VoicePool::fast_release_all() noexcept {
    for (auto& v : voices_) v.fast_release();
}

void VoicePool::fast_release_patch(const Patch* p) noexcept {
    for (auto& v : voices_)
        if (v.active && v.patch == p) v.fast_release();
}

void VoicePool::retrigger_same_note(u8 chan_slot, u8 note) noexcept {
    // GM-typical retrigger-with-overlap: the old group goes into fast release.
    for (auto& v : voices_)
        if (v.active && v.chan_slot == chan_slot && v.note == note && !v.released)
            v.fast_release();
}

void VoicePool::choke_group(u8 chan_slot, i8 exclusive_group, u32 keep_group) noexcept {
    if (exclusive_group < 0) return;
    for (auto& v : voices_)
        if (v.active && v.chan_slot == chan_slot && v.group_id != keep_group &&
            v.patch && v.patch->exclusive_group == exclusive_group)
            v.fast_release();
}

void VoicePool::update_active_count() noexcept {
    i32 n = 0;
    for (const auto& v : voices_)
        if (v.active) ++n;
    diag_.active_voices = n;
    if (n > diag_.peak_voices) diag_.peak_voices = n;
}

}  // namespace msynth
