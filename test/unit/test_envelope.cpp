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
    CHECK_EQ(e.level, GAIN_ONE / 2);
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
    const gain_t mid = e.level;
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
    const gain_t mid = e.level;
    CHECK(mid > 0);
    e.note_on(p);  // retrigger of a ringing voice: normal entry path
    CHECK_EQ(e.start_level, mid);
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
