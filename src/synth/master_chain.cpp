#include "master_chain.h"

#include <initializer_list>

#include "limiter.h"  // soft_clip_q24

namespace msynth {
namespace {

// dB -> log2 units: dB * log2(10)/20; the constant is 0.16609640 in Q24.
inline i32 db_to_log2_q16(i32 db_q16) noexcept {
    return static_cast<i32>(asr(static_cast<i64>(db_q16) * 2786513LL, 24));
}
constexpr coef_t K_SQRT2 = 1518500250;  // shelf damping sqrt(2), Q2.30
constexpr i32 IN_LIMIT = 32 << SAMPLE_SHIFT;
constexpr int BLOCK_LOG2 = 6;  // CONTROL_BLOCK == 64

inline i32 clampq(i64 v) noexcept { return clamp_i32(v, -IN_LIMIT, IN_LIMIT); }

}  // namespace

// ---- params -----------------------------------------------------------------

void MasterParams::set(MasterParamId id, i32 v) noexcept {
    auto c = [](i32 x, i64 lo, i64 hi) { return clamp_i32(x, lo, hi); };
    constexpr i64 HZ_LO = 20 << GAIN_SHIFT, HZ_HI = static_cast<i64>(20000) << GAIN_SHIFT;
    constexpr i64 DB24 = 24 << GAIN_SHIFT;
    switch (id) {
        case MasterParamId::Enabled: enabled = v != 0; break;
        case MasterParamId::EqLowHz: eq_low_hz = c(v, HZ_LO, HZ_HI); break;
        case MasterParamId::EqLowDb: eq_low_db = c(v, -DB24, DB24); break;
        case MasterParamId::EqMidHz: eq_mid_hz = c(v, HZ_LO, HZ_HI); break;
        case MasterParamId::EqMidDb: eq_mid_db = c(v, -DB24, DB24); break;
        case MasterParamId::EqMidQ: eq_mid_q = c(v, GAIN_ONE / 2, 10 << GAIN_SHIFT); break;
        case MasterParamId::EqHighHz: eq_high_hz = c(v, HZ_LO, HZ_HI); break;
        case MasterParamId::EqHighDb: eq_high_db = c(v, -DB24, DB24); break;
        case MasterParamId::CompThresholdDb: comp_threshold_db = c(v, -(60 << GAIN_SHIFT), 0); break;
        case MasterParamId::CompRatio: comp_ratio = c(v, GAIN_ONE, 20 << GAIN_SHIFT); break;
        case MasterParamId::CompAttackMs: comp_attack_ms = c(v, GAIN_ONE / 10, 200 << GAIN_SHIFT); break;
        case MasterParamId::CompReleaseMs: comp_release_ms = c(v, 5 << GAIN_SHIFT, 2000 << GAIN_SHIFT); break;
        case MasterParamId::CompMakeupDb: comp_makeup_db = c(v, 0, DB24); break;
        case MasterParamId::CompKneeDb: comp_knee_db = c(v, 0, DB24); break;
        case MasterParamId::LimCeilingDb: lim_ceiling_db = c(v, -(12 << GAIN_SHIFT), 0); break;
        case MasterParamId::LimLookaheadBlocks: lim_lookahead_blocks = c(v, 1, 8); break;
        case MasterParamId::LimReleaseMs: lim_release_ms = c(v, GAIN_ONE, 1000 << GAIN_SHIFT); break;
        case MasterParamId::Width: width = c(v, 0, 2 * GAIN_ONE); break;
        case MasterParamId::GainDb: gain_db = c(v, -DB24, DB24); break;
        case MasterParamId::SatDrive: sat_drive = c(v, GAIN_ONE, 8 * GAIN_ONE); break;
        default: break;
    }
}

i32 MasterParams::get(MasterParamId id) const noexcept {
    switch (id) {
        case MasterParamId::Enabled: return enabled ? 1 : 0;
        case MasterParamId::EqLowHz: return eq_low_hz;
        case MasterParamId::EqLowDb: return eq_low_db;
        case MasterParamId::EqMidHz: return eq_mid_hz;
        case MasterParamId::EqMidDb: return eq_mid_db;
        case MasterParamId::EqMidQ: return eq_mid_q;
        case MasterParamId::EqHighHz: return eq_high_hz;
        case MasterParamId::EqHighDb: return eq_high_db;
        case MasterParamId::CompThresholdDb: return comp_threshold_db;
        case MasterParamId::CompRatio: return comp_ratio;
        case MasterParamId::CompAttackMs: return comp_attack_ms;
        case MasterParamId::CompReleaseMs: return comp_release_ms;
        case MasterParamId::CompMakeupDb: return comp_makeup_db;
        case MasterParamId::CompKneeDb: return comp_knee_db;
        case MasterParamId::LimCeilingDb: return lim_ceiling_db;
        case MasterParamId::LimLookaheadBlocks: return lim_lookahead_blocks;
        case MasterParamId::LimReleaseMs: return lim_release_ms;
        case MasterParamId::Width: return width;
        case MasterParamId::GainDb: return gain_db;
        case MasterParamId::SatDrive: return sat_drive;
        default: return 0;
    }
}

// ---- chain -------------------------------------------------------------------

void MasterChain::set_param(MasterParamId id, i32 v) noexcept {
    const bool was_enabled = p_.enabled;
    const i32 old_la = p_.lim_lookahead_blocks;
    p_.set(id, v);
    recompute();
    // Enabling starts from silence in every state; a lookahead change moves
    // the delay tap, so its history is meaningless too.
    if ((p_.enabled && !was_enabled) || old_la != p_.lim_lookahead_blocks) clear();
}

void MasterChain::set_params(const MasterParams& p) noexcept {
    for (int i = 0; i < static_cast<int>(MasterParamId::COUNT); ++i)
        set_param(static_cast<MasterParamId>(i), p.get(static_cast<MasterParamId>(i)));
}

void MasterChain::recompute() noexcept {
    auto shelf = [](Band& b, i32 hz, i32 db) {
        b.coefs.compute(cutoff_g(cutoff_index_from_hz(hz)), K_SQRT2);
        b.delta = db_to_lin_q16(db) - GAIN_ONE;
    };
    shelf(low_, p_.eq_low_hz, p_.eq_low_db);
    shelf(high_, p_.eq_high_hz, p_.eq_high_db);
    // Bell: k = 1/Q (unity-peak bandpass is k * v1).
    i64 k = (static_cast<i64>(COEF_ONE) << GAIN_SHIFT) / p_.eq_mid_q;
    if (k > COEF_MAX) k = COEF_MAX;
    mid_.coefs.compute(cutoff_g(cutoff_index_from_hz(p_.eq_mid_hz)), static_cast<coef_t>(k));
    mid_.delta = db_to_lin_q16(p_.eq_mid_db) - GAIN_ONE;

    thr_log2_ = db_to_log2_q16(p_.comp_threshold_db);
    knee_log2_ = db_to_log2_q16(p_.comp_knee_db);
    // 1 - 1/ratio in Q16.
    slope_ = GAIN_ONE - static_cast<i32>((static_cast<i64>(GAIN_ONE) << GAIN_SHIFT) / p_.comp_ratio);
    att_coef_ = onepole_coef_from_ms(p_.comp_attack_ms);
    rel_coef_ = onepole_coef_from_ms(p_.comp_release_ms);
    makeup_ = db_to_lin_q16(p_.comp_makeup_db);

    half_delta_ = static_cast<i32>(asr(p_.width - GAIN_ONE, 1));
    out_gain_ = db_to_lin_q16(p_.gain_db);

    ceiling_q24_ = db_to_lin_q16(p_.lim_ceiling_db) << 8;  // Q16 -> Q8.24
    lim_rel_coef_ = onepole_coef_from_ms(p_.lim_release_ms);
}

void MasterChain::clear() noexcept {
    for (Band* b : {&low_, &mid_, &high_}) {
        b->l.reset(b->coefs);
        b->r.reset(b->coefs);
    }
    comp_env_ = 0;
    comp_cur_ = GAIN_ONE;
    lim_env_ = lim_cur_ = GAIN_ONE;
    for (i32& p : peaks_) p = 0;
    dl_l_.clear();
    dl_r_.clear();
    meters_.comp_gr = meters_.lim_gain = GAIN_ONE;
}

MasterMeters MasterChain::take_meters() noexcept {
    MasterMeters m = meters_;
    meters_.peak_l = meters_.peak_r = 0;
    meters_.sumsq_l = meters_.sumsq_r = 0;
    meters_.samples = 0;
    meters_.clips = 0;
    return m;
}

void MasterChain::process_block(i32* l, i32* r, int n) noexcept {
    // ---- EQ: three parallel-form bands, always run (delta 0 -> identity) ----
    for (Band* b : {&low_, &mid_, &high_}) {
        b->l.set_block_target(b->coefs, BLOCK_LOG2);
        b->r.set_block_target(b->coefs, BLOCK_LOG2);
    }
    for (int i = 0; i < n; ++i) {
        i64 xl = l[i], xr = r[i];
        // Low shelf: x + delta * LP(x)
        xl += asr(static_cast<i64>(low_.l.process(l[i], FilterMode::Lowpass)) * low_.delta, GAIN_SHIFT);
        xr += asr(static_cast<i64>(low_.r.process(r[i], FilterMode::Lowpass)) * low_.delta, GAIN_SHIFT);
        // Bell: x + delta * k * BP(x)
        const i32 bl = static_cast<i32>(asr(static_cast<i64>(mid_.coefs.k) *
                                            mid_.l.process(l[i], FilterMode::Bandpass), COEF_SHIFT));
        const i32 br = static_cast<i32>(asr(static_cast<i64>(mid_.coefs.k) *
                                            mid_.r.process(r[i], FilterMode::Bandpass), COEF_SHIFT));
        xl += asr(static_cast<i64>(bl) * mid_.delta, GAIN_SHIFT);
        xr += asr(static_cast<i64>(br) * mid_.delta, GAIN_SHIFT);
        // High shelf: x + delta * HP(x)
        xl += asr(static_cast<i64>(high_.l.process(l[i], FilterMode::Highpass)) * high_.delta, GAIN_SHIFT);
        xr += asr(static_cast<i64>(high_.r.process(r[i], FilterMode::Highpass)) * high_.delta, GAIN_SHIFT);
        l[i] = clampq(xl);
        r[i] = clampq(xr);
    }

    // ---- Compressor: block-rate linked peak detector, log2-domain envelope ----
    {
        i32 peak = 0;
        for (int i = 0; i < n; ++i) {
            const i32 al = l[i] < 0 ? -l[i] : l[i];
            const i32 ar = r[i] < 0 ? -r[i] : r[i];
            if (al > peak) peak = al;
            if (ar > peak) peak = ar;
        }
        const u32 lvl_q16 = static_cast<u32>(peak) >> 8;  // Q8.24 -> Q16.16
        const i32 lvl_log2 = lvl_q16 ? log2_q16(lvl_q16) : -(24 << GAIN_SHIFT);
        const i32 over = lvl_log2 - thr_log2_;
        const i32 half = knee_log2_ / 2;
        i32 gr = 0;  // desired gain reduction, log2 units
        if (over >= half) {
            gr = gmul(over, slope_);
        } else if (over > -half) {  // inside the soft knee: quadratic blend
            const i64 t = over + half;                                // 0 .. knee
            const i64 t2 = asr(t * t, GAIN_SHIFT);                    // Q16
            gr = static_cast<i32>((asr(t2 * slope_, GAIN_SHIFT) << GAIN_SHIFT) /
                                  (2 * static_cast<i64>(knee_log2_)));
        }
        const gain_t coef = gr > comp_env_ ? att_coef_ : rel_coef_;
        comp_env_ += static_cast<i32>(asr(static_cast<i64>(gr - comp_env_) * coef, GAIN_SHIFT));
        if (comp_env_ < 0) comp_env_ = 0;
        const gain_t gr_lin = exp2_q16(-comp_env_);
        meters_.comp_gr = gr_lin;
        const gain_t target = gmul(gr_lin, makeup_);
        const i32 step = static_cast<i32>(asr(static_cast<i64>(target) - comp_cur_, BLOCK_LOG2));
        gain_t g = comp_cur_;
        for (int i = 0; i < n; ++i) {
            g += step;
            l[i] = clampq(asr(static_cast<i64>(l[i]) * g, GAIN_SHIFT));
            r[i] = clampq(asr(static_cast<i64>(r[i]) * g, GAIN_SHIFT));
        }
        comp_cur_ = target;
    }

    // ---- Bus saturation (post-comp): cubic soft clip at +/-1.0, gated so
    // drive 1.0 is an exact no-op ----
    if (p_.sat_drive != GAIN_ONE) {
        for (int i = 0; i < n; ++i) {
            l[i] = soft_clip_q24(asr(static_cast<i64>(l[i]) * p_.sat_drive, GAIN_SHIFT));
            r[i] = soft_clip_q24(asr(static_cast<i64>(r[i]) * p_.sat_drive, GAIN_SHIFT));
        }
    }

    // ---- Stereo width (difference form; width 1.0 is bit-exact identity) ----
    if (half_delta_ != 0) {
        for (int i = 0; i < n; ++i) {
            const i64 t = asr(static_cast<i64>(l[i] - r[i]) * half_delta_, GAIN_SHIFT);
            l[i] = clampq(l[i] + t);
            r[i] = clampq(r[i] - t);
        }
    }

    // ---- Output gain (0 dB is exactly GAIN_ONE: skipped) ----
    if (out_gain_ != GAIN_ONE) {
        for (int i = 0; i < n; ++i) {
            l[i] = clampq(asr(static_cast<i64>(l[i]) * out_gain_, GAIN_SHIFT));
            r[i] = clampq(asr(static_cast<i64>(r[i]) * out_gain_, GAIN_SHIFT));
        }
    }

    // ---- Lookahead limiter ----
    // The block entering now is emitted `lookahead` blocks later. The gain
    // target is the minimum needed over every block currently in the delay
    // (newest included), so both ends of this block's ramp are already at or
    // below what the block being emitted requires: the ceiling holds.
    {
        i32 peak = 0;
        for (int i = 0; i < n; ++i) {
            const i32 al = l[i] < 0 ? -l[i] : l[i];
            const i32 ar = r[i] < 0 ? -r[i] : r[i];
            if (al > peak) peak = al;
            if (ar > peak) peak = ar;
        }
        const int la = p_.lim_lookahead_blocks;
        for (int i = la; i > 0; --i) peaks_[i] = peaks_[i - 1];
        peaks_[0] = peak;
        i32 pmax = 0;
        for (int i = 0; i <= la; ++i)
            if (peaks_[i] > pmax) pmax = peaks_[i];
        gain_t need = GAIN_ONE;
        if (pmax > ceiling_q24_) {
            need = static_cast<gain_t>((static_cast<i64>(ceiling_q24_) << GAIN_SHIFT) / pmax) - 1;
            if (need < 0) need = 0;
        }
        if (need < lim_env_) lim_env_ = need;  // instant attack
        else lim_env_ += static_cast<i32>(asr(static_cast<i64>(need - lim_env_) * lim_rel_coef_, GAIN_SHIFT));
        meters_.lim_gain = lim_env_;
        const gain_t target = lim_env_;
        const i32 step = static_cast<i32>(asr(static_cast<i64>(target) - lim_cur_, BLOCK_LOG2));
        const u32 d = static_cast<u32>(la) * CONTROL_BLOCK;
        gain_t g = lim_cur_;
        for (int i = 0; i < n; ++i) {
            g += step;
            dl_l_.push(l[i]);
            dl_r_.push(r[i]);
            // read(1) is the sample just pushed; read(d + 1) is d samples ago.
            l[i] = static_cast<i32>(asr(static_cast<i64>(dl_l_.read(d + 1)) * g, GAIN_SHIFT));
            r[i] = static_cast<i32>(asr(static_cast<i64>(dl_r_.read(d + 1)) * g, GAIN_SHIFT));
        }
        lim_cur_ = target;
    }
}

}  // namespace msynth
