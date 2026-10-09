#pragma once
//
// Patch fitting: find synth parameters whose render matches a target sample.
//
// Shared by midisynth_fit (the CLI) and the DAW's Fit panel (through WASM),
// so both run the identical optimiser and loss -- a fit done in the browser
// is the same fit the CLI would produce.
//
// The engine is fixed-point integer arithmetic, so there is nothing to
// backpropagate through: gradients are FINITE DIFFERENCES, one extra render
// per parameter per step, which is affordable because the renders happen
// in-process.
//
// The ARCHITECTURE is fixed by hand -- oscillator types, filter modes, layer
// count, wavetable recipe -- and only the continuous knobs move. The discrete
// structural choices are exactly the ones a gradient cannot cross.
//
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "../synth/patch_loader.h"
#include "../synth/synth_engine.h"

namespace msynth {
namespace fit {

// ---------------------------------------------------------------- loss
inline void fft(std::vector<std::complex<float>>& a) {
    const size_t n = a.size();
    if (n < 2) return;
    std::vector<std::complex<float>> ev(n / 2), od(n / 2);
    for (size_t i = 0; i < n / 2; ++i) { ev[i] = a[2 * i]; od[i] = a[2 * i + 1]; }
    fft(ev);
    fft(od);
    for (size_t k = 0; k < n / 2; ++k) {
        const auto t = std::polar(1.0f, -2.0f * 3.14159265358979f * k / n) * od[k];
        a[k] = ev[k] + t;
        a[k + n / 2] = ev[k] - t;
    }
}

inline std::vector<float> log_spectrum(const std::vector<float>& x, size_t N, size_t hop) {
    std::vector<float> out;
    if (x.size() < N) return out;
    for (size_t off = 0; off + N <= x.size(); off += hop) {
        std::vector<std::complex<float>> buf(N);
        for (size_t i = 0; i < N; ++i) {
            const float w = 0.5f - 0.5f * std::cos(2.0f * 3.14159265358979f * i / N);
            buf[i] = {x[off + i] * w, 0.0f};
        }
        fft(buf);
        for (size_t k = 0; k < N / 2; ++k) out.push_back(std::log(1e-5f + std::abs(buf[k])));
    }
    return out;
}

// Octave-band energy distribution, as percentages of the total. The raw STFT
// term below treats every bin alike, so a near-silent bin counts as much as
// the fundamental -- which lets the optimiser win on paper with a sound whose
// gross spectral balance is nothing like the target. This term is what stops
// that: it compares where the energy actually IS.
inline std::vector<float> band_profile(const std::vector<float>& x) {
    static const float EDGES[] = {0, 35, 60, 120, 250, 600, 1500, 4000, 24000};
    constexpr int NB = 8;
    std::vector<float> acc(NB, 0.0f);
    const size_t N = 4096;
    for (size_t off = 0; off + N <= x.size(); off += N) {
        std::vector<std::complex<float>> buf(N);
        for (size_t i = 0; i < N; ++i) {
            const float w = 0.5f - 0.5f * std::cos(2.0f * 3.14159265358979f * i / N);
            buf[i] = {x[off + i] * w, 0.0f};
        }
        fft(buf);
        for (size_t k = 0; k < N / 2; ++k) {
            const float f = k * static_cast<float>(SAMPLE_RATE) / N;
            const float m = std::abs(buf[k]) * std::abs(buf[k]);
            for (int b = 0; b < NB; ++b)
                if (f >= EDGES[b] && f < EDGES[b + 1]) { acc[b] += m; break; }
        }
    }
    float tot = 0;
    for (float v : acc) tot += v;
    if (tot <= 0) tot = 1;
    for (float& v : acc) v = 100.0f * v / tot;
    return acc;
}

// Multi-resolution: short windows catch the transient, long ones the tone.
// The objective, as a set of weighted terms. Which terms you include changes
// what "best fit" means, and it matters more than any other setting here: a
// pure STFT loss treats a near-silent bin as equal to the fundamental, and
// will happily land on a sound in the wrong octaves. Exposed so it can be
// built up in the UI rather than baked in.
struct LossWeights {
    float stft = 1.0f;    // multi-resolution log-magnitude spectrogram distance
    float env = 0.05f;    // amplitude envelope in dB, so decays line up
    float bands = 0.04f;  // gross octave-band balance: where the energy IS
};

struct LossParts {
    float stft = 0, env = 0, bands = 0, total = 0;
};

inline LossParts loss_parts(const std::vector<float>& a, const std::vector<float>& b,
                            const LossWeights& w) {
    LossParts p;
    if (w.stft > 0) {
        float total = 0;
        int terms = 0;
        for (size_t N : {256u, 1024u, 4096u}) {
            const auto sa = log_spectrum(a, N, N / 4), sb = log_spectrum(b, N, N / 4);
            const size_t n = std::min(sa.size(), sb.size());
            if (!n) continue;
            float acc = 0;
            for (size_t i = 0; i < n; ++i) acc += std::fabs(sa[i] - sb[i]);
            total += acc / n;
            ++terms;
        }
        p.stft = terms ? total / terms : 0;
    }
    if (w.env > 0) {
        const size_t blk = SAMPLE_RATE / 200;
        float env = 0;
        int eterms = 0;
        for (size_t off = 0; off + blk <= std::min(a.size(), b.size()); off += blk) {
            float ra = 0, rb = 0;
            for (size_t i = 0; i < blk; ++i) {
                ra += a[off + i] * a[off + i];
                rb += b[off + i] * b[off + i];
            }
            env += std::fabs(20 * std::log10(1e-6f + std::sqrt(ra / blk)) -
                             20 * std::log10(1e-6f + std::sqrt(rb / blk)));
            ++eterms;
        }
        p.env = eterms ? env / eterms : 0;
    }
    if (w.bands > 0) {
        const auto pa = band_profile(a), pb = band_profile(b);
        float bands = 0;
        for (size_t i = 0; i < pa.size(); ++i) bands += std::fabs(pa[i] - pb[i]);
        p.bands = bands;
    }
    p.total = w.stft * p.stft + w.env * p.env + w.bands * p.bands;
    return p;
}

inline float spectral_loss(const std::vector<float>& a, const std::vector<float>& b,
                           const LossWeights& w = LossWeights{}) {
    return loss_parts(a, b, w).total;
}

// ---------------------------------------------------------------- params
struct Param {
    const char* name;
    float lo, hi;
    bool log;   // frequencies and times are perceived logarithmically
};

struct Arch {
    const char* name;
    const char* json;         // template, one %g per parameter, in order
    std::vector<Param> params;
    float seconds;
    // Melodic architectures are loaded as a bank program and played at
    // `note`, so the fitted patch tracks the keyboard; percussive ones go
    // into the kit at note 36 where pitch is part of the sound.
    bool melodic = false;
    int note = 36;
    float hold = 0.0f;        // seconds to hold the key (0 = whole window)
};

// The fixed oscillator setups. Everything structural is written down here;
// the optimiser only moves the numbers.
inline std::vector<Arch> architectures() {
    return {
        {"kick",
         R"({"name":"fit_kick","oneShot":true,"layers":[
           {"pitchNote":%g,"mixGain":%g,
            "wavetable":{"generator":"additiveHarmonicMorph","frameCount":1,
                         "params":{"startHarmonics":1,"endHarmonics":1,"falloff":"flat"}},
            "ampEnv":{"attack":0,"decay":%g,"sustain":0,"release":0,"decayCurve":"exp"},
            "pitchEnv":{"attack":0,"decay":%g,"sustain":0,"release":0,"depthSemis":%g},
            "filter":{"mode":"lowpass","cutoffHz":%g,"resonance":%g,"drive":%g}},
           {"osc":"noise","noiseColor":"white","mixGain":%g,
            "ampEnv":{"attack":0,"decay":%g,"sustain":0,"release":0,"decayCurve":"exp"},
            "filter":{"mode":"bandpass","cutoffHz":%g,"resonance":0.3}}]})",
         {{"pitchNote", 20, 48, false}, {"bodyGain", 0.1f, 1.0f, false},
          {"bodyDecay", 40, 600, true}, {"pitchDecay", 4, 120, true},
          {"pitchDepth", 0, 48, false}, {"cutoff", 200, 6000, true},
          {"reso", 0, 0.8f, false}, {"drive", 1, 6, false},
          {"clickGain", 0, 0.6f, false}, {"clickDecay", 3, 60, true},
          {"clickHz", 800, 8000, true}},
         0.7f, false, 36, 0.0f},

        {"snare",
         R"({"name":"fit_snare","oneShot":true,"layers":[
           {"pitchNote":%g,"mixGain":%g,
            "wavetable":{"generator":"additiveHarmonicMorph","frameCount":1,
                         "params":{"startHarmonics":3,"endHarmonics":3,"falloff":"saw"}},
            "ampEnv":{"attack":0,"decay":%g,"sustain":0,"release":0,"decayCurve":"exp"},
            "pitchEnv":{"attack":0,"decay":%g,"sustain":0,"release":0,"depthSemis":%g},
            "filter":{"mode":"lowpass","cutoffHz":%g,"resonance":%g,"drive":%g}},
           {"osc":"noise","noiseColor":"pink","mixGain":%g,
            "ampEnv":{"attack":0,"decay":%g,"sustain":0,"release":0,"decayCurve":"exp"},
            "filter":{"mode":"highpass","cutoffHz":%g,"slope":12},
            "filter2":{"mode":"lowpass","cutoffHz":%g}},
           {"osc":"noise","noiseColor":"white","mixGain":%g,
            "ampEnv":{"attack":0,"decay":%g,"sustain":0,"release":0,"decayCurve":"exp"},
            "filter":{"mode":"bandpass","cutoffHz":%g,"resonance":0.25}}]})",
         {{"pitchNote", 40, 70, false}, {"bodyGain", 0.1f, 1.0f, false},
          {"bodyDecay", 40, 400, true}, {"pitchDecay", 5, 90, true},
          {"pitchDepth", 0, 14, false}, {"bodyCutoff", 300, 3000, true},
          {"bodyReso", 0, 0.7f, false}, {"bodyDrive", 1, 4, false},
          {"wireGain", 0.05f, 1.0f, false}, {"wireDecay", 40, 500, true},
          {"wireHp", 400, 4000, true}, {"wireLp", 4000, 12000, true},
          {"snapGain", 0, 0.8f, false}, {"snapDecay", 5, 80, true},
          {"snapHz", 1500, 8000, true}},
         0.6f},


        {"bass",
         R"({"name":"fit_bass","layers":[
           {"pitchNote":%g,"mixGain":%g,
            "wavetable":{"generator":"additiveHarmonicMorph","frameCount":1,
                         "params":{"startHarmonics":24,"endHarmonics":24,"falloff":"saw"}},
            "unison":{"voices":3,"detuneCents":%g,"spread":0.4,"phaseSpread":1},
            "ampEnv":{"attack":4,"decay":0,"sustain":1,"release":60},
            "filter":{"mode":"lowpass","cutoffHz":%g,"resonance":%g,"slope":24,"drive":%g},
            "postDrive":%g},
           {"pitchNote":%g,"mixGain":%g,
            "wavetable":{"generator":"additiveHarmonicMorph","frameCount":1,
                         "params":{"startHarmonics":1,"endHarmonics":1,"falloff":"flat"}},
            "ampEnv":{"attack":4,"decay":0,"sustain":1,"release":60},
            "filter":{"mode":"lowpass","cutoffHz":150,"slope":24}}]})",
         {{"pitchNote", 26, 62, false}, {"sawGain", 0.1f, 1.0f, false},
          {"detuneCents", 0, 30, false}, {"cutoff", 90, 3000, true},
          {"resonance", 0, 0.92f, false}, {"drive", 1, 5, false},
          {"postDrive", 1, 1.7f, false}, {"subNote", 14, 50, false},
          {"subGain", 0, 1.0f, false}},
         1.2f, false, 36, 0.0f},  // fixed pitchNote: rendered like a one-shot

        // ---- melodic: loaded as a bank program and played at `note`, so the
        // fitted patch tracks the keyboard instead of baking in a pitch. The
        // detune parameter lets the optimiser align to a target recorded at a
        // different pitch (+/- 2 octaves).

        {"piano", R"({"name":"fit_piano","layers":[
           {"detuneCents":%g,"mixGain":%g,
            "wavetable":{"generator":"additiveHarmonicMorph","frameCount":1,
                         "params":{"startHarmonics":1,"endHarmonics":1,"falloff":"flat"}},
            "ring":{"mode":"fm","ratio":%g,"index":%g},
            "ampEnv":{"attack":%g,"decay":%g,"sustain":%g,"release":%g,"decayCurve":"exp"},
            "filterEnv":{"attack":0,"decay":%g,"sustain":0.05,"release":200},
            "filter":{"mode":"lowpass","cutoffHz":%g,"slope":12},
            "modMatrix":[{"src":"FilterEnv","dst":"RingAmount","amount":%g},
                         {"src":"Velocity","dst":"RingAmount","amount":0.15}]},
           {"detuneCents":%g,"mixGain":%g,
            "wavetable":{"generator":"additiveHarmonicMorph","frameCount":1,
                         "params":{"startHarmonics":1,"endHarmonics":1,"falloff":"flat"}},
            "ring":{"mode":"fm","ratio":%g,"index":%g},
            "ampEnv":{"attack":0,"decay":%g,"sustain":0,"release":120,"decayCurve":"exp"},
            "filter":{"mode":"highpass","cutoffHz":600}}]})",
         {{"detuneCents", -2400, 2400, false}, {"bodyGain", 0.1f, 1.0f, false},
          {"fmRatio", 1, 8, false}, {"fmIndex", 0, 3, false},
          {"attack", 1, 40, true}, {"decay", 300, 6000, true},
          {"sustain", 0, 0.6f, false}, {"release", 80, 1500, true},
          {"indexDecay", 60, 2000, true}, {"cutoff", 800, 9000, true},
          {"indexEnv", 0, 0.5f, false},
          {"tineDetune", 0, 2400, false}, {"tineGain", 0, 0.4f, false},
          {"tineRatio", 2, 12, false}, {"tineIndex", 0, 2, false},
          {"tineDecay", 40, 600, true}},
         2.2f, true, 60, 1.4f},

        {"strings", R"({"name":"fit_strings","layers":[
           {"detuneCents":%g,"mixGain":%g,
            "wavetable":{"generator":"additiveHarmonicMorph","frameCount":1,
                         "params":{"startHarmonics":%g,"endHarmonics":%g,"falloff":"saw"}},
            "unison":{"voices":5,"detuneCents":%g,"spread":%g,"phaseSpread":1},
            "ampEnv":{"attack":%g,"decay":%g,"sustain":%g,"release":%g,"attackCurve":"sCurve"},
            "filter":{"mode":"lowpass","cutoffHz":%g,"resonance":%g,"slope":12},
            "filterEnv":{"attack":%g,"decay":800,"sustain":0.5,"release":600,"depth":%g},
            "keyTrackFilter":0.4,
            "lfos":[{"shape":"sine","rateHz":%g,"retrigger":false}],
            "modMatrix":[{"src":"LFO1","dst":"Pitch","amount":%g}]}]})",
         {{"detuneCents", -2400, 2400, false}, {"gain", 0.2f, 1.0f, false},
          {"harmonics", 8, 40, false}, {"harmonicsEnd", 8, 40, false},
          {"unisonDetune", 2, 30, false}, {"spread", 0, 1, false},
          {"attack", 20, 600, true}, {"decay", 200, 3000, true},
          {"sustain", 0.3f, 1.0f, false}, {"release", 100, 1500, true},
          {"cutoff", 400, 8000, true}, {"resonance", 0, 0.5f, false},
          {"filterAttack", 10, 900, true}, {"filterDepth", -0.5f, 0.6f, false},
          {"vibratoHz", 2, 7, false}, {"vibrato", 0, 0.03f, false}},
         2.2f, true, 60, 1.5f},

        {"pad", R"({"name":"fit_pad","layers":[
           {"detuneCents":%g,"mixGain":%g,
            "wavetable":{"generator":"additiveHarmonicMorph","frameCount":8,
                         "params":{"startHarmonics":%g,"endHarmonics":%g,"falloff":"triangle"}},
            "unison":{"voices":3,"detuneCents":%g,"spread":1,"phaseSpread":1},
            "ampEnv":{"attack":%g,"decay":%g,"sustain":%g,"release":%g,"attackCurve":"sCurve"},
            "wtPosEnv":{"attack":%g,"decay":2000,"sustain":0.6,"release":1500,"depth":%g},
            "filter":{"mode":"lowpass","cutoffHz":%g,"resonance":%g,"slope":24},
            "keyTrackFilter":0.3}]})",
         {{"detuneCents", -2400, 2400, false}, {"gain", 0.2f, 1.0f, false},
          {"harmonics", 4, 24, false}, {"harmonicsEnd", 6, 40, false},
          {"unisonDetune", 2, 24, false},
          {"attack", 50, 1500, true}, {"decay", 400, 4000, true},
          {"sustain", 0.3f, 1.0f, false}, {"release", 200, 2500, true},
          {"wtAttack", 100, 3000, true}, {"wtDepth", 0, 1, false},
          {"cutoff", 300, 6000, true}, {"resonance", 0, 0.5f, false}},
         2.4f, true, 60, 1.6f},

        {"organ", R"({"name":"fit_organ","layers":[
           {"detuneCents":%g,"mixGain":%g,
            "wavetable":{"generator":"drawbar","frameCount":1,
                         "params":{"drawbars":[%g,%g,%g,%g,%g,%g,%g,%g,%g]}},
            "ampEnv":{"attack":%g,"decay":0,"sustain":1,"release":%g},
            "filter":{"mode":"lowpass","cutoffHz":%g,"slope":12,"drive":%g}}]})",
         {{"detuneCents", -2400, 2400, false}, {"gain", 0.2f, 1.0f, false},
          {"db16", 0, 8, false}, {"db5", 0, 8, false}, {"db8", 0, 8, false},
          {"db4", 0, 8, false}, {"db2_2", 0, 8, false}, {"db2", 0, 8, false},
          {"db1_3", 0, 8, false}, {"db1_1", 0, 8, false}, {"db1", 0, 8, false},
          {"attack", 1, 60, true}, {"release", 20, 400, true},
          {"cutoff", 800, 10000, true}, {"drive", 1, 3, false}},
         1.8f, true, 60, 1.2f},

        {"brass", R"({"name":"fit_brass","layers":[
           {"detuneCents":%g,"mixGain":%g,
            "wavetable":{"generator":"additiveHarmonicMorph","frameCount":1,
                         "params":{"startHarmonics":%g,"endHarmonics":%g,"falloff":"saw"}},
            "unison":{"voices":2,"detuneCents":%g,"spread":0.3,"phaseSpread":1},
            "ampEnv":{"attack":%g,"decay":%g,"sustain":%g,"release":%g},
            "filter":{"mode":"lowpass","cutoffHz":%g,"resonance":%g,"slope":24,"drive":%g},
            "filterEnv":{"attack":%g,"decay":%g,"sustain":0.4,"release":200,"depth":%g},
            "keyTrackFilter":0.5}]})",
         {{"detuneCents", -2400, 2400, false}, {"gain", 0.2f, 1.0f, false},
          {"harmonics", 10, 40, false}, {"harmonicsEnd", 10, 40, false},
          {"unisonDetune", 0, 20, false},
          {"attack", 10, 250, true}, {"decay", 100, 1200, true},
          {"sustain", 0.3f, 1.0f, false}, {"release", 60, 600, true},
          {"cutoff", 400, 6000, true}, {"resonance", 0, 0.6f, false},
          {"drive", 1, 4, false}, {"filterAttack", 5, 300, true},
          {"filterDecay", 80, 1200, true}, {"filterDepth", 0, 0.8f, false}},
         2.0f, true, 60, 1.3f},

        {"pluck", R"({"name":"fit_pluck","layers":[
           {"detuneCents":%g,"mixGain":%g,
            "wavetable":{"generator":"additiveHarmonicMorph","frameCount":1,
                         "params":{"startHarmonics":%g,"endHarmonics":%g,"falloff":"saw"}},
            "unison":{"voices":2,"detuneCents":%g,"spread":0.3,"phaseSpread":1},
            "ampEnv":{"attack":%g,"decay":%g,"sustain":0,"release":%g,"decayCurve":"exp"},
            "filter":{"mode":"lowpass","cutoffHz":%g,"resonance":%g,"slope":24,"drive":%g},
            "filterEnv":{"attack":0,"decay":%g,"sustain":%g,"release":150,"depth":%g},
            "keyTrackFilter":0.5}]})",
         {{"detuneCents", -2400, 2400, false}, {"gain", 0.2f, 1.0f, false},
          {"harmonics", 6, 40, false}, {"harmonicsEnd", 6, 40, false},
          {"unisonDetune", 0, 16, false},
          {"attack", 0.5f, 30, true}, {"decay", 80, 2500, true},
          {"release", 40, 800, true}, {"cutoff", 300, 8000, true},
          {"resonance", 0, 0.7f, false}, {"drive", 1, 4, false},
          {"filterDecay", 30, 900, true}, {"filterSustain", 0, 0.5f, false},
          {"filterDepth", 0, 0.9f, false}},
         2.0f, true, 60, 0.9f},

        {"bell", R"({"name":"fit_bell","layers":[
           {"detuneCents":%g,"mixGain":%g,
            "wavetable":{"generator":"additiveHarmonicMorph","frameCount":1,
                         "params":{"startHarmonics":1,"endHarmonics":1,"falloff":"flat"}},
            "ring":{"mode":"fm","ratio":%g,"index":%g},
            "ampEnv":{"attack":%g,"decay":%g,"sustain":0,"release":%g,"decayCurve":"exp"},
            "filterEnv":{"attack":0,"decay":%g,"sustain":0,"release":200},
            "filter":{"mode":"lowpass","cutoffHz":%g,"slope":12},
            "modMatrix":[{"src":"FilterEnv","dst":"RingAmount","amount":%g}]}]})",
         {{"detuneCents", -2400, 2400, false}, {"gain", 0.2f, 1.0f, false},
          {"fmRatio", 1.5f, 14, false}, {"fmIndex", 0, 6, false},
          {"attack", 0.5f, 20, true}, {"decay", 400, 6000, true},
          {"release", 200, 3000, true}, {"indexDecay", 80, 2500, true},
          {"cutoff", 1000, 11000, true}, {"indexEnv", 0, 0.8f, false}},
         2.6f, true, 72, 0.6f},

        {"lead", R"({"name":"fit_lead","layers":[
           {"detuneCents":%g,"mixGain":%g,
            "wavetable":{"generator":"pulse","frameCount":8,
                         "params":{"startDuty":%g,"endDuty":%g}},
            "unison":{"voices":3,"detuneCents":%g,"spread":0.5,"phaseSpread":1},
            "ampEnv":{"attack":%g,"decay":%g,"sustain":%g,"release":%g},
            "wtPosEnv":{"attack":%g,"decay":900,"sustain":0.5,"release":400,"depth":%g},
            "filter":{"mode":"lowpass","cutoffHz":%g,"resonance":%g,"slope":24,"drive":%g},
            "keyTrackFilter":0.4}]})",
         {{"detuneCents", -2400, 2400, false}, {"gain", 0.2f, 1.0f, false},
          {"duty", 0.05f, 0.5f, false}, {"dutyEnd", 0.05f, 0.5f, false},
          {"unisonDetune", 0, 25, false},
          {"attack", 1, 200, true}, {"decay", 100, 2000, true},
          {"sustain", 0.2f, 1.0f, false}, {"release", 40, 800, true},
          {"wtAttack", 50, 1500, true}, {"wtDepth", 0, 1, false},
          {"cutoff", 400, 9000, true}, {"resonance", 0, 0.7f, false},
          {"drive", 1, 4, false}},
         2.0f, true, 60, 1.2f},
        {"hat",
         R"({"name":"fit_hat","oneShot":true,"layers":[
           {"osc":"noise","noiseColor":"pink","mixGain":%g,
            "ampEnv":{"attack":0,"decay":%g,"sustain":0,"release":0,"decayCurve":"exp"},
            "filter":{"mode":"highpass","cutoffHz":%g,"slope":12},
            "filter2":{"mode":"lowpass","cutoffHz":%g}},
           {"osc":"metal","pitchNote":%g,"mixGain":%g,
            "ampEnv":{"attack":0,"decay":%g,"sustain":0,"release":0,"decayCurve":"exp"},
            "filter":{"mode":"bandpass","cutoffHz":%g,"resonance":%g}}]})",
         {{"airGain", 0.1f, 1.0f, false}, {"airDecay", 10, 600, true},
          {"airHp", 800, 8000, true}, {"airLp", 5000, 12000, true},
          {"metalNote", 60, 96, false}, {"metalGain", 0, 0.8f, false},
          {"metalDecay", 8, 500, true}, {"metalHz", 2000, 10000, true},
          {"metalReso", 0, 0.8f, false}},
         0.6f, false, 36, 0.0f},
    };
}

inline float denorm(const Param& p, float u) {
    u = std::min(1.0f, std::max(0.0f, u));
    return p.log ? p.lo * std::pow(p.hi / p.lo, u) : p.lo + (p.hi - p.lo) * u;
}

inline std::string build_json(const Arch& a, const std::vector<float>& u) {
    // The template carries one %g per parameter, in order. Substituted by
    // hand rather than through snprintf so an architecture can have any
    // number of knobs.
    std::string out;
    const std::string tpl = a.json;
    size_t at = 0, idx = 0;
    while (true) {
        const size_t hit = tpl.find("%g", at);
        if (hit == std::string::npos || idx >= u.size()) {
            out += tpl.substr(at);
            break;
        }
        out += tpl.substr(at, hit - at);
        char num[32];
        std::snprintf(num, sizeof(num), "%g", denorm(a.params[idx], u[idx]));
        out += num;
        at = hit + 2;
        ++idx;
    }
    return idx == u.size() ? out : std::string{};
}

// ---------------------------------------------------------------- render
// One engine, reused: constructing it per candidate would dominate the cost.
struct Renderer {
    WaveTableCache cache;
    PatchBank bank;
    DrumKit kit;
    SynthEngine engine;
    size_t frames;

    explicit Renderer(float seconds) : frames(static_cast<size_t>(seconds * SAMPLE_RATE)) {
        tables_init();
        make_builtin_fallback(cache, bank.fallback());
        engine.set_content(&bank, &kit);
        // Dry: any send return would be fitted as if it were the instrument.
        for (auto cfg : {EngineCommand::Type::ReverbConfig, EngineCommand::Type::ChorusConfig}) {
            EngineCommand c;
            c.type = cfg;
            c.d1 = cfg == EngineCommand::Type::ReverbConfig ? 3 : 2;
            c.v = 0;
            engine.queue().push(c);
        }
    }

    // `melodic` loads the patch as bank program 0 and plays `note`, so the
    // result is a keyboard-tracking instrument rather than a fixed-pitch hit.
    bool melodic = false;
    int note = 36;
    float hold = 0.0f;

    bool render(const std::string& patch_json, std::vector<float>& out) {
        if (melodic) return render_melodic(patch_json, out);
        std::string doc = "{\"version\":1,\"notes\":{\"36\":" + patch_json + "}}";
        LoadResult r = load_drum_kit(doc.c_str(), doc.size(), cache, kit);
        if (!r.ok) return false;
        engine.set_content(&bank, &kit);
        EngineCommand off;                       // flush the previous candidate
        off.type = EngineCommand::Type::CC;
        off.slot = DRUM_CHANNEL;
        off.d1 = 120;
        engine.queue().push(off);
        std::vector<i16> pcm(2 * CONTROL_BLOCK);
        engine.render(pcm.data(), CONTROL_BLOCK);

        EngineCommand on;
        on.type = EngineCommand::Type::NoteOn;
        on.slot = DRUM_CHANNEL;
        on.d1 = 36;
        on.d2 = 120;
        engine.queue().push(on);
        std::vector<i16> buf(2 * frames);
        engine.render(buf.data(), static_cast<int>(frames));
        out.resize(frames);
        float pk = 0;
        for (size_t i = 0; i < frames; ++i) {
            out[i] = (buf[2 * i] + buf[2 * i + 1]) / 65536.0f;
            pk = std::max(pk, std::fabs(out[i]));
        }
        if (pk > 0)
            for (float& v : out) v /= pk;        // fit shape, not output gain
        return true;
    }

    // Melodic: the patch becomes bank program 0 on a normal channel, so the
    // note is played rather than baked into the sound. Sustained instruments
    // need the key released partway through or the release never happens.
    bool render_melodic(const std::string& patch_json, std::vector<float>& out) {
        const std::string doc = "{\"version\":1,\"programs\":{\"0\":" + patch_json + "}}";
        LoadResult r = load_patch_bank(doc.c_str(), doc.size(), cache, bank);
        if (!r.ok) return false;
        engine.set_content(&bank, &kit);
        EngineCommand off;
        off.type = EngineCommand::Type::CC;
        off.slot = 0;
        off.d1 = 120;
        engine.queue().push(off);
        std::vector<i16> flush(2 * CONTROL_BLOCK);
        engine.render(flush.data(), CONTROL_BLOCK);

        EngineCommand prog;
        prog.type = EngineCommand::Type::Program;
        prog.slot = 0;
        prog.d1 = 0;
        engine.queue().push(prog);
        EngineCommand on;
        on.type = EngineCommand::Type::NoteOn;
        on.slot = 0;
        on.d1 = static_cast<u8>(note);
        on.d2 = 110;
        engine.queue().push(on);

        const size_t held = hold > 0 ? std::min(frames, static_cast<size_t>(hold * SAMPLE_RATE))
                                     : frames;
        std::vector<i16> buf(2 * frames);
        engine.render(buf.data(), static_cast<int>(held));
        if (held < frames) {
            EngineCommand noff;
            noff.type = EngineCommand::Type::NoteOff;
            noff.slot = 0;
            noff.d1 = static_cast<u8>(note);
            engine.queue().push(noff);
            engine.render(buf.data() + 2 * held, static_cast<int>(frames - held));
        }
        out.resize(frames);
        float pk = 0;
        for (size_t i = 0; i < frames; ++i) {
            out[i] = (buf[2 * i] + buf[2 * i + 1]) / 65536.0f;
            pk = std::max(pk, std::fabs(out[i]));
        }
        if (pk > 0)
            for (float& v : out) v /= pk;
        return true;
    }
};



// One restart: descend from `start` (or the centre when empty) and return the
// best parameters found, with `loss_out` set. JS drives the restart loop so it
// can report progress between them.
inline std::vector<float> fit_once(const Arch& arch, const std::vector<float>& target,
                                   Renderer& rend, int iters, unsigned seed,
                                   float& loss_out, const LossWeights& weights = LossWeights{}) {
    const size_t P = arch.params.size();
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> uni(0.15f, 0.85f);
    std::vector<float> u(P);
    for (size_t i = 0; i < P; ++i) u[i] = seed == 0 ? 0.5f : uni(rng);

    auto evaluate = [&](const std::vector<float>& c) -> float {
        std::vector<float> got;
        const std::string js = build_json(arch, c);
        if (js.empty() || !rend.render(js, got)) return 1e30f;
        return spectral_loss(target, got, weights);
    };

    float loss = evaluate(u), step = 0.12f;
    int stalls = 0;
    for (int it = 0; it < iters; ++it) {
        const float eps = std::max(0.004f, std::min(0.03f, step * 0.3f));
        std::vector<float> grad(P, 0.0f);
        for (size_t i = 0; i < P; ++i) {
            std::vector<float> up = u;
            up[i] = std::min(1.0f, up[i] + eps);
            const float d = up[i] - u[i];
            if (d <= 0) continue;
            grad[i] = (evaluate(up) - loss) / d;
        }
        float norm = 0;
        for (float g : grad) norm += g * g;
        norm = std::sqrt(norm);
        if (norm < 1e-9f) break;

        bool improved = false;
        float trial = step;
        for (int back = 0; back < 5 && !improved; ++back) {
            std::vector<float> cand(P);
            for (size_t i = 0; i < P; ++i)
                cand[i] = std::min(1.0f, std::max(0.0f, u[i] - trial * grad[i] / norm));
            const float cl = evaluate(cand);
            if (cl < loss - 1e-6f) {
                u = cand; loss = cl; improved = true;
                step = std::min(0.3f, trial * 1.5f);
            } else {
                trial *= 0.4f;
            }
        }
        if (improved) {
            stalls = 0;
        } else if (++stalls >= 8) {
            break;
        } else {
            step = 0.12f;
            std::uniform_int_distribution<size_t> pick(0, P - 1);
            std::uniform_real_distribution<float> jit(-0.08f, 0.08f);
            std::vector<float> hop = u;
            const size_t j = pick(rng);
            hop[j] = std::min(1.0f, std::max(0.0f, hop[j] + jit(rng)));
            const float hl = evaluate(hop);
            if (hl < loss) { u = hop; loss = hl; }
        }
    }
    loss_out = loss;
    return u;
}

// Target conditioning: trim the leading silence and normalise, so we fit the
// timbre rather than the recording's gain staging or pre-roll.
inline void prepare(std::vector<float>& x, size_t len) {
    float pk = 0;
    for (float v : x) pk = std::max(pk, std::fabs(v));
    if (pk <= 0) { x.assign(len, 0.0f); return; }
    size_t on = 0;
    while (on < x.size() && std::fabs(x[on]) < pk * 0.02f) ++on;
    std::vector<float> out(len, 0.0f);
    for (size_t i = 0; i < len && on + i < x.size(); ++i) out[i] = x[on + i] / pk;
    x.swap(out);
}

}  // namespace fit
}  // namespace msynth
