//
// Patch loader: schema coverage, clamps-with-warning, never-silence
// substitution (plan §11/§13).
//
#include "test_framework.h"

#include "synth/patch_loader.h"

using namespace msynth;

namespace {
LoadResult load_from(const char* json, WaveTableCache& cache, Patch& out) {
    std::string err;
    JsonPtr doc = json_parse(json, std::strlen(json), &err);
    CHECK(doc != nullptr);
    return load_patch(*doc, cache, out);
}
}  // namespace

TEST(loader_full_plan_example_patch) {
    // The §13 schema example, verbatim in structure.
    static const char* kJson = R"({
      "name": "warm_pad",
      "layers": [ {
        "wavetable": { "generator": "additiveHarmonicMorph", "frameCount": 32,
                       "params": { "startHarmonics": 1, "endHarmonics": 24, "falloff": "saw" } },
        "detuneCents": -7,
        "mixGain": 0.5,
        "velLow": 0, "velHigh": 127,
        "pan": 0.0,
        "ampEnv":    { "attack": 5,  "decay": 200, "sustain": 0.7, "release": 300,
                       "attackCurve": "linear", "decayCurve": "exp", "releaseCurve": "exp" },
        "filterEnv": { "attack": 0,  "decay": 400, "sustain": 0.2, "release": 200 },
        "pitchEnv":  { "attack": 0,  "decay": 60,  "sustain": 0.0, "release": 0, "depthSemis": 0 },
        "wtPosEnv":  { "attack": 200, "decay": 500, "sustain": 0.5, "release": 400 },
        "filter": { "mode": "lowpass", "cutoffHz": 1200, "resonance": 0.3 },
        "keyTrackFilter": 0.5,
        "lfos": [ { "shape": "sine", "rateHz": 5.0, "delayMs": 300 } ],
        "modMatrix": [ { "src": "LFO1", "dst": "Pitch", "amount": 0.02 } ]
      } ]
    })";
    WaveTableCache cache;
    Patch p;
    LoadResult r = load_from(kJson, cache, p);
    CHECK(r.ok);
    CHECK(r.warnings.empty());
    CHECK(p.name == "warm_pad");
    CHECK_EQ(p.layer_count, 1);
    const LayerDef& l = p.layers[0];
    CHECK(l.wavetable != nullptr);
    CHECK_EQ(l.wavetable->frameCount, 32);
    CHECK_EQ(l.detune_cents_q16, -7 * GAIN_ONE);
    CHECK_EQ(l.mix_gain, GAIN_ONE / 2);
    // 5 ms attack at 750 blocks/s = 3 blocks (floored).
    CHECK_EQ(l.amp_env.att.duration_blocks, 3);
    CHECK_EQ(l.amp_env.dec.duration_blocks, 150);
    CHECK_EQ(l.amp_env.sustain, (7 * GAIN_ONE) / 10);
    CHECK(l.filter_mode == FilterMode::Lowpass);
    CHECK(l.cutoff_index_q16 > 0);
    CHECK_EQ(l.resonance_q16, (3 * GAIN_ONE) / 10);
    CHECK_EQ(l.key_track_filter, GAIN_ONE / 2);
    CHECK_EQ(l.lfo_count, 1);
    CHECK_EQ(l.lfos[0].delay_blocks, 225);  // 300 ms
    CHECK_EQ(l.routing_count, 1);
    CHECK(l.routings[0].src == ModSource::LFO1);
    CHECK(l.routings[0].dst == ModDest::Pitch);
    CHECK_NEAR(l.routings[0].amount, 1310, 2);  // 0.02
    // filterEnv present without explicit depth -> default 0.5.
    CHECK_EQ(l.filter_env_depth, GAIN_ONE / 2);
}

TEST(loader_resonance_clamped_with_warning) {
    WaveTableCache cache;
    Patch p;
    LoadResult r = load_from(R"({"name":"x","layers":[{
        "filter": {"mode":"lowpass", "cutoffHz": 500, "resonance": 1.2}}]})", cache, p);
    CHECK(r.ok);
    CHECK(!r.warnings.empty());
    CHECK_EQ(p.layers[0].resonance_q16, (95 * GAIN_ONE) / 100);
}

TEST(loader_cutoff_capped_with_warning) {
    WaveTableCache cache;
    Patch p;
    LoadResult r = load_from(R"({"name":"x","layers":[{
        "filter": {"mode":"lowpass", "cutoffHz": 20000}}]})", cache, p);
    CHECK(r.ok);
    bool warned = false;
    for (auto& w : r.warnings)
        if (w.find("12kHz") != std::string::npos) warned = true;
    CHECK(warned);
    CHECK(p.layers[0].cutoff_index_q16 <= (256 << GAIN_SHIFT));
}

TEST(loader_missing_filter_is_bypass) {
    WaveTableCache cache;
    Patch p;
    LoadResult r = load_from(R"({"name":"x","layers":[{}]})", cache, p);
    CHECK(r.ok);
    CHECK(p.layers[0].filter_mode == FilterMode::Bypass);
    // And the missing wavetable substituted a sine, with a warning.
    CHECK(p.layers[0].wavetable != nullptr);
    CHECK(!r.warnings.empty());
}

TEST(loader_oneshot_sustain_clamped) {
    // Rule 5: oneShot + nonzero sustain would leak the voice forever.
    WaveTableCache cache;
    Patch p;
    LoadResult r = load_from(R"({"name":"kick","oneShot":true,"exclusiveGroup":2,"layers":[{
        "ampEnv": {"attack":0,"decay":100,"sustain":0.5,"release":0}}]})", cache, p);
    CHECK(r.ok);
    CHECK(!r.warnings.empty());
    CHECK(p.one_shot);
    CHECK_EQ(p.exclusive_group, 2);
    CHECK(p.layers[0].amp_env.one_shot);
    CHECK_EQ(p.layers[0].amp_env.sustain, 0);
}

TEST(loader_noise_layer) {
    WaveTableCache cache;
    Patch p;
    LoadResult r = load_from(R"({"name":"hat","layers":[{
        "osc": "noise",
        "filter": {"mode":"highpass","cutoffHz":8000}}]})", cache, p);
    CHECK(r.ok);
    CHECK(p.layers[0].osc == OscType::Noise);
    CHECK(p.layers[0].filter_mode == FilterMode::Highpass);
}

TEST(loader_pitch_note_fixed) {
    WaveTableCache cache;
    Patch p;
    LoadResult r = load_from(R"({"name":"kick","layers":[{"pitchNote": 36.5}]})", cache, p);
    CHECK(r.ok);
    CHECK_EQ(p.layers[0].fixed_pitch_cents_q16, 3650 * GAIN_ONE);
}

TEST(loader_vel_range_swap_and_layers_cap) {
    WaveTableCache cache;
    Patch p;
    LoadResult r = load_from(R"({"name":"x","layers":[
        {"velLow": 100, "velHigh": 20}, {}, {}, {}, {}, {}]})", cache, p);
    CHECK(r.ok);
    CHECK(!r.warnings.empty());
    CHECK_EQ(p.layers[0].vel_low, 20);
    CHECK_EQ(p.layers[0].vel_high, 100);
    CHECK_EQ(p.layer_count, MAX_LAYERS);
}

TEST(loader_no_layers_is_error) {
    WaveTableCache cache;
    Patch p;
    LoadResult r = load_from(R"({"name":"empty","layers":[]})", cache, p);
    CHECK(!r.ok);
}

TEST(loader_unknown_mod_routing_dropped) {
    WaveTableCache cache;
    Patch p;
    LoadResult r = load_from(R"({"name":"x","layers":[{
        "modMatrix": [ {"src":"LFO9","dst":"Pitch","amount":1},
                       {"src":"LFO1","dst":"Pitch","amount":0.5} ]}]})", cache, p);
    CHECK(r.ok);
    CHECK(!r.warnings.empty());
    CHECK_EQ(p.layers[0].routing_count, 1);
}

TEST(loader_builtin_fallback_is_usable) {
    WaveTableCache cache;
    Patch p;
    make_builtin_fallback(cache, p);
    CHECK_EQ(p.layer_count, 1);
    CHECK(p.layers[0].wavetable != nullptr);
    CHECK(p.layers[0].amp_env.att.duration_blocks > 0);
}

TEST(loader_bank_with_fallback_and_programs) {
    static const char* kBank = R"({
      "version": 1,
      "programs": {
        "0": {"name":"piano","layers":[{}]},
        "48": {"name":"strings","layers":[{}]},
        "300": {"name":"bad-key","layers":[{}]},
        "5": {"name":"broken","layers":[]}
      }
    })";
    WaveTableCache cache;
    PatchBank bank;
    LoadResult r = load_patch_bank(kBank, std::strlen(kBank), cache, bank);
    CHECK(r.ok);
    CHECK(!r.warnings.empty());  // bad key + broken patch
    CHECK_EQ(bank.patch_count(), 2);
    CHECK(bank.lookup(0)->name == "piano");
    CHECK(bank.lookup(48)->name == "strings");
    // Unknown & broken programs resolve to the fallback -- never null.
    CHECK(bank.lookup(5) == bank.lookup(77));
    CHECK(bank.lookup(5)->layer_count > 0);
}

TEST(loader_bank_bad_json_still_yields_fallback) {
    WaveTableCache cache;
    PatchBank bank;
    LoadResult r = load_patch_bank("{{{{", 4, cache, bank);
    CHECK(!r.ok);
    CHECK(bank.lookup(0) != nullptr);
    CHECK(bank.lookup(0)->layer_count > 0);  // built-in fallback installed
}

TEST(loader_drum_kit) {
    static const char* kKit = R"({
      "version": 1,
      "notes": {
        "36": {"name":"kick","oneShot":true,"layers":[{"pitchNote":30}]},
        "42": {"name":"chh","oneShot":true,"exclusiveGroup":1,"layers":[{"osc":"noise"}]},
        "46": {"name":"ohh","oneShot":true,"exclusiveGroup":1,"layers":[{"osc":"noise"}]}
      }
    })";
    WaveTableCache cache;
    DrumKit kit;
    LoadResult r = load_drum_kit(kKit, std::strlen(kKit), cache, kit);
    CHECK(r.ok);
    CHECK_EQ(kit.note_count(), 3);
    CHECK(kit.lookup(36) != nullptr);
    CHECK(kit.lookup(36)->one_shot);
    CHECK_EQ(kit.lookup(42)->exclusive_group, 1);
    CHECK(kit.lookup(37) == nullptr);  // unmapped: silent by design
}

TEST(loader_wavetables_deduped_across_patches) {
    WaveTableCache cache;
    Patch a, b;
    const char* json = R"({"name":"x","layers":[{
        "wavetable": {"generator":"additiveHarmonicMorph","frameCount":4,
                      "params":{"startHarmonics":1,"endHarmonics":8,"falloff":"saw"}}}]})";
    load_from(json, cache, a);
    load_from(json, cache, b);
    CHECK(a.layers[0].wavetable.get() == b.layers[0].wavetable.get());
}
