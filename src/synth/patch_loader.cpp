#include "patch_loader.h"

namespace msynth {

// Shared with fx.json's delay (declared in patch_loader.h): "1/4", "1/8T"
// (triplet, x2/3), "1/8." (dotted, x3/2), "2/1" -> beats, Q16.
i32 parse_sync_beats(const std::string& s, LoadResult& r, const std::string& where) {
    if (s.empty()) return 0;
    int num = 0, den = 0;
    size_t i = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') num = num * 10 + (s[i++] - '0');
    if (i >= s.size() || s[i] != '/') { r.warn(where + ": bad sync '" + s + "'"); return 0; }
    ++i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') den = den * 10 + (s[i++] - '0');
    const bool triplet = i < s.size() && (s[i] == 'T' || s[i] == 't');
    const bool dotted = i < s.size() && s[i] == '.';
    if (num < 1 || den < 1 || den > 64) { r.warn(where + ": bad sync '" + s + "'"); return 0; }
    // whole note = 4 beats; N/D of a whole note.
    i64 beats = (static_cast<i64>(num) * 4 << GAIN_SHIFT) / den;
    if (triplet) beats = beats * 2 / 3;
    else if (dotted) beats = beats * 3 / 2;
    return static_cast<i32>(clamp_i32(beats, GAIN_ONE / 16, 64 * GAIN_ONE));
}

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
// The engine's absolute rate anchor (DX7-style R = 0..99):
//   t_ms(R) = 40000 * 2^(-R / 6.5)
// R=0 -> 40 s, R=50 -> ~187 ms, R=99 -> ~1.03 ms; each +6.5 halves the time.
// Deterministic via the baked exp2 table.
u32 rate_to_blocks(i64 rate) {
    rate = clamp_i32(rate, 0, 99);
    // 2^(R/6.5) = 2^(2R/13), Q16 argument.
    const gain_t div = exp2_q16(static_cast<i32>((rate << (GAIN_SHIFT + 1)) / 13));
    const i64 ms_q16 = (static_cast<i64>(40000) << (GAIN_SHIFT + GAIN_SHIFT)) / div;
    // Straight to blocks in i64: R=0 is 40 s, whose Q16 ms overflows i32.
    return static_cast<u32>((ms_q16 * CONTROL_RATE / 1000) >> GAIN_SHIFT);
}

EnvParams parse_env(const JsonValue* v, const EnvParams& def, LoadResult& r, const char* where) {
    if (!v || !v->is_obj()) return def;
    EnvParams e;
    // Stage times: milliseconds, or a DX7-style rate 0..99 (see
    // rate_to_blocks for the anchor). Explicit ms wins when both appear.
    auto stage_blocks = [&](const char* ms_key, const char* rate_key) -> u32 {
        if (v->get(ms_key)) return ms_q16_to_blocks(v->get_q16(ms_key, 0));
        const JsonValue* rv = v->get(rate_key);
        if (rv && rv->is_num()) return rate_to_blocks(rv->num_int);
        return 0;
    };
    e.del.duration_blocks = ms_q16_to_blocks(v->get_q16("delay", 0));
    e.att.duration_blocks = stage_blocks("attack", "attackRate");
    e.hold.duration_blocks = ms_q16_to_blocks(v->get_q16("hold", 0));
    e.dec.duration_blocks = stage_blocks("decay", "decayRate");
    e.rel.duration_blocks = stage_blocks("release", "releaseRate");
    e.key_scale_rate_q16 = clamp_i32(v->get_q16("keyScaleRate", 0), -GAIN_ONE, GAIN_ONE);
    e.sustain = clamp_gain01(v->get_q16("sustain", GAIN_ONE));
    e.loop = v->get_bool("loop", false);
    if (e.loop && e.att.duration_blocks + e.dec.duration_blocks == 0) {
        r.warn(std::string(where) + ": loop with instant attack+decay ignored");
        e.loop = false;
    }
    e.att.curve = parse_curve(v->get_str("attackCurve", ""), CurveId::Linear, r, where);
    e.dec.curve = parse_curve(v->get_str("decayCurve", ""), CurveId::Exp, r, where);
    e.rel.curve = parse_curve(v->get_str("releaseCurve", ""), CurveId::Exp, r, where);
    return e;
}

bool parse_mod_source(const std::string& s, ModSource& out) {
    if (s == "LFO1") out = ModSource::LFO1;
    else if (s == "LFO2") out = ModSource::LFO2;
    else if (s == "LFO3") out = ModSource::LFO3;
    else if (s == "LFO4") out = ModSource::LFO4;
    else if (s == "PolyAftertouch") out = ModSource::PolyAftertouch;
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
    else if (s == "FilterDrive") out = ModDest::FilterDrive;
    else if (s == "PostDrive") out = ModDest::PostDrive;
    else if (s == "RingAmount") out = ModDest::RingAmount;
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
        const std::string col = v.get_str("noiseColor", "white");
        if (col == "pink") l.noise_color = NoiseColor::Pink;
        else if (col == "brown") l.noise_color = NoiseColor::Brown;
        else if (col != "white") r.warn(where + ": unknown noiseColor '" + col + "', using white");
    } else if (osc == "metal") {
        l.osc = OscType::Metal;
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
        else if (mode == "notch") l.filter_mode = FilterMode::Notch;
        else if (mode == "comb") l.filter_mode = FilterMode::Comb;
        else if (mode == "formant") l.filter_mode = FilterMode::Formant;
        else if (mode == "bypass") l.filter_mode = FilterMode::Bypass;
        else {
            r.warn(where + ": unknown filter mode '" + mode + "', bypassing");
            l.filter_mode = FilterMode::Bypass;
        }
        if (l.filter_mode == FilterMode::Formant) {
            // Male-ish F1/F2/F3 per vowel; cutoffHz rescales them (1000 =
            // neutral), the filter envelope morphs vowel -> endVowel.
            static const struct { const char* name; int f[3]; } VOWELS[] = {
                {"a", {800, 1150, 2900}}, {"e", {400, 1600, 2700}},
                {"i", {250, 1750, 3400}}, {"o", {400, 800, 2600}},
                {"u", {350, 600, 2700}},
            };
            auto vowel_of = [&](const std::string& name, int def_idx) {
                for (int i = 0; i < 5; ++i)
                    if (name == VOWELS[i].name) return i;
                if (!name.empty()) r.warn(where + ": unknown vowel '" + name + "', using 'a'");
                return def_idx;
            };
            const int v0 = vowel_of(f->get_str("vowel", "a"), 0);
            const int v1 = vowel_of(f->get_str("endVowel", ""), v0);
            // cutoffHz acts as a formant scale: 1000 Hz = neutral.
            const i32 scale_hz = clamp_i32(f->get_q16("cutoffHz", 1000 << GAIN_SHIFT),
                                           250 << GAIN_SHIFT, 4000 << GAIN_SHIFT);
            const int pair[2] = {v0, v1};
            for (int s = 0; s < 2; ++s)
                for (int k = 0; k < 3; ++k) {
                    const i64 hz = static_cast<i64>(VOWELS[pair[s]].f[k]) * scale_hz / 1000;
                    l.formant_idx_q16[s][k] = cutoff_index_from_hz(
                        static_cast<i32>(clamp_i32(hz, 20LL << GAIN_SHIFT, 12000LL << GAIN_SHIFT)));
                }
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
        // Formant mode wants sharp peaks: default the Q high when unset.
        if (l.filter_mode == FilterMode::Formant && f->get("resonance") == nullptr)
            l.resonance_q16 = (85 * GAIN_ONE) / 100;
        const i64 slope = f->get_int("slope", 12);
        if (slope == 24) l.filter_24db = true;
        else if (slope != 12) r.warn(where + ": filter slope must be 12 or 24, using 12");
        i32 drive = f->get_q16("drive", GAIN_ONE);
        if (drive < GAIN_ONE || drive > 8 * GAIN_ONE) {
            r.warn(where + ": filter drive outside 1..8, clamped");
            drive = clamp_i32(drive, GAIN_ONE, 8 * GAIN_ONE);
        }
        l.filter_drive_q16 = drive;
    } else {
        l.filter_mode = FilterMode::Bypass;
    }
    l.key_track_filter = clamp_gain01(v.get_q16("keyTrackFilter", 0));
    i32 delay_ms = v.get_q16("startDelayMs", 0);
    if (delay_ms < 0 || delay_ms > (2000 << GAIN_SHIFT)) {
        r.warn(where + ": startDelayMs outside 0..2000, clamped");
        delay_ms = clamp_i32(delay_ms, 0, 2000 << GAIN_SHIFT);
    }
    // ms -> control blocks (750/s), floor like every other time value.
    l.start_delay_blocks = static_cast<i32>((static_cast<i64>(delay_ms) * CONTROL_RATE / 1000) >> GAIN_SHIFT);

    // Post-filter saturation (deliberately not band-limited).
    i32 post = v.get_q16("postDrive", GAIN_ONE);
    if (post < GAIN_ONE || post > 8 * GAIN_ONE) {
        r.warn(where + ": postDrive outside 1..8, clamped");
        post = clamp_i32(post, GAIN_ONE, 8 * GAIN_ONE);
    }
    l.post_drive_q16 = post;

    // Unison stack.
    const JsonValue* uni = v.get("unison");
    if (uni && uni->is_obj()) {
        l.unison_voices = static_cast<u8>(clamp_i32(uni->get_int("voices", 1), 1, 7));
        l.unison_detune_cents_q16 = clamp_i32(uni->get_q16("detuneCents", 7 << GAIN_SHIFT),
                                              0, 100 << GAIN_SHIFT);
        l.unison_spread_q16 = clamp_gain01(uni->get_q16("spread", GAIN_ONE / 2));
        l.unison_phase_spread_q16 = clamp_gain01(uni->get_q16("phaseSpread", 0));
    }

    // Ring modulation against an internal carrier at ratio x the pitch.
    const JsonValue* ring = v.get("ring");
    if (ring && ring->is_obj()) {
        l.ring_ratio_q16 = clamp_i32(ring->get_q16("ratio", GAIN_ONE / 2), GAIN_ONE / 4, 16 * GAIN_ONE);
        const std::string mode = ring->get_str("mode", "ring");
        l.ring_is_fm = mode == "fm";
        if (!l.ring_is_fm && mode != "ring")
            r.warn(where + ": unknown ring mode '" + mode + "', using ring");
        l.ring_amount_q16 = l.ring_is_fm
            ? clamp_i32(ring->get_q16("index", GAIN_ONE), 0, 12 * GAIN_ONE)
            : clamp_gain01(ring->get_q16("amount", GAIN_ONE));
        if (l.ring_amount_q16 == 0 && !l.ring_is_fm) l.ring_ratio_q16 = 0;
    }

    // Independent second filter (serial by default; "filterRouting": "parallel").
    const JsonValue* f2 = v.get("filter2");
    if (f2 && f2->is_obj()) {
        const std::string m2 = f2->get_str("mode", "highpass");
        if (m2 == "lowpass") l.filter2_mode = FilterMode::Lowpass;
        else if (m2 == "bandpass") l.filter2_mode = FilterMode::Bandpass;
        else if (m2 == "highpass") l.filter2_mode = FilterMode::Highpass;
        else if (m2 == "notch") l.filter2_mode = FilterMode::Notch;
        else if (m2 == "comb") l.filter2_mode = FilterMode::Comb;
        else if (m2 == "formant") l.filter2_mode = FilterMode::Formant;
        else if (m2 == "bypass") l.filter2_mode = FilterMode::Bypass;
        else {
            r.warn(where + ": unknown filter2 mode '" + m2 + "', bypassing");
            l.filter2_mode = FilterMode::Bypass;
        }
        if (l.filter2_mode == FilterMode::Comb || l.filter2_mode == FilterMode::Formant) {
            r.warn(where + ": filter2 cannot be comb/formant, bypassing");
            l.filter2_mode = FilterMode::Bypass;
        }
        i32 hz2 = f2->get_q16("cutoffHz", 200 << GAIN_SHIFT);
        if (hz2 > (12000 << GAIN_SHIFT)) {
            r.warn(where + ": filter2 cutoffHz above the 12kHz cap, clamping");
            hz2 = 12000 << GAIN_SHIFT;
        }
        l.filter2_cutoff_index_q16 = cutoff_index_from_hz(hz2);
        l.filter2_resonance_q16 = clamp_i32(f2->get_q16("resonance", 0), 0, (95 * GAIN_ONE) / 100);
        l.filter2_static = f2->get_bool("static", false);
        if (l.filter2_mode != FilterMode::Bypass && l.filter_24db) {
            r.warn(where + ": filter2 replaces the 24 dB cascade (slope ignored)");
        }
    }
    if (l.filter_mode == FilterMode::Formant && l.filter2_mode != FilterMode::Bypass) {
        r.warn(where + ": formant mode uses all filter stages; filter2 bypassed");
        l.filter2_mode = FilterMode::Bypass;
    }
    const std::string routing = v.get_str("filterRouting", "serial");
    if (routing == "parallel") l.filter_parallel = true;
    else if (routing != "serial") r.warn(where + ": filterRouting must be serial or parallel");

    // Release velocity -> release-time scaling.
    l.release_vel_amount_q16 = clamp_gain01(v.get_q16("releaseVel", 0));

    // Round robin group id (resolved into index/size after all layers load).
    l.rr_group = static_cast<u8>(clamp_i32(v.get_int("rrGroup", 0), 0, 15));

    // Per-layer send overrides (-1 = inherit the channel CC).
    auto send_field = [&](const char* key) -> i32 {
        const JsonValue* s = v.get(key);
        if (!s) return -1;
        if (!s->is_num()) {
            r.warn(where + ": " + key + " must be a number 0..1");
            return -1;
        }
        return clamp_gain01(s->num_q16);
    };
    l.send_reverb_q16 = send_field("sendReverb");
    l.send_chorus_q16 = send_field("sendChorus");
    l.send_delay_q16 = send_field("sendDelay");

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
            p.sync_beats_q16 = parse_sync_beats(lv->get_str("sync", ""), r, where);
            p.retrigger = lv->get_bool("retrigger", true);
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
    // Resolve round-robin groups: layers sharing rrGroup alternate 1-by-1.
    for (int g = 1; g <= 15; ++g) {
        u8 size = 0;
        for (u8 i = 0; i < out.layer_count; ++i)
            if (out.layers[i].rr_group == g) out.layers[i].rr_index = size++;
        if (size > 1)
            for (u8 i = 0; i < out.layer_count; ++i)
                if (out.layers[i].rr_group == g) out.layers[i].rr_size = size;
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
