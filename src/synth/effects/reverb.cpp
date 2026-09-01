#include "reverb.h"

namespace msynth {

namespace {
// Classic Freeverb tunings (44.1kHz), rescaled to 48kHz with integer math.
constexpr int COMB_44K[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
constexpr int ALLPASS_44K[4] = {556, 441, 341, 225};
constexpr int STEREO_SPREAD = 23;

constexpr u32 scale_len(int len44, int extra) {
    return static_cast<u32>((static_cast<i64>(len44) * 48000) / 44100) + static_cast<u32>(extra);
}
}  // namespace

void Reverb::set_params(const ReverbParams& p) noexcept {
    for (int c = 0; c < 2; ++c) {
        const int spread = c == 0 ? 0 : STEREO_SPREAD;
        for (int i = 0; i < NUM_COMBS; ++i) comb_[c][i].len = scale_len(COMB_44K[i], spread);
        for (int i = 0; i < NUM_ALLPASS; ++i) allpass_[c][i].len = scale_len(ALLPASS_44K[i], spread);
    }
    // feedback = 0.7 + 0.28 * roomSize; damp = 0.4 * damping.
    feedback_ = static_cast<gain_t>((GAIN_ONE * 7) / 10 +
                                    asr(static_cast<i64>(p.room_size) * ((GAIN_ONE * 28) / 100),
                                        GAIN_SHIFT));
    damp1_ = static_cast<gain_t>(asr(static_cast<i64>(p.damping) * ((GAIN_ONE * 4) / 10), GAIN_SHIFT));
    damp2_ = GAIN_ONE - damp1_;
    // Width: wet1 = wet*(width/2 + 0.5), wet2 = wet*((1-width)/2).
    const gain_t w = p.wet_gain;
    wet1_ = static_cast<gain_t>(asr(static_cast<i64>(w) * (p.width / 2 + GAIN_ONE / 2), GAIN_SHIFT));
    wet2_ = static_cast<gain_t>(asr(static_cast<i64>(w) * ((GAIN_ONE - p.width) / 2), GAIN_SHIFT));
}

void Reverb::clear() noexcept {
    for (int c = 0; c < 2; ++c) {
        for (auto& cb : comb_[c]) {
            cb.line.clear();
            cb.filter_store = 0;
        }
        for (auto& ap : allpass_[c]) ap.line.clear();
    }
}

void Reverb::process_block(const i64* in_l, const i64* in_r, i64* out_l, i64* out_r,
                           int n) noexcept {
    for (int i = 0; i < n; ++i) {
        // Freeverb feeds a mono-summed, attenuated input to both banks.
        const i32 input = clamp_i32(asr(in_l[i] + in_r[i], 2), -(8LL << SAMPLE_SHIFT),
                                    8LL << SAMPLE_SHIFT);
        i32 wet[2] = {0, 0};
        for (int c = 0; c < 2; ++c) {
            i64 acc = 0;
            for (auto& cb : comb_[c]) {
                const i32 out = cb.line.read(cb.len);
                // Lowpass in the feedback path.
                cb.filter_store = static_cast<i32>(
                    asr(static_cast<i64>(out) * damp2_ + static_cast<i64>(cb.filter_store) * damp1_,
                        GAIN_SHIFT));
                cb.line.push(input + static_cast<i32>(
                    asr(static_cast<i64>(cb.filter_store) * feedback_, GAIN_SHIFT)));
                acc += out;
            }
            i32 v = clamp_i32(acc, -(64LL << SAMPLE_SHIFT), 64LL << SAMPLE_SHIFT);
            for (auto& ap : allpass_[c]) {
                const i32 bufout = ap.line.read(ap.len);
                ap.line.push(v + static_cast<i32>(asr(static_cast<i64>(bufout), 1)));
                v = bufout - v;
            }
            wet[c] = v;
        }
        out_l[i] += asr(static_cast<i64>(wet[0]) * wet1_ + static_cast<i64>(wet[1]) * wet2_,
                        GAIN_SHIFT);
        out_r[i] += asr(static_cast<i64>(wet[1]) * wet1_ + static_cast<i64>(wet[0]) * wet2_,
                        GAIN_SHIFT);
    }
}

}  // namespace msynth
