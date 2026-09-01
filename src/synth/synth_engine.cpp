#include "synth_engine.h"

namespace msynth {

SynthEngine::SynthEngine() {
    tables_init();
    for (int i = 0; i < NUM_CHANNEL_SLOTS; ++i)
        channels_[i].full_reset((i & 15) == DRUM_CHANNEL);
}

i32 SynthEngine::active_note_count(int slot) const noexcept {
    // Distinct groups still sounding on the slot.
    i32 n = 0;
    u32 seen[MAX_VOICES];
    for (const Voice& v : pool_) {
        if (!v.active || v.chan_slot != slot) continue;
        bool dup = false;
        for (i32 j = 0; j < n; ++j)
            if (seen[j] == v.group_id) { dup = true; break; }
        if (!dup) seen[n++] = v.group_id;
    }
    return n;
}

int SynthEngine::active_notes(int slot, u8* out, int max) const noexcept {
    int n = 0;
    for (const Voice& v : pool_) {
        if (!v.active || v.chan_slot != slot || v.released) continue;
        bool dup = false;
        for (int j = 0; j < n; ++j)
            if (out[j] == v.note) { dup = true; break; }
        if (!dup && n < max) out[n++] = v.note;
    }
    return n;
}

void SynthEngine::note_on(u8 slot, u8 note, u8 vel) noexcept {
    if (!bank_) return;
    ChannelState& ch = channels_[slot];
    if (vel == 0) {  // note-on velocity 0 == note-off (also normalized upstream)
        pool_.note_off_group(slot, note, ch.sustain);
        return;
    }
    const Patch* patch = nullptr;
    if (ch.is_drum) {
        if (!kit_) return;
        patch = kit_->lookup(note);
        if (!patch) return;  // unmapped drum note: silent by design
    } else {
        patch = bank_->lookup(ch.program);
    }

    // Same-note retrigger policy: existing group -> fast release, new group
    // starts (retrigger-with-overlap, plan §10).
    pool_.retrigger_same_note(slot, note);

    const u32 group = pool_.new_group_id();
    for (u8 li = 0; li < patch->layer_count; ++li) {
        const LayerDef& l = patch->layers[li];
        if (vel < l.vel_low || vel > l.vel_high) continue;
        Voice* v = pool_.alloc(outgoing_bank_);
        if (!v) break;  // fully shielded pool: drop rather than stutter
        v->start(v->slot, slot, note, vel, group, li, patch, &l, ch);
    }
    // Drum choke (closed hat cuts open hat) -- after allocation so the new
    // group survives its own choke group.
    if (ch.is_drum && patch->exclusive_group >= 0)
        pool_.choke_group(slot, patch->exclusive_group, group);
}

void SynthEngine::handle_data_entry(ChannelState& ch, u8 msb) noexcept {
    if (ch.nrpn_active) return;  // NRPNs: recognized, ignored
    if (ch.rpn_msb == 0 && ch.rpn_lsb == 0) {
        // Bend range, semitones (MSB only; LSB cents ignored at this level).
        ch.bend_range_cents = static_cast<i32>(msb) * 100;
        if (ch.bend_range_cents > 4800) ch.bend_range_cents = 4800;
    } else if (ch.rpn_msb == 0 && ch.rpn_lsb == 1) {
        // Fine tune: -100..+100 cents across 0..127.
        ch.fine_tune_cents_q16 =
            static_cast<i32>(asl((static_cast<i64>(msb) - 64) * 100, GAIN_SHIFT) / 64);
    } else if (ch.rpn_msb == 0 && ch.rpn_lsb == 2) {
        ch.coarse_tune_cents = (static_cast<i32>(msb) - 64) * 100;
    }
}

void SynthEngine::handle_cc(u8 slot, u8 cc, u8 val) noexcept {
    ChannelState& ch = channels_[slot];
    switch (cc) {
        case 0: ch.bank = val; break;
        case 32: break;  // bank LSB: recognized, single-bank build
        case 1: ch.modwheel_q16 = (static_cast<i32>(val) << GAIN_SHIFT) / 127; break;
        case 5: case 65: break;  // portamento: recognized and ignored (plan scope)
        case 6: handle_data_entry(ch, val); break;
        case 38: break;  // data entry LSB
        case 7: ch.volume = val; break;
        case 10: ch.pan_q16 = static_cast<i32>(asl(static_cast<i64>(val) - 64, GAIN_SHIFT) / 64); break;
        case 11: ch.expression = val; break;
        case 64: {
            const bool down = val >= 64;
            if (ch.sustain && !down) pool_.release_pedal(slot);
            ch.sustain = down;
            break;
        }
        case 71: ch.res_offset_q16 = static_cast<i32>(asl(static_cast<i64>(val) - 64, GAIN_SHIFT) / 128); break;
        case 74: ch.cutoff_offset_q16 = static_cast<i32>(asl(static_cast<i64>(val) - 64, GAIN_SHIFT)); break;
        case 91: ch.send_reverb = val; break;
        case 93: ch.send_chorus = val; break;
        case 98: case 99: ch.nrpn_active = true; break;
        case 100: ch.rpn_lsb = val; ch.nrpn_active = false; break;
        case 101: ch.rpn_msb = val; ch.nrpn_active = false; break;
        case 120: pool_.fast_release_channel(slot); break;  // All Sound Off
        case 121: ch.reset_controllers(); break;
        case 123: pool_.release_channel(slot, ch.sustain); break;  // All Notes Off
        default: break;
    }
}

void SynthEngine::apply_command(const EngineCommand& c) noexcept {
    switch (c.type) {
        case EngineCommand::Type::NoteOn: note_on(c.slot, c.d1, c.d2); break;
        case EngineCommand::Type::NoteOff:
            pool_.note_off_group(c.slot, c.d1, channels_[c.slot].sustain);
            break;
        case EngineCommand::Type::CC: handle_cc(c.slot, c.d1, c.d2); break;
        case EngineCommand::Type::Program:
            if (!channels_[c.slot].is_drum) channels_[c.slot].program = c.d1;
            break;
        case EngineCommand::Type::PitchBend:
            channels_[c.slot].bend_raw = clamp_i32(c.v, -8192, 8191);
            break;
        case EngineCommand::Type::Aftertouch:
            channels_[c.slot].aftertouch_q16 = (static_cast<i32>(c.d1) << GAIN_SHIFT) / 127;
            break;
        case EngineCommand::Type::ChannelFade: {
            ChannelState& ch = channels_[c.slot];
            ch.fade_target_q16 = clamp_gain01(c.v);
            const i32 blocks = c.v2 > 0 ? c.v2 : 1;
            i32 dist = ch.fade_target_q16 - ch.fade_gain_q16;
            if (dist < 0) dist = -dist;
            ch.fade_step_q16 = dist / blocks;
            if (ch.fade_step_q16 == 0) ch.fade_step_q16 = 1;
            break;
        }
        case EngineCommand::Type::MasterGain:
            out_stage_.master_gain = clamp_i32(c.v, 0, 4 * GAIN_ONE);
            break;
        case EngineCommand::Type::DitherEnable: out_stage_.dither = c.d1 != 0; break;
        case EngineCommand::Type::ReverbConfig:
            switch (c.d1) {
                case 0: reverb_params_.room_size = clamp_gain01(c.v); break;
                case 1: reverb_params_.damping = clamp_gain01(c.v); break;
                case 2: reverb_params_.width = clamp_gain01(c.v); break;
                case 3: reverb_params_.wet_gain = clamp_i32(c.v, 0, 2 * GAIN_ONE); break;
                default: break;
            }
            reverb_.set_params(reverb_params_);
            break;
        case EngineCommand::Type::ChorusConfig:
            switch (c.d1) {
                case 0: chorus_params_.rate_hz_q16 = clamp_i32(c.v, 0, 10 * GAIN_ONE); break;
                case 1: chorus_params_.depth_ms_q16 = clamp_i32(c.v, 0, 20 * GAIN_ONE); break;
                case 2: chorus_params_.wet_gain = clamp_i32(c.v, 0, 2 * GAIN_ONE); break;
                default: break;
            }
            chorus_.set_params(chorus_params_);
            break;
        case EngineCommand::Type::ResetBank: {
            const int base = c.d1 ? 16 : 0;
            for (int i = 0; i < NUM_MIDI_CHANNELS; ++i)
                channels_[base + i].full_reset(i == DRUM_CHANNEL);
            break;
        }
        case EngineCommand::Type::AllSoundOffBank: {
            const int base = c.d1 ? 16 : 0;
            for (int i = 0; i < NUM_MIDI_CHANNELS; ++i)
                pool_.fast_release_channel(static_cast<u8>(base + i));
            break;
        }
        case EngineCommand::Type::SetOutgoingBank: outgoing_bank_ = c.d1; break;
        case EngineCommand::Type::Duck: out_stage_.set_duck(c.v, c.v2); break;
    }
}

void SynthEngine::render_one_block(i16* out) noexcept {
    // Apply everything due by the end of this block (timing resolution =
    // CONTROL_BLOCK, plan §7).
    EngineCommand c;
    while (queue_.peek(c) && c.when <= sample_time_) {
        queue_.pop();
        apply_command(c);
    }

    for (int i = 0; i < CONTROL_BLOCK; ++i) {
        dry_l_[i] = dry_r_[i] = 0;
        rev_l_[i] = rev_r_[i] = 0;
        cho_l_[i] = cho_r_[i] = 0;
    }

    for (int s = 0; s < NUM_CHANNEL_SLOTS; ++s) channels_[s].advance_fade_block();
    out_stage_.advance_duck_block();

    const VoiceBuses buses{dry_l_, dry_r_, rev_l_, rev_r_, cho_l_, cho_r_};
    for (Voice& v : pool_)
        if (v.active) v.render_block(channels_[v.chan_slot], buses);
    pool_.update_active_count();
    pool_diag_public_ = pool_.diag();

    // Send effects return into the dry accumulators, then master out.
    reverb_.process_block(rev_l_, rev_r_, dry_l_, dry_r_, CONTROL_BLOCK);
    chorus_.process_block(cho_l_, cho_r_, dry_l_, dry_r_, CONTROL_BLOCK);

    for (int i = 0; i < CONTROL_BLOCK; ++i) {
        out[2 * i] = out_stage_.convert(dry_l_[i]);
        out[2 * i + 1] = out_stage_.convert(dry_r_[i]);
    }
    sample_time_ += CONTROL_BLOCK;
}

void SynthEngine::render(i16* out_interleaved, int frames) noexcept {
    for (int done = 0; done + CONTROL_BLOCK <= frames; done += CONTROL_BLOCK)
        render_one_block(out_interleaved + 2 * done);
}

}  // namespace msynth
