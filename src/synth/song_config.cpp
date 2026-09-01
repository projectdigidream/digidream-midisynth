#include "song_config.h"

namespace msynth {

namespace {
gain_t get_clamped(const JsonValue* obj, const char* key, gain_t def, gain_t lo, gain_t hi,
                   LoadResult& r, const char* where) {
    if (!obj) return def;
    const JsonValue* v = obj->get(key);
    if (!v) return def;
    if (!v->is_num()) {
        r.warn(std::string(where) + "." + key + ": not a number, using default");
        return def;
    }
    const i32 q = v->num_q16;
    if (q < lo || q > hi) {
        r.warn(std::string(where) + "." + key + ": out of range, clamped");
        return clamp_i32(q, lo, hi);
    }
    return q;
}
}  // namespace

LoadResult load_song_config(const char* json, size_t len, SongFxConfig& out) {
    LoadResult r;
    out = SongFxConfig{};  // engine defaults
    std::string err;
    JsonPtr doc = json_parse(json, len, &err);
    if (!doc || !doc->is_obj()) {
        r.error("song config: JSON parse failed: " + err);
        return r;
    }
    if (doc->get_int("version", 1) != 1)
        r.warn("song config: unknown schema version, best-effort load");

    const JsonValue* rev = doc->get("reverb");
    if (rev && !rev->is_obj()) r.warn("song config: 'reverb' is not an object, ignored");
    if (rev && rev->is_obj()) {
        out.reverb.room_size = get_clamped(rev, "roomSize", out.reverb.room_size, 0, GAIN_ONE, r, "reverb");
        out.reverb.damping = get_clamped(rev, "damping", out.reverb.damping, 0, GAIN_ONE, r, "reverb");
        out.reverb.width = get_clamped(rev, "width", out.reverb.width, 0, GAIN_ONE, r, "reverb");
        out.reverb.wet_gain = get_clamped(rev, "wetGain", out.reverb.wet_gain, 0, 2 * GAIN_ONE, r, "reverb");
    }
    const JsonValue* cho = doc->get("chorus");
    if (cho && !cho->is_obj()) r.warn("song config: 'chorus' is not an object, ignored");
    if (cho && cho->is_obj()) {
        out.chorus.rate_hz_q16 = get_clamped(cho, "rateHz", out.chorus.rate_hz_q16, 0, 10 * GAIN_ONE, r, "chorus");
        out.chorus.depth_ms_q16 = get_clamped(cho, "depthMs", out.chorus.depth_ms_q16, 0, 20 * GAIN_ONE, r, "chorus");
        out.chorus.wet_gain = get_clamped(cho, "wetGain", out.chorus.wet_gain, 0, 2 * GAIN_ONE, r, "chorus");
    }
    return r;
}

void push_song_config(SynthEngine& engine, const SongFxConfig& cfg) {
    auto push = [&](EngineCommand::Type t, u8 id, i32 v) {
        EngineCommand c;
        c.type = t;
        c.d1 = id;
        c.v = v;
        engine.queue().push(c);
    };
    push(EngineCommand::Type::ReverbConfig, 0, cfg.reverb.room_size);
    push(EngineCommand::Type::ReverbConfig, 1, cfg.reverb.damping);
    push(EngineCommand::Type::ReverbConfig, 2, cfg.reverb.width);
    push(EngineCommand::Type::ReverbConfig, 3, cfg.reverb.wet_gain);
    push(EngineCommand::Type::ChorusConfig, 0, cfg.chorus.rate_hz_q16);
    push(EngineCommand::Type::ChorusConfig, 1, cfg.chorus.depth_ms_q16);
    push(EngineCommand::Type::ChorusConfig, 2, cfg.chorus.wet_gain);
}

}  // namespace msynth
