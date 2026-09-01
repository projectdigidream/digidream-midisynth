#include "patch_loader.h"

namespace msynth {
namespace {

u32 ms_q16_to_blocks(i32 ms_q16) {
    if (ms_q16 <= 0) return 0;
    return static_cast<u32>((static_cast<i64>(ms_q16) * CONTROL_RATE / 1000) >> GAIN_SHIFT);
}

CurveId parse_curve(const std::string& s, CurveId def, LoadResult& r, const char* where) {
    if (s.empty()) return def;
    if (s == "linear") return CurveId::Linear;
    if (s == "exp") return CurveId::Exp;
    if (s == "expAttack") return CurveId::ExpAttack;
    if (s == "sCurve") return CurveId::SCurve;
    r.warn(std::string(where) + ": unknown curve '" + s + "', using default");
    return def;
}

// {attack, decay, sustain, release, attackCurve, decayCurve, releaseCurve}
EnvParams parse_env(const JsonValue* v, const EnvParams& def, LoadResult& r, const char* where) {
    if (!v || !v->is_obj()) return def;
    EnvParams e;
    e.att.duration_blocks = ms_q16_to_blocks(v->get_q16("attack", 0));
    e.dec.duration_blocks = ms_q16_to_blocks(v->get_q16("decay", 0));
    e.rel.duration_blocks = ms_q16_to_blocks(v->get_q16("release", 0));
    e.sustain = clamp_gain01(v->get_q16("sustain", GAIN_ONE));
    e.att.curve = parse_curve(v->get_str("attackCurve", ""), CurveId::Linear, r, where);
    e.dec.curve = parse_curve(v->get_str("decayCurve", ""), CurveId::Exp, r, where);
    e.rel.curve = parse_curve(v->get_str("releaseCurve", ""), CurveId::Exp, r, where);
    return e;
}

bool parse_mod_source(const std::string& s, ModSource& out) {
    if (s == "LFO1") out = ModSource::LFO1;
    else if (s == "LFO2") out = ModSource::LFO2;
    else if (s == "AmpEnv") out = ModSource::AmpEnv;
    else if (s == "FilterEnv") out = ModSource::FilterEnv;
    else if (s == "PitchEnv") out = ModSource::PitchEnv;
    else if (s == "WtPosEnv") out = ModSource::WtPosEnv;
    else if (s == "Velocity") out = ModSource::Velocity;
    else if (s == "KeyTrack") out = ModSource::KeyTrack;
    else if (s == "ModWheel") out = ModSource::ModWheel;
    else if (s == "ChannelAftertouch") out = ModSource::ChannelAftertouch;
    else return false;
    return true;
}

bool parse_mod_dest(const std::string& s, ModDest& out) {
    if (s == "Pitch") out = ModDest::Pitch;
    else if (s == "FilterCutoff") out = ModDest::FilterCutoff;
    else if (s == "FilterRes") out = ModDest::FilterRes;
    else if (s == "Amplitude") out = ModDest::Amplitude;
    else if (s == "WtPosition") out = ModDest::WtPosition;
    else if (s == "Pan") out = ModDest::Pan;
    else return false;
    return true;
}

// A plain sine recipe: the substitute for any layer whose wavetable can't be
// built (error policy: never silence).
WaveTablePtr sine_table(WaveTableCache& cache) {
    auto params = std::make_shared<JsonValue>();
    params->type = JsonValue::Type::Object;
    auto one = std::make_shared<JsonValue>();
    one->type = JsonValue::Type::Number;
    one->num_int = 1;
    one->num_q16 = GAIN_ONE;
    params->obj["startHarmonics"] = one;
    params->obj["endHarmonics"] = one;
    auto flat = std::make_shared<JsonValue>();
    flat->type = JsonValue::Type::String;
    flat->str = "flat";
    params->obj["falloff"] = flat;
    std::string err;
    return cache.get_or_create("additiveHarmonicMorph", 1, params.get(), &err);
}

void parse_layer(const JsonValue& v, WaveTableCache& cache, LayerDef& l, LoadResult& r,
                 const std::string& where) {
    const std::string osc = v.get_str("osc", "wavetable");
    if (osc == "noise") {
        l.osc = OscType::Noise;
    } else {
        l.osc = OscType::Wavetable;
        const JsonValue* wt = v.get("wavetable");
        if (wt && wt->is_obj()) {
            const std::string gen = wt->get_str("generator", "additiveHarmonicMorph");
            const int frames = static_cast<int>(wt->get_int("frameCount", 1));
            std::string err;
            l.wavetable = cache.get_or_create(gen, frames, wt->get("params"), &err);
            if (!l.wavetable) {
                r.warn(where + ": " + err + "; substituting sine");
                l.wavetable = sine_table(cache);
            }
        } else {
            r.warn(where + ": missing wavetable; substituting sine");
            l.wavetable = sine_table(cache);
        }
    }

    l.detune_cents_q16 = v.get_q16("detuneCents", 0);
    l.mix_gain = clamp_gain01(v.get_q16("mixGain", GAIN_ONE));
    l.vel_low = static_cast<u8>(clamp_i32(v.get_int("velLow", 0), 0, 127));
    l.vel_high = static_cast<u8>(clamp_i32(v.get_int("velHigh", 127), 0, 127));
    if (l.vel_low > l.vel_high) {
        r.warn(where + ": velLow > velHigh, swapping");
        const u8 t = l.vel_low;
        l.vel_low = l.vel_high;
        l.vel_high = t;
    }
    l.pan_q16 = clamp_i32(v.get_q16("pan", 0), -GAIN_ONE, GAIN_ONE);

    const std::string vc = v.get_str("velCurve", "squared");
    if (vc == "squared") l.vel_curve = VelCurve::Squared;
    else if (vc == "linear") l.vel_curve = VelCurve::Linear;
    else if (vc == "soft") l.vel_curve = VelCurve::Soft;
    else {
        r.warn(where + ": unknown velCurve '" + vc + "', using squared");
        l.vel_curve = VelCurve::Squared;
    }

    const i32 pitch_note_q16 = v.get_q16("pitchNote", -GAIN_ONE);
    l.fixed_pitch_cents_q16 =
        pitch_note_q16 >= 0 ? static_cast<i32>(static_cast<i64>(pitch_note_q16) * 100) : -1;

    EnvParams amp_def;
    amp_def.att.duration_blocks = 1;
    amp_def.rel.duration_blocks = ms_q16_to_blocks(50 << GAIN_SHIFT);
    l.amp_env = parse_env(v.get("ampEnv"), amp_def, r, "ampEnv");
    l.filter_env = parse_env(v.get("filterEnv"), EnvParams{}, r, "filterEnv");
    l.pitch_env = parse_env(v.get("pitchEnv"), EnvParams{}, r, "pitchEnv");
    l.wt_env = parse_env(v.get("wtPosEnv"), EnvParams{}, r, "wtPosEnv");

    const JsonValue* fenv = v.get("filterEnv");
    l.filter_env_depth = fenv ? clamp_i32(fenv->get_q16("depth", GAIN_ONE / 2),
                                          -GAIN_ONE, GAIN_ONE)
                              : 0;
    const JsonValue* penv = v.get("pitchEnv");
    l.pitch_env_cents_q16 = penv ? static_cast<i32>(clamp_i32(
        static_cast<i64>(penv->get_q16("depthSemis", 0)) * 100,
        -(4800LL << GAIN_SHIFT), 4800LL << GAIN_SHIFT)) : 0;
    const JsonValue* wenv = v.get("wtPosEnv");
    l.wt_env_depth = wenv ? clamp_gain01(wenv->get_q16("depth", GAIN_ONE)) : 0;

    // Filter block; omitted or mode "bypass" = no filter stage at all.
    const JsonValue* f = v.get("filter");
    if (f && f->is_obj()) {
        const std::string mode = f->get_str("mode", "lowpass");
        if (mode == "lowpass") l.filter_mode = FilterMode::Lowpass;
        else if (mode == "bandpass") l.filter_mode = FilterMode::Bandpass;
        else if (mode == "highpass") l.filter_mode = FilterMode::Highpass;
        else if (mode == "bypass") l.filter_mode = FilterMode::Bypass;
        else {
            r.warn(where + ": unknown filter mode '" + mode + "', bypassing");
            l.filter_mode = FilterMode::Bypass;
        }
        i32 hz = f->get_q16("cutoffHz", 1000 << GAIN_SHIFT);
        if (hz > (12000 << GAIN_SHIFT)) {
            r.warn(where + ": cutoffHz above the 12kHz cap, clamping (consider bypass)");
            hz = 12000 << GAIN_SHIFT;
        }
        l.cutoff_index_q16 = cutoff_index_from_hz(hz);
        i32 res = f->get_q16("resonance", 0);
        const i32 max_res = (95 * GAIN_ONE) / 100;
        if (res > max_res) {
            r.warn(where + ": resonance above self-oscillation cap, clamping to 0.95");
            res = max_res;
        }
        l.resonance_q16 = clamp_gain01(res);
    } else {
        l.filter_mode = FilterMode::Bypass;
    }
    l.key_track_filter = clamp_gain01(v.get_q16("keyTrackFilter", 0));

    // LFOs.
    const JsonValue* lfos = v.get("lfos");
    l.lfo_count = 0;
    if (lfos && lfos->is_arr()) {
        for (const auto& lv : lfos->arr) {
            if (l.lfo_count >= MAX_LFOS) {
                r.warn(where + ": too many LFOs, extras dropped");
                break;
            }
            LfoParams& p = l.lfos[l.lfo_count];
            const std::string shape = lv->get_str("shape", "sine");
            if (shape == "sine") p.shape = LfoShape::Sine;
            else if (shape == "triangle") p.shape = LfoShape::Triangle;
            else if (shape == "saw") p.shape = LfoShape::Saw;
            else if (shape == "square") p.shape = LfoShape::Square;
            else if (shape == "sampleHold") p.shape = LfoShape::SampleHold;
            else {
                r.warn(where + ": unknown LFO shape '" + shape + "', using sine");
                p.shape = LfoShape::Sine;
            }
            i32 rate = clamp_i32(lv->get_q16("rateHz", 5 << GAIN_SHIFT), 0, 40 << GAIN_SHIFT);
            p.inc_per_block =
                static_cast<u32>((static_cast<u64>(static_cast<u32>(rate)) << 16) / CONTROL_RATE);
            p.delay_blocks = ms_q16_to_blocks(lv->get_q16("delayMs", 0));
            ++l.lfo_count;
        }
    }

    // Mod matrix.
    const JsonValue* mm = v.get("modMatrix");
    l.routing_count = 0;
    if (mm && mm->is_arr()) {
        for (const auto& rv : mm->arr) {
            if (l.routing_count >= MAX_ROUTINGS) {
                r.warn(where + ": too many mod routings, extras dropped");
                break;
            }
            ModRouting& mr = l.routings[l.routing_count];
            if (!parse_mod_source(rv->get_str("src", ""), mr.src) ||
                !parse_mod_dest(rv->get_str("dst", ""), mr.dst)) {
                r.warn(where + ": unknown mod src/dst, routing dropped");
                continue;
            }
            mr.amount = clamp_i32(rv->get_q16("amount", 0), -8 * GAIN_ONE, 8 * GAIN_ONE);
            ++l.routing_count;
        }
    }
}

}  // namespace

LoadResult load_patch(const JsonValue& doc, WaveTableCache& cache, Patch& out) {
    LoadResult r;
    out = Patch{};
    out.name = doc.get_str("name", "unnamed");

    out.one_shot = doc.get_bool("oneShot", false);
    out.exclusive_group =
        static_cast<i8>(clamp_i32(doc.get_int("exclusiveGroup", -1), -1, 15));

    const JsonValue* layers = doc.get("layers");
    if (!layers || !layers->is_arr() || layers->arr.empty()) {
        r.error("patch '" + out.name + "': no layers");
        return r;
    }
    for (const auto& lv : layers->arr) {
        if (out.layer_count >= MAX_LAYERS) {
            r.warn("patch '" + out.name + "': more than " + std::to_string(MAX_LAYERS) +
                   " layers, extras dropped");
            break;
        }
        LayerDef& l = out.layers[out.layer_count];
        parse_layer(*lv, cache, l, r, "patch '" + out.name + "'");
        if (out.one_shot) {
            l.amp_env.one_shot = true;
            if (l.amp_env.sustain != 0) {
                // Rule 5: a one-shot whose decay lands on nonzero sustain
                // would never free its voice.
                r.warn("patch '" + out.name + "': oneShot with nonzero sustain, clamped to 0");
                l.amp_env.sustain = 0;
            }
        }
        ++out.layer_count;
    }
    return r;
}

void make_builtin_fallback(WaveTableCache& cache, Patch& out) {
    // A soft additive pad: something musical for every unauthored program.
    static const char* kJson = R"({
      "name": "fallback_pad",
      "layers": [ {
        "wavetable": { "generator": "additiveHarmonicMorph", "frameCount": 8,
                       "params": { "startHarmonics": 3, "endHarmonics": 8, "falloff": "saw" } },
        "mixGain": 0.55,
        "ampEnv": { "attack": 12, "decay": 300, "sustain": 0.75, "release": 250 },
        "wtPosEnv": { "attack": 400, "decay": 800, "sustain": 0.4, "release": 300, "depth": 0.8 },
        "filter": { "mode": "lowpass", "cutoffHz": 3200, "resonance": 0.15 },
        "keyTrackFilter": 0.6
      } ]
    })";
    std::string err;
    JsonPtr doc = json_parse(kJson, std::char_traits<char>::length(kJson), &err);
    if (doc) load_patch(*doc, cache, out);
}

LoadResult load_patch_bank(const char* json, size_t len, WaveTableCache& cache, PatchBank& out) {
    LoadResult r;
    std::string err;
    JsonPtr doc = json_parse(json, len, &err);
    if (!doc || !doc->is_obj()) {
        r.error("bank: JSON parse failed: " + err);
        make_builtin_fallback(cache, out.fallback());
        return r;
    }
    if (doc->get_int("version", 1) != 1) r.warn("bank: unknown schema version, best-effort load");

    const JsonValue* fb = doc->get("fallback");
    if (fb && fb->is_obj()) {
        LoadResult fr = load_patch(*fb, cache, out.fallback());
        for (auto& w : fr.warnings) r.warn(w);
        if (!fr.ok) {
            for (auto& e : fr.errors) r.warn("fallback: " + e + "; using built-in");
            make_builtin_fallback(cache, out.fallback());
        }
    } else {
        make_builtin_fallback(cache, out.fallback());
    }

    const JsonValue* progs = doc->get("programs");
    if (progs && progs->is_obj()) {
        for (const auto& kv : progs->obj) {
            char* endp = nullptr;
            const long num = std::strtol(kv.first.c_str(), &endp, 10);
            if (!endp || *endp != '\0' || num < 0 || num > 127) {
                r.warn("bank: bad program key '" + kv.first + "', skipped");
                continue;
            }
            auto patch = std::make_unique<Patch>();
            LoadResult pr = load_patch(*kv.second, cache, *patch);
            for (auto& w : pr.warnings) r.warn(w);
            if (!pr.ok) {
                for (auto& e : pr.errors) r.warn("program " + kv.first + ": " + e +
                                                 "; falling back");
                continue;  // lookup() serves the fallback for this program
            }
            out.set_program(static_cast<u8>(num), std::move(patch));
        }
    }
    return r;
}

LoadResult load_drum_kit(const char* json, size_t len, WaveTableCache& cache, DrumKit& out) {
    LoadResult r;
    std::string err;
    JsonPtr doc = json_parse(json, len, &err);
    if (!doc || !doc->is_obj()) {
        r.error("drumkit: JSON parse failed: " + err);
        return r;
    }
    if (doc->get_int("version", 1) != 1) r.warn("drumkit: unknown schema version");

    const JsonValue* notes = doc->get("notes");
    if (!notes || !notes->is_obj()) {
        r.error("drumkit: missing 'notes'");
        return r;
    }
    for (const auto& kv : notes->obj) {
        char* endp = nullptr;
        const long num = std::strtol(kv.first.c_str(), &endp, 10);
        if (!endp || *endp != '\0' || num < 0 || num > 127) {
            r.warn("drumkit: bad note key '" + kv.first + "', skipped");
            continue;
        }
        auto patch = std::make_unique<Patch>();
        LoadResult pr = load_patch(*kv.second, cache, *patch);
        for (auto& w : pr.warnings) r.warn(w);
        if (!pr.ok) {
            for (auto& e : pr.errors) r.warn("drum note " + kv.first + ": " + e + "; skipped");
            continue;
        }
        out.set_note(static_cast<u8>(num), std::move(patch));
    }
    return r;
}

}  // namespace msynth
