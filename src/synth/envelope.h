#pragma once
//
// ADSR state machine (plan Appendix C). Uniform stage records; termination is
// structural (phase reaches ONE), never asymptotic. Advances once per control
// block; audio-rate smoothing of the output is the consumer's job.
//
#include "tables.h"

namespace msynth {

inline constexpr u32 ENV_PHASE_ONE = 1u << 24;

struct StageParams {
    u32 duration_blocks = 0;  // 0 = instant jump
    CurveId curve = CurveId::Linear;
};

struct EnvParams {
    StageParams att{0, CurveId::Linear};
    StageParams dec{0, CurveId::Exp};
    StageParams rel{0, CurveId::Exp};
    gain_t sustain = GAIN_ONE;  // Q16.16 [0,1]
    bool one_shot = false;      // Attack -> Decay -> Idle, note-off ignored
};

struct EnvelopeState {
    enum class Stage : u8 { Idle, Attack, Decay, Sustain, Release };

    Stage stage = Stage::Idle;
    gain_t start_level = 0;
    gain_t level = 0;
    gain_t target = 0;
    u32 phase = 0;
    u32 step = 0;
    const gain_t* curve = nullptr;
    const EnvParams* p = nullptr;

    static u32 step_for(u32 duration_blocks) noexcept {
        // duration 0 -> full phase in one add (instant, falls through).
        return duration_blocks == 0 ? ENV_PHASE_ONE : ENV_PHASE_ONE / duration_blocks;
    }

    void enter(Stage s, gain_t tgt, const StageParams& sp) noexcept {
        stage = s;
        start_level = level;
        target = tgt;
        phase = 0;
        step = step_for(sp.duration_blocks);
        curve = curve_table(sp.curve);
    }

    // Rule 3: every stage starts from the current level -- retrigger of a
    // ringing voice is the normal entry path, no special case.
    void note_on(const EnvParams& params) noexcept {
        p = &params;
        enter(Stage::Attack, GAIN_ONE, params.att);
    }

    // Rule 7: the envelope never sees the sustain pedal; deferral of the
    // note-off event happens at the channel layer.
    void note_off() noexcept {
        if (stage == Stage::Idle || !p) return;
        if (p->one_shot) return;  // note-off immunity
        enter(Stage::Release, 0, p->rel);
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

    // One control block; returns current level Q16.16.
    gain_t advance_block() noexcept {
        for (;;) {
            switch (stage) {
                case Stage::Idle: return 0;
                case Stage::Sustain: return level;
                default: break;
            }
            const u64 next_phase = static_cast<u64>(phase) + step;
            if (next_phase < ENV_PHASE_ONE) {
                phase = static_cast<u32>(next_phase);
                const gain_t c = curve_eval(curve, phase);
                level = start_level + static_cast<gain_t>(
                    asr(static_cast<i64>(target - start_level) * c, GAIN_SHIFT));
                return level;
            }
            // Stage complete: land exactly on target, chain to the next stage.
            level = target;
            switch (stage) {
                case Stage::Attack:
                    enter(Stage::Decay, p->one_shot ? 0 : p->sustain, p->dec);
                    break;
                case Stage::Decay:
                    if (p->one_shot) { kill(); return 0; }
                    stage = Stage::Sustain;
                    level = p->sustain;
                    return level;
                case Stage::Release:
                    kill();
                    return 0;
                default:
                    kill();
                    return 0;
            }
            // Rule 1: a zero-duration next stage falls through within this
            // same block (the loop); a timed stage waits for the next block.
            if (step < ENV_PHASE_ONE) return level;
        }
    }
};

}  // namespace msynth
