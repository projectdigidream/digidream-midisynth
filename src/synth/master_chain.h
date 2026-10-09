#pragma once
//
// Mastering chain: EQ -> compressor -> stereo width -> output gain ->
// lookahead limiter, plus level meters. Sits after master gain / ducking and
// before the soft clip + dither of OutputStage (limiter.h), so the game plays
// the mastered result identically everywhere -- it is baked into .mstk (MSTR
// section) and applied through EngineCommand::Type::MasterParam.
//
// BYPASSED BY DEFAULT: with `enabled == false` the engine's output path is
// byte-identical to an engine without this file (golden renders unchanged).
// When enabled, the limiter adds lookahead_blocks * CONTROL_BLOCK samples of
// latency (2 blocks = 128 samples = 2.7 ms by default).
//
// Fixed-point throughout (Q8.24 audio, Q16.16 gains, Q2.30 filter coefs,
// log2-domain dynamics via tables.h): no floats, no libm, deterministic.
//
#include "effects/delay_line.h"
#include "filter.h"

namespace msynth {

// Parameter ids are part of the .mstk MSTR section: APPEND ONLY.
enum class MasterParamId : u8 {
    Enabled = 0,        // 0/1
    EqLowHz,            // Q16 Hz, low shelf corner        [20, 20000]
    EqLowDb,            // Q16 dB                          [-24, 24]
    EqMidHz,            // Q16 Hz, bell centre             [20, 20000]
    EqMidDb,            // Q16 dB                          [-24, 24]
    EqMidQ,             // Q16 Q                           [0.5, 10]
    EqHighHz,           // Q16 Hz, high shelf corner       [20, 20000]
    EqHighDb,           // Q16 dB                          [-24, 24]
    CompThresholdDb,    // Q16 dBFS                        [-60, 0]
    CompRatio,          // Q16 ratio                       [1, 20]
    CompAttackMs,       // Q16 ms                          [0.1, 200]
    CompReleaseMs,      // Q16 ms                          [5, 2000]
    CompMakeupDb,       // Q16 dB                          [0, 24]
    CompKneeDb,         // Q16 dB soft-knee width          [0, 24]
    LimCeilingDb,       // Q16 dBFS                        [-12, 0]
    LimLookaheadBlocks, // integer control blocks          [1, 8]
    LimReleaseMs,       // Q16 ms                          [1, 1000]
    Width,              // Q16 stereo width, 1.0 = as is   [0, 2]
    GainDb,             // Q16 output gain                 [-24, 24]
    SatDrive,           // Q16 bus saturation (post-comp)  [1, 8]; 1 = off
    COUNT
};

struct MasterParams {
    bool enabled = false;
    i32 eq_low_hz = 120 << GAIN_SHIFT, eq_low_db = 0;
    i32 eq_mid_hz = 1000 << GAIN_SHIFT, eq_mid_db = 0, eq_mid_q = GAIN_ONE;
    i32 eq_high_hz = 8000 << GAIN_SHIFT, eq_high_db = 0;
    i32 comp_threshold_db = -(12 << GAIN_SHIFT), comp_ratio = 2 << GAIN_SHIFT;
    i32 comp_attack_ms = 10 << GAIN_SHIFT, comp_release_ms = 150 << GAIN_SHIFT;
    i32 comp_makeup_db = 0, comp_knee_db = 6 << GAIN_SHIFT;
    i32 lim_ceiling_db = -(1 << GAIN_SHIFT), lim_lookahead_blocks = 2;
    i32 lim_release_ms = 80 << GAIN_SHIFT;
    i32 width = GAIN_ONE, gain_db = 0;
    i32 sat_drive = GAIN_ONE;  // bus saturation between compressor and width

    // Clamped setters/getters by id (the single place ranges live).
    void set(MasterParamId id, i32 v) noexcept;
    i32 get(MasterParamId id) const noexcept;
};

struct MasterMeters {
    i32 peak_l = 0, peak_r = 0;    // Q8.24 absolute peak (pre soft clip)
    u64 sumsq_l = 0, sumsq_r = 0;  // sum of (|v| >> 8)^2  (Q16.16 squared)
    u32 samples = 0;               // samples accumulated
    gain_t comp_gr = GAIN_ONE;     // compressor gain reduction (linear, <= 1)
    gain_t lim_gain = GAIN_ONE;    // limiter gain (linear, <= 1)
    u32 clips = 0;                 // samples beyond +/-1.0 (soft clip engaged)
};

class MasterChain {
  public:
    MasterChain() { recompute(); }

    void set_param(MasterParamId id, i32 v) noexcept;
    void set_params(const MasterParams& p) noexcept;
    const MasterParams& params() const noexcept { return p_; }
    bool enabled() const noexcept { return p_.enabled; }

    // Reset all state (filters, envelopes, delay lines). Called on enable.
    void clear() noexcept;

    // One control block in place. l/r are Q8.24, |x| <= 32.0.
    void process_block(i32* l, i32* r, int n) noexcept;

    // Meter one output sample (both paths, bypass included).
    inline void meter(i64 l, i64 r) noexcept {
        constexpr i64 LIM = static_cast<i64>(32) << SAMPLE_SHIFT;
        i64 al = l < 0 ? -l : l;
        i64 ar = r < 0 ? -r : r;
        if (al > SAMPLE_ONE || ar > SAMPLE_ONE) ++meters_.clips;
        if (al > LIM) al = LIM;
        if (ar > LIM) ar = LIM;
        if (al > meters_.peak_l) meters_.peak_l = static_cast<i32>(al);
        if (ar > meters_.peak_r) meters_.peak_r = static_cast<i32>(ar);
        const u64 ql = static_cast<u64>(al >> 8), qr = static_cast<u64>(ar >> 8);
        meters_.sumsq_l += ql * ql;
        meters_.sumsq_r += qr * qr;
        if (++meters_.samples >= (1u << 20)) {  // keep the mean, bound the sums
            meters_.sumsq_l >>= 1;
            meters_.sumsq_r >>= 1;
            meters_.samples >>= 1;
        }
    }
    // Read and reset the level accumulators (gains are instantaneous).
    MasterMeters take_meters() noexcept;
    const MasterMeters& meters() const noexcept { return meters_; }

    // Delay introduced by the limiter when enabled (samples).
    int latency_samples() const noexcept {
        return p_.enabled ? p_.lim_lookahead_blocks * CONTROL_BLOCK : 0;
    }

  private:
    struct Band {
        FilterCoefs coefs;
        FilterState l, r;
        gain_t delta = 0;  // lin(dB) - 1
    };
    void recompute() noexcept;

    MasterParams p_;
    Band low_, mid_, high_;
    // Compressor (log2 domain, Q16).
    i32 thr_log2_ = 0, knee_log2_ = 0;
    gain_t slope_ = 0, att_coef_ = GAIN_ONE, rel_coef_ = GAIN_ONE, makeup_ = GAIN_ONE;
    i32 comp_env_ = 0;                 // gain reduction in log2 units
    gain_t comp_cur_ = GAIN_ONE;       // current applied gain
    // Width / gain.
    i32 half_delta_ = 0;
    gain_t out_gain_ = GAIN_ONE;
    // Limiter.
    i32 ceiling_q24_ = SAMPLE_ONE;
    gain_t lim_rel_coef_ = GAIN_ONE;
    gain_t lim_env_ = GAIN_ONE, lim_cur_ = GAIN_ONE;
    i32 peaks_[9] = {};                // block peaks, [0] = newest
    DelayLine<10> dl_l_, dl_r_;        // 1024 > 8 blocks + 1 (read(d + 1) with d <= 512)
    MasterMeters meters_;
};

}  // namespace msynth
