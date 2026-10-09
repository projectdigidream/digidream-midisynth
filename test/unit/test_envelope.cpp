//
// Envelope state machine: each test pins one of Appendix C's structural
// rules.
//
#include "test_framework.h"

#include "synth/envelope.h"

using namespace msynth;

namespace {
EnvParams make_env(u32 att, u32 dec, gain_t sus, u32 rel, bool one_shot = false) {
    EnvParams p;
    p.att = {att, CurveId::Linear};
    p.dec = {dec, CurveId::Linear};
    p.rel = {rel, CurveId::Linear};
    p.sustain = sus;
    p.one_shot = one_shot;
    return p;
}
}  // namespace

TEST(env_full_adsr_cycle) {
    EnvParams p = make_env(4, 4, GAIN_ONE / 2, 4);
    EnvelopeState e;
    e.note_on(p);
    // Attack: rises to ONE.
    gain_t prev = 0;
    for (int i = 0; i < 4; ++i) {
        const gain_t v = e.advance_block();
        CHECK(v >= prev);
        prev = v;
    }
    CHECK(e.stage == EnvelopeState::Stage::Attack || prev == GAIN_ONE);
    // Decay to sustain.
    for (int i = 0; i < 8; ++i) e.advance_block();
    CHECK(e.stage == EnvelopeState::Stage::Sustain);
    CHECK_EQ(e.level, ENV_ONE / 2);
    // Sustain holds indefinitely.
    for (int i = 0; i < 100; ++i) CHECK_EQ(e.advance_block(), GAIN_ONE / 2);
    // Release to idle.
    e.note_off();
    for (int i = 0; i < 8; ++i) e.advance_block();
    CHECK(e.idle());
    CHECK_EQ(e.advance_block(), 0);
}

TEST(env_rule1_zero_duration_chains_within_block) {
    // 0ms attack + 0ms decay must pass through to sustain in ONE block.
    EnvParams p = make_env(0, 0, GAIN_ONE / 4, 4);
    EnvelopeState e;
    e.note_on(p);
    const gain_t v = e.advance_block();
    CHECK(e.stage == EnvelopeState::Stage::Sustain);
    CHECK_EQ(v, GAIN_ONE / 4);
}

TEST(env_rule2_termination_is_structural) {
    // Exp curves still end exactly at the stage boundary -- no asymptotic
    // never-reaching-zero leak.
    EnvParams p = make_env(1, 1, 0, 6);
    p.rel.curve = CurveId::Exp;
    EnvelopeState e;
    e.note_on(p);
    for (int i = 0; i < 4; ++i) e.advance_block();
    e.note_off();
    int blocks = 0;
    while (!e.idle() && blocks < 100) {
        e.advance_block();
        ++blocks;
    }
    CHECK(blocks <= 8);  // 6-block release terminates on schedule, not "eventually"
    CHECK_EQ(e.level, 0);
}

TEST(env_rule3_release_during_attack_no_jump) {
    EnvParams p = make_env(100, 10, GAIN_ONE, 10);
    EnvelopeState e;
    e.note_on(p);
    for (int i = 0; i < 10; ++i) e.advance_block();  // partway up the attack
    const gain_t mid = e.level_q16();
    CHECK(mid > 0 && mid < GAIN_ONE);
    e.note_off();
    // Release starts from the current level, first block stays close to it.
    const gain_t after = e.advance_block();
    CHECK(after <= mid);
    CHECK(after > mid - mid / 4);
}

TEST(env_rule3_retrigger_from_nonzero_level) {
    EnvParams p = make_env(10, 10, GAIN_ONE / 2, 10);
    EnvelopeState e;
    e.note_on(p);
    for (int i = 0; i < 30; ++i) e.advance_block();  // in sustain
    e.note_off();
    e.advance_block();  // partway down release
    const gain_t mid = e.level_q16();
    CHECK(mid > 0);
    e.note_on(p);  // retrigger of a ringing voice: normal entry path
    CHECK_NEAR(e.start_level, asl(static_cast<i32>(mid), 8), 256);  // Q24 vs Q16 view
    const gain_t v = e.advance_block();
    CHECK(v >= mid);  // rises from current level, not from zero
}

TEST(env_oneshot_skips_sustain_and_ignores_noteoff) {
    EnvParams p = make_env(2, 4, 0, 50, /*one_shot=*/true);
    EnvelopeState e;
    e.note_on(p);
    e.note_off();  // must be ignored (drums)
    int blocks = 0;
    while (!e.idle() && blocks < 100) {
        e.advance_block();
        ++blocks;
    }
    // Attack(2) + decay(4) then Idle -- release(50) never entered.
    CHECK(blocks <= 8);
}

TEST(env_rule6_fast_release_from_any_stage) {
    EnvParams p = make_env(100, 100, GAIN_ONE, 500);
    EnvelopeState e;
    e.note_on(p);
    for (int i = 0; i < 5; ++i) e.advance_block();
    e.fast_release(3);
    int blocks = 0;
    while (!e.idle() && blocks < 10) {
        e.advance_block();
        ++blocks;
    }
    CHECK(blocks <= 4);  // ~4ms, not the patch's 500-block release
}

TEST(env_fast_release_on_idle_is_noop) {
    EnvelopeState e;
    e.fast_release(3);
    CHECK(e.idle());
    CHECK_EQ(e.advance_block(), 0);
}

TEST(env_instant_release) {
    EnvParams p = make_env(0, 0, GAIN_ONE, 0);
    EnvelopeState e;
    e.note_on(p);
    e.advance_block();
    CHECK(e.stage == EnvelopeState::Stage::Sustain);
    e.note_off();
    e.advance_block();
    CHECK(e.idle());
}

TEST(env_level_bounded) {
    for (auto curve : {CurveId::Linear, CurveId::Exp, CurveId::ExpAttack, CurveId::SCurve}) {
        EnvParams p = make_env(7, 5, GAIN_ONE / 3, 9);
        p.att.curve = p.dec.curve = p.rel.curve = curve;
        EnvelopeState e;
        e.note_on(p);
        for (int i = 0; i < 20; ++i) {
            const gain_t v = e.advance_block();
            CHECK(v >= 0 && v <= GAIN_ONE);
        }
        e.note_off();
        for (int i = 0; i < 20; ++i) {
            const gain_t v = e.advance_block();
            CHECK(v >= 0 && v <= GAIN_ONE);
        }
    }
}

TEST(env_decay_monotonic_down_to_sustain) {
    EnvParams p = make_env(1, 20, GAIN_ONE / 3, 5);
    p.dec.curve = CurveId::Exp;
    EnvelopeState e;
    e.note_on(p);
    e.advance_block();  // finish attack
    gain_t prev = GAIN_ONE;
    for (int i = 0; i < 25; ++i) {
        const gain_t v = e.advance_block();
        CHECK(v <= prev);
        prev = v;
    }
    CHECK_EQ(prev, GAIN_ONE / 3);
}

TEST(envelope_dahdsr_and_loop) {
    // Powers of two: ENV_PHASE_ONE divides exactly, so stage boundaries land
    // on precise block counts.
    EnvParams p;
    p.del.duration_blocks = 8;
    p.att.duration_blocks = 4;
    p.hold.duration_blocks = 8;
    p.dec.duration_blocks = 4;
    p.dec.curve = CurveId::Linear;
    p.att.curve = CurveId::Linear;
    p.sustain = GAIN_ONE / 2;
    p.rel.duration_blocks = 4;
    EnvelopeState e;
    e.note_on(p);
    // Delay: level stays at 0 (the 8th call completes the delay, still 0).
    for (int i = 0; i < 8; ++i) CHECK_EQ(e.advance_block(), 0);
    // Attack rises to full over 4 blocks.
    gain_t mid = 0;
    for (int i = 0; i < 4; ++i) mid = e.advance_block();
    CHECK_EQ(mid, GAIN_ONE);
    // Hold pins full level.
    for (int i = 0; i < 8; ++i) CHECK_EQ(e.advance_block(), GAIN_ONE);
    // Decay to sustain.
    for (int i = 0; i < 4; ++i) e.advance_block();
    CHECK_EQ(e.advance_block(), GAIN_ONE / 2);

    // Loop: attack <-> decay cycles while held, still releasable.
    EnvParams lp;
    lp.att.duration_blocks = 4;
    lp.att.curve = CurveId::Linear;
    lp.dec.duration_blocks = 4;
    lp.dec.curve = CurveId::Linear;
    lp.sustain = 0;
    lp.rel.duration_blocks = 2;
    lp.loop = true;
    EnvelopeState le;
    le.note_on(lp);
    gain_t peak = 0, trough = GAIN_ONE;
    for (int i = 0; i < 40; ++i) {
        const gain_t v = le.advance_block();
        peak = std::max(peak, v);
        if (i > 8) trough = std::min(trough, v);
    }
    CHECK_EQ(peak, GAIN_ONE);
    CHECK(trough < GAIN_ONE / 4);   // it came back down...
    CHECK(!le.idle());              // ...and never stopped
    le.note_off();
    for (int i = 0; i < 6; ++i) le.advance_block();
    CHECK(le.idle());

    // Scaled release: half-speed scale doubles the tail.
    EnvelopeState s1, s2;
    EnvParams sp;
    sp.rel.duration_blocks = 20;
    sp.rel.curve = CurveId::Linear;
    s1.note_on(sp);
    s2.note_on(sp);
    s1.advance_block();
    s2.advance_block();
    s1.note_off(GAIN_ONE);
    s2.note_off(2 * GAIN_ONE);
    int b1 = 0, b2 = 0;
    while (!s1.idle() && b1 < 100) { s1.advance_block(); ++b1; }
    while (!s2.idle() && b2 < 100) { s2.advance_block(); ++b2; }
    CHECK_NEAR(b2, 2 * b1, 3);
}

TEST(envelope_q24_tail_has_fine_resolution) {
    // Release from a quiet level (-56 dBFS): in Q16 the whole tail would
    // traverse ~100 distinct values; Q8.24 gives hundreds of steps across
    // even a short window, so the decay glides.
    EnvParams p;
    p.att.duration_blocks = 0;
    p.dec.duration_blocks = 0;
    p.sustain = 100;  // Q16: ~1.5e-3
    p.rel.duration_blocks = 512;
    p.rel.curve = CurveId::Exp;
    EnvelopeState e;
    e.note_on(p);
    e.advance_block();
    CHECK_EQ(e.level, 100 << 8);  // sustain lands exactly in Q24
    e.note_off();
    int distinct = 0;
    i32 prev = -1;
    for (int i = 0; i < 512; ++i) {
        e.advance_block();
        if (e.level != prev) { ++distinct; prev = e.level; }
        // Monotone, never negative.
        CHECK(e.level >= 0);
    }
    CHECK(distinct > 400);  // Q16 would cap this at <= 101
    CHECK(e.idle());
    // The Q16 view still reports landmark values exactly.
    EnvelopeState f;
    EnvParams q;
    q.att.duration_blocks = 0;
    q.sustain = GAIN_ONE / 2;
    f.note_on(q);
    CHECK_EQ(f.advance_block(), GAIN_ONE / 2);
}

TEST(envelope_key_scaled_rate) {
    // dur_scale halves the decay: sustain reached in half the blocks.
    EnvParams p;
    p.att.duration_blocks = 0;
    p.dec.duration_blocks = 64;
    p.dec.curve = CurveId::Linear;
    p.sustain = 0;
    auto blocks_to_floor = [&](gain_t scale) {
        EnvelopeState e;
        e.note_on(p, scale);
        int n = 0;
        while (e.advance_block() > 0 && n < 400) ++n;
        return n;
    };
    const int full = blocks_to_floor(GAIN_ONE);
    const int half = blocks_to_floor(GAIN_ONE / 2);
    const int twice = blocks_to_floor(2 * GAIN_ONE);
    CHECK_NEAR(half * 2, full, 4);
    CHECK_NEAR(twice, full * 2, 4);
    // Extreme scaling still lands on the scaled duration (64/16 = 4).
    CHECK_NEAR(blocks_to_floor(GAIN_ONE / 16), 4, 2);
}
