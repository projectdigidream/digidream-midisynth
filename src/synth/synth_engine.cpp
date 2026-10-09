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

    // Round robin + glide bookkeeping (channel-scoped, deterministic).
    const u32 rr = ch.rr_counter++;
    const i32 target_cents = (static_cast<i32>(note) * 100) << GAIN_SHIFT;
    const bool glide = ch.portamento && !ch.is_drum && ch.last_note_cents_q16 >= 0 &&
                       ch.last_note_cents_q16 != target_cents && ch.portamento_blocks > 0;
    const i32 glide_from = ch.last_note_cents_q16;
    if (!ch.is_drum) ch.last_note_cents_q16 = target_cents;

    const u32 group = pool_.new_group_id();
    for (u8 li = 0; li < patch->layer_count; ++li) {
        const LayerDef& l = patch->layers[li];
        if (vel < l.vel_low || vel > l.vel_high) continue;
        if (l.rr_size > 1 && (rr % l.rr_size) != l.rr_index) continue;  // round robin
        const int uni = l.unison_voices > 1 ? l.unison_voices : 1;
        for (int u = 0; u < uni; ++u) {
            Voice* v = pool_.alloc(outgoing_bank_);
            if (!v) break;  // fully shielded pool: drop rather than stutter
            v->start(v->slot, slot, note, vel, group, li, patch, &l, ch,
                     sample_time_ / CONTROL_BLOCK, tempo_bpm_q16_);
            if (uni > 1) {
                // Symmetric spread: -1..+1 across the stack (centre stays put).
                const i32 f_q16 = static_cast<i32>(
                    ((static_cast<i64>(2 * u) - (uni - 1)) << GAIN_SHIFT) / (uni - 1));
                v->unison_extra_cents_q16 = static_cast<i32>(
                    asr(static_cast<i64>(l.unison_detune_cents_q16) * f_q16, GAIN_SHIFT));
                v->unison_pan_q16 = static_cast<gain_t>(
                    asr(static_cast<i64>(l.unison_spread_q16) * f_q16, GAIN_SHIFT));
                // Start the stack's oscillators apart so it beats immediately
                // instead of only once the detune has drifted them apart.
                if (l.unison_phase_spread_q16 > 0)
                    v->osc.phase = static_cast<u32>(
                        (static_cast<u64>(0x9E3779B9u * static_cast<u32>(u + 1)) *
                         static_cast<u32>(l.unison_phase_spread_q16)) >> GAIN_SHIFT);
            }
            if (glide && l.fixed_pitch_cents_q16 < 0) {
                v->glide_offset_cents_q16 = glide_from - target_cents;
                v->glide_step_cents_q16 = static_cast<i32>(
                    v->glide_offset_cents_q16 / static_cast<i32>(ch.portamento_blocks));
                if (v->glide_step_cents_q16 == 0)
                    v->glide_step_cents_q16 = v->glide_offset_cents_q16 > 0 ? 1 : -1;
            }
        }
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
        case 5:
            // Portamento time: quadratic 0..127 -> 0..1000 ms, in blocks.
            ch.portamento_blocks = static_cast<u32>(
                (static_cast<i64>(val) * val * 1000 / 16129) * CONTROL_RATE / 1000);
            break;
        case 65: ch.portamento = val >= 64; break;
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
        case 94: ch.send_delay = val; break;  // delay/echo send
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
        case EngineCommand::Type::NoteOn:
            note_on(c.slot, c.d1, c.d2);
            sidechain_.on_note(c.slot, c.d1);  // the kick pulls the pads down
            break;
        case EngineCommand::Type::SidechainConfig:
            switch (c.d1) {
                case 0: sidechain_params_.source = clamp_i32(c.v, -1, NUM_CHANNEL_SLOTS - 1); break;
                case 1: sidechain_params_.note = clamp_i32(c.v, -1, 127); break;
                case 2: sidechain_params_.targets = static_cast<u32>(c.v); break;
                case 3: sidechain_params_.depth = clamp_gain01(c.v); break;
                case 4: sidechain_params_.attack_ms_q16 = clamp_i32(c.v, 0, 500 << GAIN_SHIFT); break;
                case 5: sidechain_params_.hold_ms_q16 = clamp_i32(c.v, 0, 500 << GAIN_SHIFT); break;
                case 6: sidechain_params_.release_ms_q16 = clamp_i32(c.v, 0, 2000 << GAIN_SHIFT); break;
                default: break;
            }
            sidechain_.set_params(sidechain_params_);
            break;
        case EngineCommand::Type::NoteOff:
            // d2 = release velocity (0 = "no information" -> neutral 64).
            pool_.note_off_group(c.slot, c.d1, channels_[c.slot].sustain,
                                 c.d2 ? c.d2 : 64);
            break;
        case EngineCommand::Type::PolyAftertouch:
            pool_.poly_aftertouch(c.slot, c.d1,
                                  (static_cast<i32>(c.d2) << GAIN_SHIFT) / 127);
            break;
        case EngineCommand::Type::TempoBpm:
            tempo_bpm_q16_ = clamp_i32(c.v, 20 << GAIN_SHIFT, 400 << GAIN_SHIFT);
            delay_fx_.set_tempo(tempo_bpm_q16_);  // a synced echo follows the song
            break;
        case EngineCommand::Type::TuningCent:
            if (c.d1 < 12) tuning_cents_q16_[c.d1] = clamp_i32(c.v, -(200 << GAIN_SHIFT), 200 << GAIN_SHIFT);
            break;
        case EngineCommand::Type::ChannelEqParam:
            if (c.slot < NUM_CHANNEL_SLOTS) chan_eq_[c.slot].set_param(c.d1, c.v);
            break;
        case EngineCommand::Type::DelayConfig:
            switch (c.d1) {
                case 0: delay_params_.time_ms_q16 = clamp_i32(c.v, GAIN_ONE, 640 << GAIN_SHIFT); break;
                case 1: delay_params_.feedback_q16 = clamp_i32(c.v, 0, (GAIN_ONE * 9) / 10); break;
                case 2: delay_params_.wet_gain = clamp_i32(c.v, 0, 2 * GAIN_ONE); break;
                case 3: delay_params_.ping_pong = c.v != 0; break;
                case 4: delay_params_.sync_beats_q16 = clamp_i32(c.v, 0, 64 << GAIN_SHIFT); break;
                default: break;
            }
            delay_fx_.set_params(delay_params_);
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
        case EngineCommand::Type::MasterParam:
            if (c.d1 < static_cast<u8>(MasterParamId::COUNT))
                master_.set_param(static_cast<MasterParamId>(c.d1), c.v);
            break;
        case EngineCommand::Type::ReverbConfig:
            switch (c.d1) {
                case 0: reverb_params_.room_size = clamp_gain01(c.v); break;
                case 1: reverb_params_.damping = clamp_gain01(c.v); break;
                case 2: reverb_params_.width = clamp_gain01(c.v); break;
                case 3: reverb_params_.wet_gain = clamp_i32(c.v, 0, 2 * GAIN_ONE); break;
                case 4: reverb_params_.pre_delay_ms_q16 = clamp_i32(c.v, 0, 200 << GAIN_SHIFT); break;
                case 5: reverb_params_.high_pass_hz_q16 = clamp_i32(c.v, 0, 1000 << GAIN_SHIFT); break;
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
        del_l_[i] = del_r_[i] = 0;
    }
    // Zero only the channel lanes that were written last block.
    for (int s = 0; s < NUM_CHANNEL_SLOTS; ++s) {
        if (!chan_dirty_[s]) continue;
        chan_dirty_[s] = false;
        for (int i = 0; i < CONTROL_BLOCK; ++i) chan_l_[s][i] = chan_r_[s][i] = 0;
    }

    for (int s = 0; s < NUM_CHANNEL_SLOTS; ++s) channels_[s].advance_fade_block();
    out_stage_.advance_duck_block();

    for (Voice& v : pool_) {
        if (!v.active) continue;
        const u8 s = v.chan_slot;
        chan_dirty_[s] = true;
        const VoiceBuses buses{chan_l_[s], chan_r_[s], rev_l_, rev_r_, cho_l_, cho_r_,
                               del_l_, del_r_, tempo_bpm_q16_, tuning_cents_q16_};
        v.render_block(channels_[s], buses);
    }
    // Channel strips: EQ active lanes, duck the sidechain targets, then fold
    // every dirty lane into dry.
    for (int s = 0; s < NUM_CHANNEL_SLOTS; ++s) {
        if (!chan_dirty_[s]) continue;
        if (chan_eq_[s].active()) chan_eq_[s].process_block(chan_l_[s], chan_r_[s], CONTROL_BLOCK);
        if (sidechain_.ducks(s)) {
            // Block-rate endpoints, per-sample ramp -- the house idiom, and
            // two divisions a block instead of sixty-four.
            gain_t g = sidechain_.gain_at(0);
            const gain_t dg = (sidechain_.gain_at(CONTROL_BLOCK) - g) / CONTROL_BLOCK;
            for (int i = 0; i < CONTROL_BLOCK; ++i, g += dg) {
                chan_l_[s][i] = asr(chan_l_[s][i] * g, GAIN_SHIFT);
                chan_r_[s][i] = asr(chan_r_[s][i] * g, GAIN_SHIFT);
            }
        }
        for (int i = 0; i < CONTROL_BLOCK; ++i) {
            dry_l_[i] += chan_l_[s][i];
            dry_r_[i] += chan_r_[s][i];
        }
    }
    sidechain_.advance(CONTROL_BLOCK);
    pool_.update_active_count();
    pool_diag_public_ = pool_.diag();

    // Send effects return into the dry accumulators, then master out.
    reverb_.process_block(rev_l_, rev_r_, dry_l_, dry_r_, CONTROL_BLOCK);
    chorus_.process_block(cho_l_, cho_r_, dry_l_, dry_r_, CONTROL_BLOCK);
    delay_fx_.process_block(del_l_, del_r_, dry_l_, dry_r_, CONTROL_BLOCK);

    if (!master_.enabled()) {
        // Bypass: byte-identical to the pre-mastering output path.
        for (int i = 0; i < CONTROL_BLOCK; ++i) {
            const i64 sl = out_stage_.pre_gain(dry_l_[i]);
            const i64 sr = out_stage_.pre_gain(dry_r_[i]);
            out[2 * i] = out_stage_.finish(sl, 0);
            out[2 * i + 1] = out_stage_.finish(sr, 1);
            master_.meter(sl, sr);
        }
    } else {
        constexpr i64 LIM = static_cast<i64>(32) << SAMPLE_SHIFT;
        for (int i = 0; i < CONTROL_BLOCK; ++i) {
            mst_l_[i] = clamp_i32(out_stage_.pre_gain(dry_l_[i]), -LIM, LIM);
            mst_r_[i] = clamp_i32(out_stage_.pre_gain(dry_r_[i]), -LIM, LIM);
        }
        master_.process_block(mst_l_, mst_r_, CONTROL_BLOCK);
        for (int i = 0; i < CONTROL_BLOCK; ++i) {
            out[2 * i] = out_stage_.finish(mst_l_[i], 0);
            out[2 * i + 1] = out_stage_.finish(mst_r_[i], 1);
            master_.meter(mst_l_[i], mst_r_[i]);
        }
    }
    sample_time_ += CONTROL_BLOCK;
}

void SynthEngine::render(i16* out_interleaved, int frames) noexcept {
    for (int done = 0; done + CONTROL_BLOCK <= frames; done += CONTROL_BLOCK)
        render_one_block(out_interleaved + 2 * done);
}

}  // namespace msynth
