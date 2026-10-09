#pragma once
//
// ADSR state machine (plan Appendix C). Uniform stage records; termination is
// structural (phase reaches ONE), never asymptotic. Advances once per control
// block; audio-rate smoothing of the output is the consumer's job.
//
#include "tables.h"

namespace msynth {

inline constexpr u32 ENV_PHASE_ONE = 1u << 24;
// Envelope levels are Q8.24 internally: exponential tails keep ~8 bits of
// relative precision down at -96 dB instead of quantising into audible
// stairs (Q16.16 leaves a -60 dB tail only ~65 distinct values).
inline constexpr i32 ENV_ONE = 1 << 24;

struct StageParams {
    u32 duration_blocks = 0;  // 0 = instant jump
    CurveId curve = CurveId::Linear;
};

struct EnvParams {
    StageParams del{0, CurveId::Linear};  // pre-attack delay (holds level)
    StageParams att{0, CurveId::Linear};
    StageParams hold{0, CurveId::Linear};  // full-level hold before decay
    StageParams dec{0, CurveId::Exp};
    StageParams rel{0, CurveId::Exp};
    gain_t sustain = GAIN_ONE;  // Q16.16 [0,1]
    // Key-scaled rate [-1, 1] Q16: stage durations are multiplied by
    // 2^(-ks * (note - 60) / 12) at note-on. +1 = envelopes run twice as
    // fast per octave above middle C (piano-like); negative flips it.
    gain_t key_scale_rate_q16 = 0;
    bool one_shot = false;      // Attack -> Decay -> Idle, note-off ignored
    bool loop = false;          // Decay end -> back to Attack while held
};

struct EnvelopeState {
    enum class Stage : u8 { Idle, Delay, Attack, Hold, Decay, Sustain, Release };

    Stage stage = Stage::Idle;
    i32 start_level = 0;  // Q8.24
    i32 level = 0;        // Q8.24
    i32 target = 0;       // Q8.24
    u32 phase = 0;
    u32 step = 0;
    const gain_t* curve = nullptr;
    const EnvParams* p = nullptr;
    // Per-note duration multiplier (key-scaled rate), set at note_on and
    // applied to every timed stage entered afterwards. Q16.16; 1.0 = as
    // authored. fast_release() bypasses it (stealing stays bounded).
    gain_t dur_scale_q16 = GAIN_ONE;

    u32 scaled_blocks(u32 duration_blocks) const noexcept {
        if (dur_scale_q16 == GAIN_ONE || duration_blocks == 0) return duration_blocks;
        const u32 d = static_cast<u32>(
            asr(static_cast<i64>(duration_blocks) * dur_scale_q16, GAIN_SHIFT));
        // A timed stage never collapses to instant from scaling alone.
        return d == 0 ? 1 : d;
    }

    static u32 step_for(u32 duration_blocks) noexcept {
        // duration 0 -> full phase in one add (instant, falls through).
        return duration_blocks == 0 ? ENV_PHASE_ONE : ENV_PHASE_ONE / duration_blocks;
    }

    // Q16 view for the mod matrix / diagnostics (landmark values are exact:
    // 1.0 and the sustain level convert without rounding).
    gain_t level_q16() const noexcept { return static_cast<gain_t>(asr(level, 8)); }

    void enter(Stage s, i32 tgt, const StageParams& sp) noexcept {
        stage = s;
        start_level = level;
        target = tgt;
        phase = 0;
        step = step_for(scaled_blocks(sp.duration_blocks));
        curve = curve_table(sp.curve);
    }

    // Rule 3: every stage starts from the current level -- retrigger of a
    // ringing voice is the normal entry path, no special case.
    // rate_scale_q16 multiplies every stage duration for this note (key
    // scaling; 1.0 = as authored).
    void note_on(const EnvParams& params, gain_t rate_scale_q16 = GAIN_ONE) noexcept {
        p = &params;
        dur_scale_q16 = rate_scale_q16;
        if (params.del.duration_blocks > 0) enter(Stage::Delay, level, params.del);
        else enter(Stage::Attack, ENV_ONE, params.att);
    }

    // Rule 7: the envelope never sees the sustain pedal; deferral of the
    // note-off event happens at the channel layer.
    // rel_scale_q16 scales the release duration (release velocity; 1.0 = as
    // authored, the historical default).
    void note_off(gain_t rel_scale_q16 = GAIN_ONE) noexcept {
        if (stage == Stage::Idle || !p) return;
        if (p->one_shot) return;  // note-off immunity
        StageParams rel = p->rel;
        if (rel_scale_q16 != GAIN_ONE)
            rel.duration_blocks = static_cast<u32>(
                asr(static_cast<i64>(rel.duration_blocks) * rel_scale_q16, GAIN_SHIFT));
        enter(Stage::Release, 0, rel);
    }

    // Rule 6: stealing, choke, All Sound Off, crossfade-out -- all this one
    // path with a forced short duration.
    void fast_release(u32 blocks) noexcept {
        if (stage == Stage::Idle) return;
        stage = Stage::Release;
        start_level = level;
        target = 0;
        phase = 0;
        step = step_for(blocks);
        curve = curve_table(CurveId::Linear);
    }

    void kill() noexcept {
        stage = Stage::Idle;
        level = 0;
    }

    bool idle() const noexcept { return stage == Stage::Idle; }

    // One control block; returns the current level as Q16.16 (read
    // `level` / `level_q16()` for the full Q8.24 resolution).
    gain_t advance_block() noexcept {
        for (;;) {
            switch (stage) {
                case Stage::Idle: return 0;
                case Stage::Sustain: return level_q16();
                default: break;
            }
            const u64 next_phase = static_cast<u64>(phase) + step;
            if (next_phase < ENV_PHASE_ONE) {
                phase = static_cast<u32>(next_phase);
                const gain_t c = curve_eval(curve, phase);
                level = start_level + static_cast<i32>(
                    asr(static_cast<i64>(target - start_level) * c, GAIN_SHIFT));
                return level_q16();
            }
            // Stage complete: land exactly on target, chain to the next stage.
            level = target;
            switch (stage) {
                case Stage::Delay:
                    enter(Stage::Attack, ENV_ONE, p->att);
                    break;
                case Stage::Attack:
                    if (p->hold.duration_blocks > 0) enter(Stage::Hold, ENV_ONE, p->hold);
                    else enter(Stage::Decay, p->one_shot ? 0 : asl(p->sustain, 8), p->dec);
                    break;
                case Stage::Hold:
                    enter(Stage::Decay, p->one_shot ? 0 : asl(p->sustain, 8), p->dec);
                    break;
                case Stage::Decay:
                    if (p->one_shot) { kill(); return 0; }
                    if (p->loop) {  // A-D cycle while held (note-off still releases)
                        enter(Stage::Attack, ENV_ONE, p->att);
                        break;
                    }
                    stage = Stage::Sustain;
                    level = asl(p->sustain, 8);
                    return level_q16();
                case Stage::Release:
                    kill();
                    return 0;
                default:
                    kill();
                    return 0;
            }
            // Rule 1: a zero-duration next stage falls through within this
            // same block (the loop); a timed stage waits for the next block.
            if (step < ENV_PHASE_ONE) return level_q16();
        }
    }
};

}  // namespace msynth
