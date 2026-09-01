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
                  const ChannelState& ch) noexcept {
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

    amp_env.note_on(l->amp_env);
    filt_env.note_on(l->filter_env);
    pitch_env.note_on(l->pitch_env);
    wt_env.note_on(l->wt_env);
    for (int i = 0; i < l->lfo_count; ++i) lfo[i].note_on(l->lfos[i], seed ^ (0x9E3779B9u * (i + 1)));

    // Filter starts at the block-0 coefficients, not sweeping from stale state.
    if (l->filter_mode != FilterMode::Bypass) {
        FilterCoefs c;
        c.compute(cutoff_g(l->cutoff_index_q16), resonance_k(l->resonance_q16));
        filter.reset(c);
    }

    cur_gl = cur_gr = 0;
    d_gl = d_gr = 0;
    send_rev = send_cho = 0;
}

void Voice::note_off(bool pedal_down) noexcept {
    if (!active || released) return;
    if (patch && patch->one_shot) return;  // one-shot: note-off immune, keeps
                                           // normal (not released) steal class
    if (pedal_down) {
        pedal_held = true;  // defer: the envelope never sees the pedal
        return;
    }
    released = true;
    amp_env.note_off();
    filt_env.note_off();
    pitch_env.note_off();
    wt_env.note_off();
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
    amp_env.fast_release(FAST_RELEASE_BLOCKS);
}

bool Voice::render_block(const ChannelState& ch, const VoiceBuses& buses) noexcept {
    if (!active) return false;

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
    for (int i = 0; i < l.lfo_count; ++i)
        mod.src[static_cast<int>(ModSource::LFO1) + i] = lfo[i].advance_block(l.lfos[i]);
    mod.evaluate(l.routings, l.routing_count);

    // Pitch: base + channel bend + pitch envelope (implicit depth) + matrix.
    i64 cents = static_cast<i64>(base_cents_q16) + ch.bend_cents_q16();
    cents += asr(static_cast<i64>(pe) * l.pitch_env_cents_q16, GAIN_SHIFT);
    cents += mod.dst[static_cast<int>(ModDest::Pitch)] * 1200;  // amount 1.0 = one octave
    osc.phase_inc = phase_inc_for_cents(clamp_i32(cents, 0, 0x7FFFFFFF));

    // Wavetable morph position.
    if (l.osc == OscType::Wavetable) {
        const i32 range = (osc.frame_count - 1) << GAIN_SHIFT;
        i64 pos = asr(static_cast<i64>(we) * l.wt_env_depth, GAIN_SHIFT);
        pos += mod.dst[static_cast<int>(ModDest::WtPosition)];
        osc.set_frame_pos(static_cast<i32>(asr(pos * range, GAIN_SHIFT)));
    }

    // Filter coefficients for this block.
    if (l.filter_mode != FilterMode::Bypass) {
        i64 idx = l.cutoff_index_q16 + ch.cutoff_offset_q16;
        idx += asr(static_cast<i64>(fe) * l.filter_env_depth, GAIN_SHIFT) * CUTOFF_STEPS;
        idx += mod.dst[static_cast<int>(ModDest::FilterCutoff)] * CUTOFF_STEPS;
        // Key tracking: follow pitch away from middle C.
        if (l.key_track_filter != 0) {
            const i64 oct_off = (cents - ((60 * 100) << GAIN_SHIFT)) / 1200;  // Q16 octaves
            const i64 steps = asr(oct_off * cutoff_steps_per_octave_q16(), GAIN_SHIFT);
            idx += asr(steps * l.key_track_filter, GAIN_SHIFT);
        }
        i64 res = static_cast<i64>(l.resonance_q16) + ch.res_offset_q16 +
                  mod.dst[static_cast<int>(ModDest::FilterRes)];
        FilterCoefs c;
        c.compute(cutoff_g(clamp_i32(idx, 0, static_cast<i64>(CUTOFF_STEPS) << GAIN_SHIFT)),
                  resonance_k(clamp_gain01(res)));
        filter.set_block_target(c, 6);  // 2^6 = CONTROL_BLOCK
    }

    // Amplitude: env * velocity * layer mix * channel gain * matrix Amplitude.
    gain_t g = gmul(a, vel_gain);
    g = gmul(g, l.mix_gain);
    g = gmul(g, ch.channel_gain_q16());
    const gain_t amp_mod = clamp_gain01(GAIN_ONE + mod.dst[static_cast<int>(ModDest::Amplitude)]);
    g = gmul(g, amp_mod);

    i64 pan = static_cast<i64>(l.pan_q16) + ch.pan_q16 + mod.dst[static_cast<int>(ModDest::Pan)];
    gain_t pl, pr;
    pan_gains(clamp_i32(pan, -GAIN_ONE, GAIN_ONE), pl, pr);
    const gain_t tgt_gl = gmul(g, pl);
    const gain_t tgt_gr = gmul(g, pr);
    d_gl = asr(static_cast<i64>(tgt_gl) - cur_gl, 6);
    d_gr = asr(static_cast<i64>(tgt_gr) - cur_gr, 6);

    send_rev = static_cast<gain_t>((static_cast<i32>(ch.send_reverb) << GAIN_SHIFT) / 127);
    send_cho = static_cast<gain_t>((static_cast<i32>(ch.send_chorus) << GAIN_SHIFT) / 127);

    // ---- Audio rate ----------------------------------------------------
    const bool use_filter = l.filter_mode != FilterMode::Bypass;
    const bool is_noise = l.osc == OscType::Noise;
    const bool has_rev = send_rev != 0;
    const bool has_cho = send_cho != 0;

    for (int i = 0; i < CONTROL_BLOCK; ++i) {
        i32 s_q30 = is_noise ? static_cast<i32>(asl(noise.next_sample(), 6)) : osc.next_q30();
        if (use_filter) s_q30 = filter.process(s_q30, l.filter_mode);
        cur_gl += d_gl;
        cur_gr += d_gr;
        // Q2.30 * Q16.16 >> (16+6) -> Q8.24
        const i64 out_l = asr(static_cast<i64>(s_q30) * cur_gl, GAIN_SHIFT + 6);
        const i64 out_r = asr(static_cast<i64>(s_q30) * cur_gr, GAIN_SHIFT + 6);
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
    }

    ++age_blocks;
    if (steal_shield > 0) --steal_shield;
    return true;
}

}  // namespace msynth
