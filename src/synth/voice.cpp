#include "voice.h"

namespace msynth {

namespace {
// ~4ms fast release (3 control blocks).
constexpr u32 FAST_RELEASE_BLOCKS = 3;
// Steal immunity for freshly started voices (~4ms).
constexpr u8 STEAL_SHIELD_BLOCKS = 3;
}  // namespace

void Voice::start(u8 pool_slot, u8 channel_slot, u8 midi_note, u8 vel, u32 group,
                  u8 layer_idx, const Patch* p, const LayerDef* l,
                  const ChannelState& ch, u64 blocks_elapsed, gain_t bpm_q16) noexcept {
    active = true;
    slot = pool_slot;
    chan_slot = channel_slot;
    note = midi_note;
    velocity = vel;
    group_id = group;
    layer_index = layer_idx;
    age_blocks = 0;
    steal_shield = STEAL_SHIELD_BLOCKS;
    released = false;
    pedal_held = false;
    patch = p;
    layer = l;

    base_cents_q16 = (l->fixed_pitch_cents_q16 >= 0 ? l->fixed_pitch_cents_q16
                                                    : (static_cast<i32>(midi_note) * 100) << GAIN_SHIFT)
                   + l->detune_cents_q16;
    switch (l->vel_curve) {
        case VelCurve::Linear: vel_gain = VEL_LINEAR_Q16[vel]; break;
        case VelCurve::Soft: vel_gain = VEL_SOFT_Q16[vel]; break;
        default: vel_gain = VEL_GAIN_Q16[vel]; break;
    }
    key_track_q16 = static_cast<i32>(asl(static_cast<i64>(midi_note) - 60, GAIN_SHIFT) / 64);

    // Deterministic seeds from stable inputs only -- output must not depend
    // on allocation history (plan §14).
    const u32 seed = static_cast<u32>(pool_slot) * 2654435761u ^ midi_note;
    noise = XorShift32(seed);
    noise_b0 = noise_b1 = noise_b2 = 0;
    for (int i = 0; i < 6; ++i) metal_phase[i] = 0;

    if (l->osc == OscType::Wavetable && l->wavetable) {
        // Mip from the maximum pitch this voice can reach: note + bend range
        // + upward pitch-envelope depth, with a small margin for matrix
        // vibrato (Appendix A: selected once at note-on).
        i32 max_cents = base_cents_q16 + (ch.bend_range_cents << GAIN_SHIFT) + (100 << GAIN_SHIFT);
        if (l->pitch_env_cents_q16 > 0) max_cents += l->pitch_env_cents_q16;
        osc.phase = 0;
        osc.set_table(*l->wavetable, mip_for_cents(max_cents));
        osc.set_frame_pos(0);
    }

    // Key-scaled envelope rate: durations x 2^(-ks * (note - 60) / 12),
    // per envelope, via the baked exp2 table (|ks| <= 1, note delta < 6
    // octaves -- comfortably inside exp2_q16's domain).
    const auto rate_scale = [&](gain_t ks) -> gain_t {
        if (ks == 0) return GAIN_ONE;
        const i32 arg = static_cast<i32>(-static_cast<i64>(ks) * (static_cast<i32>(midi_note) - 60) / 12);
        return exp2_q16(arg);
    };
    amp_env.note_on(l->amp_env, rate_scale(l->amp_env.key_scale_rate_q16));
    filt_env.note_on(l->filter_env, rate_scale(l->filter_env.key_scale_rate_q16));
    pitch_env.note_on(l->pitch_env, rate_scale(l->pitch_env.key_scale_rate_q16));
    wt_env.note_on(l->wt_env, rate_scale(l->wt_env.key_scale_rate_q16));
    for (int i = 0; i < l->lfo_count; ++i)
        lfo[i].note_on(l->lfos[i], seed ^ (0x9E3779B9u * (i + 1)), blocks_elapsed, bpm_q16);

    // Filter starts at the block-0 coefficients, not sweeping from stale state.
    if (l->filter_mode == FilterMode::Comb) {
        comb_line.clear();
        comb_delay_q16 = static_cast<u32>(
            clamp_i32((static_cast<i64>(SAMPLE_RATE) << 32) /
                          cutoff_hz_from_index(l->cutoff_index_q16),
                      2 << GAIN_SHIFT, (4094 << GAIN_SHIFT)));
        comb_delta_q16 = 0;
    } else if (l->filter_mode == FilterMode::Formant) {
        FilterCoefs c;
        const coef_t k = resonance_k(l->resonance_q16);
        c.compute(cutoff_g(l->formant_idx_q16[0][0]), k);
        filter.reset(c);
        c.compute(cutoff_g(l->formant_idx_q16[0][1]), k);
        filter2.reset(c);
        c.compute(cutoff_g(l->formant_idx_q16[0][2]), k);
        filter3.reset(c);
    } else if (l->filter_mode != FilterMode::Bypass) {
        FilterCoefs c;
        c.compute(cutoff_g(l->cutoff_index_q16), resonance_k(l->resonance_q16));
        filter.reset(c);
        if (l->filter_24db && l->filter2_mode == FilterMode::Bypass) filter2.reset(c);
    }
    if (l->filter2_mode != FilterMode::Bypass) {
        FilterCoefs c2;
        c2.compute(cutoff_g(l->filter2_cutoff_index_q16), resonance_k(l->filter2_resonance_q16));
        filter2.reset(c2);
    }

    cur_gl = cur_gr = 0;
    d_gl = d_gr = 0;
    send_rev = send_cho = send_del = 0;
    unison_extra_cents_q16 = 0;
    unison_pan_q16 = 0;
    glide_offset_cents_q16 = 0;
    glide_step_cents_q16 = 0;
    ring_phase = 0;
    poly_at_q16 = 0;
    delay_blocks = l->start_delay_blocks;
    pending_off_blocks = 0;
    pending_note_off = false;
    pending_pedal = false;
}

void Voice::note_off(bool pedal_down, u8 release_vel) noexcept {
    if (!active || released) return;
    if (patch && patch->one_shot) return;  // one-shot: note-off immune, keeps
                                           // normal (not released) steal class
    // Release velocity: harder lifts shorten the release, soft lifts stretch
    // it, scaled by the layer's releaseVel amount (0 = classic behavior).
    rel_scale_q16 = GAIN_ONE;
    if (layer && layer->release_vel_amount_q16 != 0 && release_vel != 64) {
        const i32 dev = (static_cast<i32>(64) - release_vel) << GAIN_SHIFT;  // +soft/-hard
        rel_scale_q16 = GAIN_ONE + static_cast<i32>(
            asr(static_cast<i64>(dev) / 64 * layer->release_vel_amount_q16, GAIN_SHIFT));
        rel_scale_q16 = clamp_i32(rel_scale_q16, GAIN_ONE / 8, 2 * GAIN_ONE);
    }
    if (delay_blocks > 0) {
        // The layer hasn't spoken yet: remember the note-off and apply it the
        // moment it starts, so a flam layer still gets its attack + release.
        pending_note_off = true;
        pending_pedal = pedal_down;
        return;
    }
    if (pedal_down) {
        pedal_held = true;  // defer: the envelope never sees the pedal
        return;
    }
    released = true;
    amp_env.note_off(rel_scale_q16);
    filt_env.note_off(rel_scale_q16);
    pitch_env.note_off(rel_scale_q16);
    wt_env.note_off(rel_scale_q16);
}

void Voice::release_pedal() noexcept {
    if (!active || !pedal_held) return;
    pedal_held = false;
    released = true;
    amp_env.note_off();
    filt_env.note_off();
    pitch_env.note_off();
    wt_env.note_off();
}

void Voice::fast_release() noexcept {
    if (!active) return;
    released = true;
    pedal_held = false;
    delay_blocks = 0;  // a killed flam never speaks
    pending_note_off = false;
    amp_env.fast_release(FAST_RELEASE_BLOCKS);
}

bool Voice::render_block(const ChannelState& ch, const VoiceBuses& buses) noexcept {
    if (!active) return false;

    // startDelayMs: hold everything (envelopes, LFOs, oscillator) and emit
    // silence until the delay elapses -- the layer then starts as if the
    // note-on had just happened.
    if (delay_blocks > 0) {
        --delay_blocks;
        ++age_blocks;
        if (steal_shield > 0) --steal_shield;
        if (delay_blocks == 0 && pending_note_off) {
            // Let the layer play its attack before the deferred note-off:
            // releasing at level 0 would silence the flam entirely.
            pending_note_off = false;
            // +2: the envelope must ADVANCE through the attack before the
            // note-off, or a 0-block attack would release from level 0.
            pending_off_blocks = static_cast<i32>(layer->amp_env.att.duration_blocks) + 2;
        }
        return true;
    }
    if (pending_off_blocks > 0 && --pending_off_blocks == 0) note_off(pending_pedal);

    // Glide: the pitch offset decays linearly to zero over the portamento time.
    if (glide_offset_cents_q16 != 0) {
        if ((glide_offset_cents_q16 > 0) == (glide_step_cents_q16 > 0) &&
            (glide_offset_cents_q16 > 0 ? glide_offset_cents_q16 <= glide_step_cents_q16
                                        : glide_offset_cents_q16 >= glide_step_cents_q16))
            glide_offset_cents_q16 = 0;
        else glide_offset_cents_q16 -= glide_step_cents_q16;
    }

    // ---- Control rate --------------------------------------------------
    const gain_t a = amp_env.advance_block();
    if (amp_env.idle()) {  // Rule 4: only the amp envelope frees the voice
        active = false;
        return false;
    }
    const gain_t fe = filt_env.advance_block();
    const gain_t pe = pitch_env.advance_block();
    const gain_t we = wt_env.advance_block();

    const LayerDef& l = *layer;
    mod.src[static_cast<int>(ModSource::AmpEnv)] = a;
    mod.src[static_cast<int>(ModSource::FilterEnv)] = fe;
    mod.src[static_cast<int>(ModSource::PitchEnv)] = pe;
    mod.src[static_cast<int>(ModSource::WtPosEnv)] = we;
    mod.src[static_cast<int>(ModSource::Velocity)] = vel_gain;
    mod.src[static_cast<int>(ModSource::KeyTrack)] = key_track_q16;
    mod.src[static_cast<int>(ModSource::ModWheel)] = ch.modwheel_q16;
    mod.src[static_cast<int>(ModSource::ChannelAftertouch)] = ch.aftertouch_q16;
    mod.src[static_cast<int>(ModSource::PolyAftertouch)] = poly_at_q16;
    for (int i = 0; i < l.lfo_count; ++i)
        mod.src[static_cast<int>(ModSource::LFO1) + i] =
            lfo[i].advance_block(l.lfos[i], buses.tempo_bpm_q16);
    mod.evaluate(l.routings, l.routing_count);

    // Pitch: base + channel bend + pitch envelope (implicit depth) + matrix,
    // plus unison detune, glide, and the 12-tone microtuning table.
    i64 cents = static_cast<i64>(base_cents_q16) + ch.bend_cents_q16();
    cents += unison_extra_cents_q16;
    cents += glide_offset_cents_q16;
    if (buses.tuning_cents) cents += buses.tuning_cents[note % 12];
    cents += asr(static_cast<i64>(pe) * l.pitch_env_cents_q16, GAIN_SHIFT);
    cents += mod.dst[static_cast<int>(ModDest::Pitch)] * 1200;  // amount 1.0 = one octave
    osc.phase_inc = phase_inc_for_cents(clamp_i32(cents, 0, 0x7FFFFFFF));

    // Wavetable morph position.
    if (l.osc == OscType::Wavetable) {
        const i32 range = (osc.frame_count - 1) << GAIN_SHIFT;
        i64 pos = asr(static_cast<i64>(we) * l.wt_env_depth, GAIN_SHIFT);
        pos += mod.dst[static_cast<int>(ModDest::WtPosition)];
        osc.set_frame_target(static_cast<i32>(asr(pos * range, GAIN_SHIFT)));
    }

    // Filter coefficients for this block.
    i64 mod_idx = 0;
    if (l.filter_mode != FilterMode::Bypass || l.filter2_mode != FilterMode::Bypass) {
        mod_idx = ch.cutoff_offset_q16;
        mod_idx += asr(static_cast<i64>(fe) * l.filter_env_depth, GAIN_SHIFT) * CUTOFF_STEPS;
        mod_idx += mod.dst[static_cast<int>(ModDest::FilterCutoff)] * CUTOFF_STEPS;
        // Key tracking: follow pitch away from middle C.
        if (l.key_track_filter != 0) {
            const i64 oct_off = (cents - ((60 * 100) << GAIN_SHIFT)) / 1200;  // Q16 octaves
            const i64 steps = asr(oct_off * cutoff_steps_per_octave_q16(), GAIN_SHIFT);
            mod_idx += asr(steps * l.key_track_filter, GAIN_SHIFT);
        }
    }
    gain_t comb_fb = 0;
    gain_t formant_morph = 0;
    if (l.filter_mode == FilterMode::Comb) {
        // The comb is a pitched resonator: cutoff (plus every cutoff
        // modulation) sets its frequency, resonance sets the feedback.
        const i64 idx = clamp_i32(l.cutoff_index_q16 + mod_idx, 0,
                                  static_cast<i64>(CUTOFF_STEPS) << GAIN_SHIFT);
        const u32 target = static_cast<u32>(
            clamp_i32((static_cast<i64>(SAMPLE_RATE) << 32) /
                          cutoff_hz_from_index(static_cast<i32>(idx)),
                      2 << GAIN_SHIFT, (4094 << GAIN_SHIFT)));
        comb_delta_q16 = static_cast<i32>(
            asr(static_cast<i64>(target) - static_cast<i64>(comb_delay_q16), 6));
        i64 res = static_cast<i64>(l.resonance_q16) + ch.res_offset_q16 +
                  mod.dst[static_cast<int>(ModDest::FilterRes)];
        comb_fb = clamp_i32(res, 0, (95 * GAIN_ONE) / 100);
    } else if (l.filter_mode == FilterMode::Formant) {
        // The filter envelope morphs vowel -> endVowel; cutoff modulations
        // shift every formant together (a size/gender scale).
        formant_morph = clamp_gain01(static_cast<i64>(fe));
        const coef_t k = resonance_k(clamp_gain01(
            static_cast<i64>(l.resonance_q16) + ch.res_offset_q16 +
            mod.dst[static_cast<int>(ModDest::FilterRes)]));
        FilterState* fs[3] = {&filter, &filter2, &filter3};
        for (int b = 0; b < 3; ++b) {
            const i32 i0 = l.formant_idx_q16[0][b], i1 = l.formant_idx_q16[1][b];
            i64 idx = i0 + asr(static_cast<i64>(i1 - i0) * formant_morph, GAIN_SHIFT) + mod_idx;
            FilterCoefs c;
            c.compute(cutoff_g(clamp_i32(idx, 0, static_cast<i64>(CUTOFF_STEPS) << GAIN_SHIFT)), k);
            fs[b]->set_block_target(c, 6);
        }
    } else if (l.filter_mode != FilterMode::Bypass) {
        const i64 idx = l.cutoff_index_q16 + mod_idx;
        i64 res = static_cast<i64>(l.resonance_q16) + ch.res_offset_q16 +
                  mod.dst[static_cast<int>(ModDest::FilterRes)];
        FilterCoefs c;
        c.compute(cutoff_g(clamp_i32(idx, 0, static_cast<i64>(CUTOFF_STEPS) << GAIN_SHIFT)),
                  resonance_k(clamp_gain01(res)));
        filter.set_block_target(c, 6);  // 2^6 = CONTROL_BLOCK
        if (l.filter_24db && l.filter2_mode == FilterMode::Bypass) filter2.set_block_target(c, 6);
    }
    if (l.filter2_mode != FilterMode::Bypass) {
        // Independent second filter. By default it follows the same
        // modulation offsets (env, matrix, CC74, key tracking) so sweeps
        // move both; a `static` filter2 is a fixed corner -- a cabinet
        // low-cut does not ride the filter envelope.
        const i64 idx2 = l.filter2_cutoff_index_q16 + (l.filter2_static ? 0 : mod_idx);
        const i64 res2 = static_cast<i64>(l.filter2_resonance_q16) +
                         (l.filter2_static ? 0 : ch.res_offset_q16);
        FilterCoefs c2;
        c2.compute(cutoff_g(clamp_i32(idx2, 0, static_cast<i64>(CUTOFF_STEPS) << GAIN_SHIFT)),
                   resonance_k(clamp_gain01(res2)));
        filter2.set_block_target(c2, 6);
    }
    // Drive is routable: amount 1.0 = the full +7x of the 1..8 range.
    gain_t drive_now = l.filter_drive_q16;
    {
        const i64 dmod = mod.dst[static_cast<int>(ModDest::FilterDrive)];
        if (dmod != 0)
            drive_now = clamp_i32(static_cast<i64>(drive_now) + dmod * 7, GAIN_ONE, 8 * GAIN_ONE);
    }
    // ...and so is the post-filter stage (the "power amp" responds to touch).
    gain_t post_now = l.post_drive_q16;
    {
        const i64 pmod = mod.dst[static_cast<int>(ModDest::PostDrive)];
        if (pmod != 0)
            post_now = clamp_i32(static_cast<i64>(post_now) + pmod * 7, GAIN_ONE, 8 * GAIN_ONE);
    }

    // Amplitude: env * velocity * layer mix * channel gain * matrix Amplitude,
    // carried in Q8.24 from the envelope's full-resolution level so quiet
    // exponential tails keep their relative precision all the way down.
    i64 g24 = amp_env.level;  // Q8.24
    g24 = asr(g24 * vel_gain, GAIN_SHIFT);
    g24 = asr(g24 * l.mix_gain, GAIN_SHIFT);
    g24 = asr(g24 * ch.channel_gain_q16(), GAIN_SHIFT);
    const gain_t amp_mod = clamp_gain01(GAIN_ONE + mod.dst[static_cast<int>(ModDest::Amplitude)]);
    g24 = asr(g24 * amp_mod, GAIN_SHIFT);

    i64 pan = static_cast<i64>(l.pan_q16) + ch.pan_q16 + unison_pan_q16 +
              mod.dst[static_cast<int>(ModDest::Pan)];
    gain_t pl, pr;
    pan_gains(clamp_i32(pan, -GAIN_ONE, GAIN_ONE), pl, pr);
    const i32 tgt_gl = static_cast<i32>(asr(g24 * pl, GAIN_SHIFT));
    const i32 tgt_gr = static_cast<i32>(asr(g24 * pr, GAIN_SHIFT));
    d_gl = static_cast<i32>(asr(static_cast<i64>(tgt_gl) - cur_gl, 6));
    d_gr = static_cast<i32>(asr(static_cast<i64>(tgt_gr) - cur_gr, 6));

    send_rev = l.send_reverb_q16 >= 0
                   ? l.send_reverb_q16
                   : static_cast<gain_t>((static_cast<i32>(ch.send_reverb) << GAIN_SHIFT) / 127);
    send_cho = l.send_chorus_q16 >= 0
                   ? l.send_chorus_q16
                   : static_cast<gain_t>((static_cast<i32>(ch.send_chorus) << GAIN_SHIFT) / 127);
    send_del = l.send_delay_q16 >= 0
                   ? l.send_delay_q16
                   : static_cast<gain_t>((static_cast<i32>(ch.send_delay) << GAIN_SHIFT) / 127);

    // ---- Audio rate ----------------------------------------------------
    const bool use_comb = l.filter_mode == FilterMode::Comb;
    const bool use_formant = l.filter_mode == FilterMode::Formant;
    const bool use_filter = l.filter_mode != FilterMode::Bypass && !use_comb && !use_formant;
    const bool use_filter2 = l.filter2_mode != FilterMode::Bypass;
    const bool use_drive = (use_filter || use_filter2 || use_comb || use_formant) &&
                           drive_now != GAIN_ONE;
    const bool use_24db = use_filter && !use_filter2 && l.filter_24db;
    const bool parallel = use_filter && use_filter2 && l.filter_parallel;
    // Ring depth / FM index, modulatable: 1.0 of matrix amount = full ring,
    // or the whole 12-radian index range in fm mode.
    gain_t ring_now = l.ring_amount_q16;
    {
        const i64 rmod = mod.dst[static_cast<int>(ModDest::RingAmount)];
        if (rmod != 0 && l.ring_ratio_q16 != 0) {
            const i64 span = l.ring_is_fm ? 12 : 1;
            ring_now = clamp_i32(static_cast<i64>(ring_now) + rmod * span,
                                 0, l.ring_is_fm ? 12 * GAIN_ONE : GAIN_ONE);
        }
    }
    const bool use_fm = l.ring_is_fm && ring_now != 0 && l.ring_ratio_q16 != 0 &&
                        l.osc == OscType::Wavetable;
    const bool use_ring = !l.ring_is_fm && ring_now != 0 && l.ring_ratio_q16 != 0;
    const bool use_post = post_now != GAIN_ONE;
    const bool is_noise = l.osc == OscType::Noise;
    const bool is_metal = l.osc == OscType::Metal;
    // TR-808 cymbal cluster: six squares at inharmonic ratios of the pitch.
    u32 metal_inc[6];
    if (is_metal) {
        static constexpr u32 METAL_RATIO_Q16[6] = {65536, 87972, 80686, 108348, 127946, 141054};
        for (int i = 0; i < 6; ++i)
            metal_inc[i] = static_cast<u32>(
                (static_cast<u64>(osc.phase_inc) * METAL_RATIO_Q16[i]) >> GAIN_SHIFT);
    }
    const bool has_rev = send_rev != 0;
    const bool has_cho = send_cho != 0;
    const bool has_del = send_del != 0;
    const u32 ring_inc = (use_ring || use_fm) ? static_cast<u32>(
        (static_cast<u64>(osc.phase_inc) * static_cast<u32>(l.ring_ratio_q16)) >> GAIN_SHIFT) : 0;

    for (int i = 0; i < CONTROL_BLOCK; ++i) {
        i32 s_q30;
        if (is_metal) {
            i32 acc = 0;
            for (int k = 0; k < 6; ++k) {
                metal_phase[k] += metal_inc[k];
                acc += metal_phase[k] < 0x80000000u ? 1 : -1;
            }
            s_q30 = acc * (COEF_ONE / 6);
        } else {
            u32 phase_mod = 0;
            if (use_fm) {
                // Phase modulation by the internal carrier. index is Q16
                // radians; 1 radian = 2^32/(2*pi) phase units, and 10430/2^16
                // is that 1/(2*pi) after the Q32 product.
                ring_phase += ring_inc;
                const i32 sin_q16 = static_cast<i32>(asr(sine_q30(ring_phase), COEF_SHIFT - GAIN_SHIFT));
                const i64 rad_q32 = static_cast<i64>(sin_q16) * ring_now;
                phase_mod = static_cast<u32>((rad_q32 * 10430) >> GAIN_SHIFT);
            }
            s_q30 = is_noise ? asl(coloured_noise(l.noise_color), 6) : osc.next_q30(phase_mod);
        }
        if (use_ring) {
            // mix = (1 - amt) + amt * carrier: amt 1.0 = classic full ring.
            ring_phase += ring_inc;
            const gain_t mix = GAIN_ONE - ring_now +
                static_cast<gain_t>(asr(static_cast<i64>(ring_now) * sine_q30(ring_phase), COEF_SHIFT));
            s_q30 = static_cast<i32>(asr(static_cast<i64>(s_q30) * mix, GAIN_SHIFT));
        }
        if (use_comb) {
            if (use_drive) s_q30 = drive_q30(s_q30, drive_now);
            // Feedback comb tuned to the (modulated) cutoff frequency:
            // y = x + fb * y[n - d]; peaks at harmonics of 1/d.
            comb_delay_q16 = static_cast<u32>(
                clamp_i32(static_cast<i64>(comb_delay_q16) + comb_delta_q16,
                          2 << GAIN_SHIFT, (4094 << GAIN_SHIFT)));
            const i32 tap = comb_line.read_lerp(comb_delay_q16);
            i64 y = static_cast<i64>(s_q30) + asr(static_cast<i64>(tap) * comb_fb, GAIN_SHIFT);
            s_q30 = filt_sat(y);
            comb_line.push(s_q30);
        } else if (use_formant) {
            if (use_drive) s_q30 = drive_q30(s_q30, drive_now);
            // Three parallel unity-peak bandpasses (k*v1), F1 strongest.
            const i64 b1 = static_cast<i64>(filter.k) * filter.process(s_q30, FilterMode::Bandpass);
            const i64 b2 = static_cast<i64>(filter2.k) * filter2.process(s_q30, FilterMode::Bandpass);
            const i64 b3 = static_cast<i64>(filter3.k) * filter3.process(s_q30, FilterMode::Bandpass);
            s_q30 = filt_sat(asr(b1, COEF_SHIFT) + asr(b2, COEF_SHIFT + 1) + asr(b3, COEF_SHIFT + 2));
        } else if (use_filter || use_filter2) {
            if (use_drive) s_q30 = drive_q30(s_q30, drive_now);
            if (parallel) {
                const i32 a = filter.process(s_q30, l.filter_mode);
                const i32 b = filter2.process(s_q30, l.filter2_mode);
                s_q30 = static_cast<i32>(asr(static_cast<i64>(a) + b, 1));
            } else {
                if (use_filter) s_q30 = filter.process(s_q30, l.filter_mode);
                if (use_filter2) s_q30 = filter2.process(s_q30, l.filter2_mode);
                else if (use_24db) s_q30 = filter2.process(s_q30, l.filter_mode);
            }
        }
        // Post-filter saturation: deliberately NOT band-limited (the point is
        // distorting the filtered signal at play time).
        if (use_post) s_q30 = drive_q30(s_q30, post_now);
        cur_gl += d_gl;
        cur_gr += d_gr;
        // Q2.30 * Q8.24 >> (24+6) -> Q8.24
        const i64 out_l = asr(static_cast<i64>(s_q30) * cur_gl, SAMPLE_SHIFT + 6);
        const i64 out_r = asr(static_cast<i64>(s_q30) * cur_gr, SAMPLE_SHIFT + 6);
        buses.dry_l[i] += out_l;
        buses.dry_r[i] += out_r;
        if (has_rev) {
            buses.rev_l[i] += asr(out_l * send_rev, GAIN_SHIFT);
            buses.rev_r[i] += asr(out_r * send_rev, GAIN_SHIFT);
        }
        if (has_cho) {
            buses.cho_l[i] += asr(out_l * send_cho, GAIN_SHIFT);
            buses.cho_r[i] += asr(out_r * send_cho, GAIN_SHIFT);
        }
        if (has_del) {
            buses.del_l[i] += asr(out_l * send_del, GAIN_SHIFT);
            buses.del_r[i] += asr(out_r * send_del, GAIN_SHIFT);
        }
    }

    ++age_blocks;
    if (steal_shield > 0) --steal_shield;
    return true;
}

}  // namespace msynth
