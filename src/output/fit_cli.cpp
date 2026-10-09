//
// midisynth_fit: fit a patch to a recorded sample.
//
//   midisynth_fit target.wav --arch kick --out fitted.json [--render fit.wav]
//                 [--iters 60] [--restarts 4] [--seed 1]
//
// The engine is fixed-point integer arithmetic, so there is nothing to
// backpropagate through: gradients are FINITE DIFFERENCES, one extra render
// per parameter per step. That is affordable because a drum hit is a few
// hundred milliseconds and the renders happen in-process.
//
// The ARCHITECTURE is fixed by hand -- oscillator types, filter modes, layer
// count, wavetable recipe -- and only the continuous knobs move. That is what
// keeps the search tractable: the discrete structural choices are exactly the
// ones a gradient cannot cross.
//
// Loss is a multi-resolution STFT log-magnitude distance (the standard for
// audio matching: comparing waveforms directly would be dominated by phase,
// which nobody hears) plus an amplitude-envelope term so decays line up.
//
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "fit.h"
#include "wav_writer.h"

using namespace msynth;

namespace {
using namespace msynth::fit;


// ---------------------------------------------------------------- wav in
// 16-bit PCM only -- convert anything else first (ffmpeg -c:a pcm_s16le).
bool read_wav16(const std::string& path, std::vector<float>& mono, int& rate) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4))
        return false;
    int channels = 0, bits = 0;
    size_t pos = 12, data_at = 0, data_len = 0;
    while (pos + 8 <= b.size()) {
        u32 sz;
        std::memcpy(&sz, b.data() + pos + 4, 4);
        if (!std::memcmp(b.data() + pos, "fmt ", 4)) {
            u16 ch, bp;
            u32 sr;
            std::memcpy(&ch, b.data() + pos + 10, 2);
            std::memcpy(&sr, b.data() + pos + 12, 4);
            std::memcpy(&bp, b.data() + pos + 22, 2);
            channels = ch;
            rate = static_cast<int>(sr);
            bits = bp;
        } else if (!std::memcmp(b.data() + pos, "data", 4)) {
            data_at = pos + 8;
            data_len = sz;
        }
        pos += 8 + sz + (sz & 1);
    }
    if (!data_at || bits != 16 || channels < 1) return false;
    const size_t n = std::min<size_t>(data_len, b.size() - data_at) / 2;
    mono.clear();
    for (size_t i = 0; i + channels <= n; i += channels) {
        int acc = 0;
        for (int c = 0; c < channels; ++c) {
            i16 v;
            std::memcpy(&v, b.data() + data_at + (i + c) * 2, 2);
            acc += v;
        }
        mono.push_back(static_cast<float>(acc) / (channels * 32768.0f));
    }
    return true;
}

// Linear resample to the engine rate; drum hits have no content where this
// would matter and it keeps the tool dependency-free.
void resample_to_engine(std::vector<float>& x, int from) {
    if (from == SAMPLE_RATE || from <= 0 || x.empty()) return;
    const double step = static_cast<double>(from) / SAMPLE_RATE;
    std::vector<float> out;
    out.reserve(static_cast<size_t>(x.size() / step) + 1);
    for (double p = 0; p + 1 < x.size(); p += step) {
        const size_t i = static_cast<size_t>(p);
        const float t = static_cast<float>(p - i);
        out.push_back(x[i] * (1 - t) + x[i + 1] * t);
    }
    x.swap(out);
}


}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: midisynth_fit target.wav --arch NAME [--out patch.json]\n"
            "  percussive: kick snare hat bass   melodic: piano strings pad organ\n"
            "              brass pluck bell lead  (melodic wants ONE sustained note)\n"
            "       [--render fit.wav] [--iters 60] [--restarts 4] [--seed 1]\n"
            "       [--window MS]  fit only the first MS (a recording's room tail\n"
            "                      is not something the synth can reproduce)\n"
            "  target must be 16-bit PCM wav (ffmpeg -i in -c:a pcm_s16le out.wav)\n");
        return 2;
    }
    std::string target_path = argv[1], arch_name = "kick", out_path, render_path;
    int iters = 60, restarts = 4;
    unsigned seed = 1;
    // A recorded hit is mostly room. The synth has no room, so fitting the
    // whole tail makes it chase reverb instead of the drum -- keep the window
    // on the direct sound.
    float window_ms = 0;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--arch") arch_name = next();
        else if (a == "--out") out_path = next();
        else if (a == "--render") render_path = next();
        else if (a == "--iters") iters = std::atoi(next());
        else if (a == "--restarts") restarts = std::atoi(next());
        else if (a == "--seed") seed = static_cast<unsigned>(std::atoi(next()));
        else if (a == "--window") window_ms = static_cast<float>(std::atof(next()));
    }

    const auto archs = architectures();
    const Arch* arch = nullptr;
    for (const auto& a : archs)
        if (arch_name == a.name) arch = &a;
    if (!arch) {
        std::fprintf(stderr, "unknown --arch '%s' (have:", arch_name.c_str());
        for (const auto& a : archs) std::fprintf(stderr, " %s", a.name);
        std::fprintf(stderr, ")\n");
        return 2;
    }

    std::vector<float> target;
    int rate = SAMPLE_RATE;
    if (!read_wav16(target_path, target, rate)) {
        std::fprintf(stderr, "cannot read '%s' (16-bit PCM wav only)\n", target_path.c_str());
        return 1;
    }
    resample_to_engine(target, rate);
    const float seconds = window_ms > 0 ? window_ms / 1000.0f : arch->seconds;
    const size_t frames = static_cast<size_t>(seconds * SAMPLE_RATE);
    prepare(target, frames);

    Renderer rend(seconds);
    const size_t P = arch->params.size();
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> uni(0.15f, 0.85f);

    std::vector<float> best_u;
    float best_loss = 1e30f;
    int renders = 0;

    auto evaluate = [&](const std::vector<float>& u) -> float {
        std::vector<float> got;
        const std::string js = build_json(*arch, u);
        ++renders;
        if (js.empty() || !rend.render(js, got)) return 1e30f;
        return spectral_loss(target, got);
    };

    for (int r = 0; r < restarts; ++r) {
        std::vector<float> u(P);
        for (size_t i = 0; i < P; ++i) u[i] = r == 0 ? 0.5f : uni(rng);
        float loss = evaluate(u), step = 0.12f;
        int stalls = 0;

        for (int it = 0; it < iters; ++it) {
            // Measure the gradient at the scale we actually move: a fixed eps
            // beside a collapsed step samples a different function entirely.
            const float eps = std::max(0.004f, std::min(0.03f, step * 0.3f));
            std::vector<float> grad(P, 0.0f);
            for (size_t i = 0; i < P; ++i) {
                std::vector<float> up = u;
                up[i] = std::min(1.0f, up[i] + eps);
                const float d = up[i] - u[i];
                if (d <= 0) continue;                 // already pinned at the top
                grad[i] = (evaluate(up) - loss) / d;
            }
            float norm = 0;
            for (float g : grad) norm += g * g;
            norm = std::sqrt(norm);
            if (norm < 1e-9f) break;

            // The line search owns its own trial length; `step` is only a
            // starting estimate, and it must never ratchet down permanently.
            bool improved = false;
            float trial = step;
            for (int back = 0; back < 5 && !improved; ++back) {
                std::vector<float> cand(P);
                for (size_t i = 0; i < P; ++i)
                    cand[i] = std::min(1.0f, std::max(0.0f, u[i] - trial * grad[i] / norm));
                const float cl = evaluate(cand);
                if (cl < loss - 1e-6f) {
                    u = cand;
                    loss = cl;
                    improved = true;
                    step = std::min(0.3f, trial * 1.5f);
                } else {
                    trial *= 0.4f;
                }
            }
            if (improved) {
                stalls = 0;
            } else if (++stalls >= 8) {
                break;                                 // genuinely stuck, not just unlucky
            } else {
                // Nudge along one axis and carry on: this landscape has flats
                // that a pure descent direction cannot leave.
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
        if (loss < best_loss) { best_loss = loss; best_u = u; }
        std::fprintf(stderr, "restart %d: loss %.4f%s\n", r, loss, loss == best_loss ? "  <- best" : "");
    }

    if (best_u.empty()) {
        std::fprintf(stderr, "fit failed\n");
        return 1;
    }
    const std::string js = build_json(*arch, best_u);
    std::fprintf(stderr, "\nbest loss %.4f after %d renders\n", best_loss, renders);
    for (size_t i = 0; i < P; ++i)
        std::fprintf(stderr, "  %-12s %g\n", arch->params[i].name, denorm(arch->params[i], best_u[i]));

    if (!out_path.empty()) {
        std::ofstream f(out_path);
        f << js << "\n";
        std::fprintf(stderr, "wrote %s\n", out_path.c_str());
    } else {
        std::printf("%s\n", js.c_str());
    }
    if (!render_path.empty()) {
        std::vector<float> got;
        rend.render(js, got);
        std::vector<i16> pcm(got.size() * 2);
        for (size_t i = 0; i < got.size(); ++i) {
            const i16 v = static_cast<i16>(std::max(-1.0f, std::min(1.0f, got[i] * 0.9f)) * 32767);
            pcm[2 * i] = pcm[2 * i + 1] = v;
        }
        write_wav16(render_path, pcm, SAMPLE_RATE, 2);
        std::fprintf(stderr, "rendered %s\n", render_path.c_str());
    }
    return 0;
}
