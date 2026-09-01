#pragma once
//
// Shared helpers for engine/sequencer/integration tests.
//
#include <cstring>
#include <memory>
#include <vector>

#include "synth/patch_loader.h"
#include "synth/synth_engine.h"

namespace testfw {

using namespace msynth;

struct EngineFixture {
    WaveTableCache cache;
    PatchBank bank;
    DrumKit kit;
    SynthEngine engine;

    EngineFixture() {
        tables_init();
        make_builtin_fallback(cache, bank.fallback());
        // A plain, fast-attack sine patch on program 0 for predictable tests.
        static const char* kSimple = R"({"name":"test_sine","layers":[{
            "wavetable": {"generator":"additiveHarmonicMorph","frameCount":1,
                          "params":{"startHarmonics":1,"endHarmonics":1,"falloff":"flat"}},
            "ampEnv": {"attack":1,"decay":1,"sustain":1.0,"release":20}}]})";
        std::string err;
        JsonPtr doc = json_parse(kSimple, std::strlen(kSimple), &err);
        auto p = std::make_unique<Patch>();
        load_patch(*doc, cache, *p);
        bank.set_program(0, std::move(p));
        // Drums: a one-shot kick on 36, chokeable hats on 42/46.
        static const char* kKit = R"({"version":1,"notes":{
          "36": {"name":"kick","oneShot":true,"layers":[
                 {"pitchNote":33,"ampEnv":{"attack":0,"decay":150,"sustain":0,"release":0},
                  "pitchEnv":{"attack":0,"decay":40,"sustain":0,"release":0,"depthSemis":24}}]},
          "42": {"name":"chh","oneShot":true,"exclusiveGroup":1,"layers":[
                 {"osc":"noise","ampEnv":{"attack":0,"decay":40,"sustain":0,"release":0}}]},
          "46": {"name":"ohh","oneShot":true,"exclusiveGroup":1,"layers":[
                 {"osc":"noise","ampEnv":{"attack":0,"decay":400,"sustain":0,"release":0}}]}
        }})";
        load_drum_kit(kKit, std::strlen(kKit), cache, kit);
        engine.set_content(&bank, &kit);
        // Tests assert exact silence after decays; the GM-default reverb
        // send (CC91=40) would otherwise ring for seconds. Zero the effect
        // returns; tests that exercise the effects set them back explicitly.
        set_reverb_wet(0);
        set_chorus_wet(0);
    }

    void set_reverb_wet(gain_t wet) {
        EngineCommand c;
        c.type = EngineCommand::Type::ReverbConfig;
        c.d1 = 3;
        c.v = wet;
        cmd(c);
    }
    void set_chorus_wet(gain_t wet) {
        EngineCommand c;
        c.type = EngineCommand::Type::ChorusConfig;
        c.d1 = 2;
        c.v = wet;
        cmd(c);
    }

    void cmd(EngineCommand c, u64 when = 0) {
        c.when = when;
        engine.queue().push(c);
    }
    void note_on(u8 slot, u8 note, u8 vel, u64 when = 0) {
        EngineCommand c;
        c.type = EngineCommand::Type::NoteOn;
        c.slot = slot;
        c.d1 = note;
        c.d2 = vel;
        cmd(c, when);
    }
    void note_off(u8 slot, u8 note, u64 when = 0) {
        EngineCommand c;
        c.type = EngineCommand::Type::NoteOff;
        c.slot = slot;
        c.d1 = note;
        cmd(c, when);
    }
    void cc(u8 slot, u8 num, u8 val, u64 when = 0) {
        EngineCommand c;
        c.type = EngineCommand::Type::CC;
        c.slot = slot;
        c.d1 = num;
        c.d2 = val;
        cmd(c, when);
    }

    // Render N frames, return interleaved stereo.
    std::vector<i16> render(int frames) {
        std::vector<i16> out(static_cast<size_t>(frames) * 2);
        engine.render(out.data(), frames);
        return out;
    }
};

inline i64 peak_abs(const std::vector<i16>& pcm, int channel = -1) {
    i64 peak = 0;
    for (size_t i = channel < 0 ? 0 : static_cast<size_t>(channel); i < pcm.size();
         i += channel < 0 ? 1 : 2) {
        const i64 v = pcm[i] < 0 ? -static_cast<i64>(pcm[i]) : pcm[i];
        if (v > peak) peak = v;
    }
    return peak;
}

inline i64 rms_energy(const std::vector<i16>& pcm) {
    i64 acc = 0;
    for (i16 s : pcm) acc += static_cast<i64>(s) * s;
    return pcm.empty() ? 0 : acc / static_cast<i64>(pcm.size());
}

// Rising zero crossings of the left channel (pitch estimation).
inline int zero_crossings(const std::vector<i16>& pcm) {
    int n = 0;
    for (size_t i = 2; i < pcm.size(); i += 2)
        if (pcm[i - 2] < 0 && pcm[i] >= 0) ++n;
    return n;
}

}  // namespace testfw
