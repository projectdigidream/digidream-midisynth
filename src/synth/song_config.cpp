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

// Read one master field: raw Q16 in, clamped through MasterParams::set (the
// single place ranges live); a value that moved was out of range.
void master_field(const JsonValue* obj, const char* key, MasterParamId id, MasterParams& out,
                  LoadResult& r, const char* where) {
    if (!obj) return;
    const JsonValue* v = obj->get(key);
    if (!v) return;
    i32 q;
    if (v->is_num()) q = v->num_q16;
    else if (v->type == JsonValue::Type::Bool) q = v->boolean ? 1 : 0;
    else {
        r.warn(std::string(where) + "." + key + ": not a number, using default");
        return;
    }
    if (id == MasterParamId::LimLookaheadBlocks) q = static_cast<i32>(v->num_int);
    out.set(id, q);
    if (out.get(id) != q && id != MasterParamId::Enabled)
        r.warn(std::string(where) + "." + key + ": out of range, clamped");
}

const JsonValue* sub(const JsonValue* obj, const char* key, LoadResult& r, const char* where) {
    if (!obj) return nullptr;
    const JsonValue* v = obj->get(key);
    if (v && !v->is_obj()) {
        r.warn(std::string(where) + "." + key + ": not an object, ignored");
        return nullptr;
    }
    return v;
}

void parse_master_object(const JsonValue* m, MasterParams& out, LoadResult& r) {
    out = MasterParams{};
    if (!m) return;
    master_field(m, "enabled", MasterParamId::Enabled, out, r, "master");
    const JsonValue* eq = sub(m, "eq", r, "master");
    const JsonValue* lo = sub(eq, "low", r, "master.eq");
    master_field(lo, "hz", MasterParamId::EqLowHz, out, r, "master.eq.low");
    master_field(lo, "db", MasterParamId::EqLowDb, out, r, "master.eq.low");
    const JsonValue* mid = sub(eq, "mid", r, "master.eq");
    master_field(mid, "hz", MasterParamId::EqMidHz, out, r, "master.eq.mid");
    master_field(mid, "db", MasterParamId::EqMidDb, out, r, "master.eq.mid");
    master_field(mid, "q", MasterParamId::EqMidQ, out, r, "master.eq.mid");
    const JsonValue* hi = sub(eq, "high", r, "master.eq");
    master_field(hi, "hz", MasterParamId::EqHighHz, out, r, "master.eq.high");
    master_field(hi, "db", MasterParamId::EqHighDb, out, r, "master.eq.high");
    const JsonValue* comp = sub(m, "comp", r, "master");
    master_field(comp, "thresholdDb", MasterParamId::CompThresholdDb, out, r, "master.comp");
    master_field(comp, "ratio", MasterParamId::CompRatio, out, r, "master.comp");
    master_field(comp, "attackMs", MasterParamId::CompAttackMs, out, r, "master.comp");
    master_field(comp, "releaseMs", MasterParamId::CompReleaseMs, out, r, "master.comp");
    master_field(comp, "makeupDb", MasterParamId::CompMakeupDb, out, r, "master.comp");
    master_field(comp, "kneeDb", MasterParamId::CompKneeDb, out, r, "master.comp");
    const JsonValue* lim = sub(m, "limiter", r, "master");
    master_field(lim, "ceilingDb", MasterParamId::LimCeilingDb, out, r, "master.limiter");
    master_field(lim, "lookaheadBlocks", MasterParamId::LimLookaheadBlocks, out, r, "master.limiter");
    master_field(lim, "releaseMs", MasterParamId::LimReleaseMs, out, r, "master.limiter");
    master_field(m, "satDrive", MasterParamId::SatDrive, out, r, "master");
    master_field(m, "width", MasterParamId::Width, out, r, "master");
    master_field(m, "gainDb", MasterParamId::GainDb, out, r, "master");
}

// Decimal for a Q16.16 value that the parser's TRUNCATING conversion maps
// back to exactly this value: n = ceil(frac * 10^9 / 2^16) (same rule as
// json_serialize in json_min.cpp).
std::string q16_str(i32 v) {
    std::string s;
    if (v < 0) { s += '-'; v = -v; }
    const u32 u = static_cast<u32>(v);
    s += std::to_string(u >> GAIN_SHIFT);
    const u32 frac = u & 0xFFFFu;
    if (frac) {
        const u64 n9 = (static_cast<u64>(frac) * 1000000000ull + 65535ull) >> 16;
        std::string d = std::to_string(n9);
        d.insert(0, 9 - d.size(), '0');
        while (d.size() > 1 && d.back() == '0') d.pop_back();
        s += '.';
        s += d;
    }
    return s;
}

}  // namespace

std::string master_to_json_number(i32 q16) { return q16_str(q16); }

LoadResult load_master_config(const char* json, size_t len, MasterParams& out) {
    LoadResult r;
    out = MasterParams{};
    std::string err;
    JsonPtr doc = json_parse(json, len, &err);
    if (!doc || !doc->is_obj()) {
        r.error("master config: JSON parse failed: " + err);
        return r;
    }
    const JsonValue* m = doc->get("master");
    if (m && !m->is_obj()) {
        r.error("master config: 'master' is not an object");
        return r;
    }
    if (!m && doc->get_int("version", 1) != 1)
        r.warn("master config: unknown schema version, best-effort load");
    parse_master_object(m ? m : doc.get(), out, r);
    return r;
}

void push_master_config(SynthEngine& engine, const MasterParams& p) {
    auto push = [&](MasterParamId id) {
        EngineCommand c;
        c.type = EngineCommand::Type::MasterParam;
        c.d1 = static_cast<u8>(id);
        c.v = p.get(id);
        engine.queue().push(c);
    };
    for (int i = 1; i < static_cast<int>(MasterParamId::COUNT); ++i) push(static_cast<MasterParamId>(i));
    push(MasterParamId::Enabled);  // last: the chain starts clean with final params
}

std::string master_to_json(const MasterParams& p, bool wrap) {
    std::string s;
    const char* ind = wrap ? "    " : "  ";
    auto line = [&](const std::string& body, bool last = false) {
        s += ind;
        s += body;
        s += last ? "\n" : ",\n";
    };
    if (wrap) s += "{\n  \"version\": 1,\n  \"master\": {\n";
    else s += "{\n";
    line("\"enabled\": " + std::string(p.enabled ? "true" : "false"));
    line("\"eq\": { \"low\": { \"hz\": " + q16_str(p.eq_low_hz) + ", \"db\": " + q16_str(p.eq_low_db) +
         " }, \"mid\": { \"hz\": " + q16_str(p.eq_mid_hz) + ", \"db\": " + q16_str(p.eq_mid_db) +
         ", \"q\": " + q16_str(p.eq_mid_q) + " }, \"high\": { \"hz\": " + q16_str(p.eq_high_hz) +
         ", \"db\": " + q16_str(p.eq_high_db) + " } }");
    line("\"comp\": { \"thresholdDb\": " + q16_str(p.comp_threshold_db) + ", \"ratio\": " +
         q16_str(p.comp_ratio) + ", \"attackMs\": " + q16_str(p.comp_attack_ms) + ", \"releaseMs\": " +
         q16_str(p.comp_release_ms) + ", \"makeupDb\": " + q16_str(p.comp_makeup_db) + ", \"kneeDb\": " +
         q16_str(p.comp_knee_db) + " }");
    line("\"limiter\": { \"ceilingDb\": " + q16_str(p.lim_ceiling_db) + ", \"lookaheadBlocks\": " +
         std::to_string(p.lim_lookahead_blocks) + ", \"releaseMs\": " + q16_str(p.lim_release_ms) + " }");
    line("\"satDrive\": " + q16_str(p.sat_drive));
    line("\"width\": " + q16_str(p.width));
    line("\"gainDb\": " + q16_str(p.gain_db), true);
    s += wrap ? "  }\n}\n" : "}\n";
    return s;
}

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
        out.reverb.pre_delay_ms_q16 = get_clamped(rev, "preDelayMs", 0, 0, 200 << GAIN_SHIFT, r, "reverb");
        out.reverb.high_pass_hz_q16 = get_clamped(rev, "highPassHz", 0, 0, 1000 << GAIN_SHIFT, r, "reverb");
    }
    const JsonValue* cho = doc->get("chorus");
    if (cho && !cho->is_obj()) r.warn("song config: 'chorus' is not an object, ignored");
    if (cho && cho->is_obj()) {
        out.chorus.rate_hz_q16 = get_clamped(cho, "rateHz", out.chorus.rate_hz_q16, 0, 10 * GAIN_ONE, r, "chorus");
        out.chorus.depth_ms_q16 = get_clamped(cho, "depthMs", out.chorus.depth_ms_q16, 0, 20 * GAIN_ONE, r, "chorus");
        out.chorus.wet_gain = get_clamped(cho, "wetGain", out.chorus.wet_gain, 0, 2 * GAIN_ONE, r, "chorus");
    }
    const JsonValue* del = doc->get("delay");
    if (del && !del->is_obj()) r.warn("song config: 'delay' is not an object, ignored");
    if (del && del->is_obj()) {
        out.delay.time_ms_q16 = get_clamped(del, "timeMs", out.delay.time_ms_q16, GAIN_ONE, 640 << GAIN_SHIFT, r, "delay");
        out.delay.feedback_q16 = get_clamped(del, "feedback", out.delay.feedback_q16, 0, (GAIN_ONE * 9) / 10, r, "delay");
        out.delay.wet_gain = get_clamped(del, "wetGain", out.delay.wet_gain, 0, 2 * GAIN_ONE, r, "delay");
        out.delay.ping_pong = del->get_bool("pingPong", false);
        // "sync": "1/8." wins over timeMs and tracks the song tempo.
        out.delay.sync_beats_q16 = parse_sync_beats(del->get_str("sync", ""), r, "delay");
    }
    // "sidechain": {source, note, targets[], depth, attackMs, holdMs, releaseMs}
    // Channels are 1-based here, as everywhere else in fx.json.
    const JsonValue* sc = doc->get("sidechain");
    if (sc && !sc->is_obj()) r.warn("song config: 'sidechain' is not an object, ignored");
    if (sc && sc->is_obj()) {
        const i32 src = static_cast<i32>(sc->get_int("source", 0));
        out.sidechain.source = (src >= 1 && src <= 16) ? src - 1 : -1;
        if (src != 0 && out.sidechain.source < 0)
            r.warn("sidechain: 'source' must be a channel 1..16, ignored");
        out.sidechain.note = clamp_i32(static_cast<i32>(sc->get_int("note", -1)), -1, 127);
        const JsonValue* tg = sc->get("targets");
        out.sidechain.targets = 0;
        if (tg && tg->is_arr()) {
            for (const auto& e : tg->arr) {
                const i32 ch = static_cast<i32>(e->num_int);
                if (ch >= 1 && ch <= 16) out.sidechain.targets |= 1u << (ch - 1);
                else r.warn("sidechain: target channel out of range, skipped");
            }
        } else if (tg) {
            r.warn("sidechain: 'targets' is not an array, ignored");
        }
        out.sidechain.depth = get_clamped(sc, "depth", 0, 0, GAIN_ONE, r, "sidechain");
        out.sidechain.attack_ms_q16 = get_clamped(sc, "attackMs", 5 << GAIN_SHIFT, 0, 500 << GAIN_SHIFT, r, "sidechain");
        out.sidechain.hold_ms_q16 = get_clamped(sc, "holdMs", 15 << GAIN_SHIFT, 0, 500 << GAIN_SHIFT, r, "sidechain");
        out.sidechain.release_ms_q16 = get_clamped(sc, "releaseMs", 180 << GAIN_SHIFT, 0, 2000 << GAIN_SHIFT, r, "sidechain");
    }

    const JsonValue* tun = doc->get("tuning");
    if (tun && !tun->is_obj()) r.warn("song config: 'tuning' is not an object, ignored");
    if (tun && tun->is_obj()) {
        const JsonValue* cents = tun->get("cents");
        if (cents && cents->is_arr()) {
            for (int i = 0; i < 12 && i < static_cast<int>(cents->arr.size()); ++i)
                out.tuning_cents_q16[i] = clamp_i32(cents->arr[i]->num_q16,
                                                    -(200 << GAIN_SHIFT), 200 << GAIN_SHIFT);
        } else {
            r.warn("song config: tuning.cents must be an array of up to 12 cents");
        }
    }
    const JsonValue* ceq = doc->get("channelEq");
    if (ceq && !ceq->is_obj()) r.warn("song config: 'channelEq' is not an object, ignored");
    if (ceq && ceq->is_obj()) {
        for (const auto& [key, val] : ceq->obj) {
            int chn = 0;
            for (char c2 : key) { if (c2 < '0' || c2 > '9') { chn = -1; break; } chn = chn * 10 + (c2 - '0'); }
            if (chn < 1 || chn > 16 || !val->is_obj()) {
                r.warn("song config: channelEq keys are MIDI channels \"1\"..\"16\"");
                continue;
            }
            ChannelEqParams& e = out.channel_eq[chn - 1];
            e.low_db_q16 = get_clamped(val.get(), "lowDb", 0, -(24 << GAIN_SHIFT), 24 << GAIN_SHIFT, r, "channelEq");
            e.mid_db_q16 = get_clamped(val.get(), "midDb", 0, -(24 << GAIN_SHIFT), 24 << GAIN_SHIFT, r, "channelEq");
            e.mid_hz_q16 = get_clamped(val.get(), "midHz", 1000 << GAIN_SHIFT, 100 << GAIN_SHIFT, 8000 << GAIN_SHIFT, r, "channelEq");
            e.high_db_q16 = get_clamped(val.get(), "highDb", 0, -(24 << GAIN_SHIFT), 24 << GAIN_SHIFT, r, "channelEq");
        }
    }
    const JsonValue* mst = doc->get("master");
    if (mst && !mst->is_obj()) r.warn("song config: 'master' is not an object, ignored");
    if (mst && mst->is_obj()) parse_master_object(mst, out.master, r);
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
    push(EngineCommand::Type::ReverbConfig, 4, cfg.reverb.pre_delay_ms_q16);
    push(EngineCommand::Type::ReverbConfig, 5, cfg.reverb.high_pass_hz_q16);
    push(EngineCommand::Type::ChorusConfig, 0, cfg.chorus.rate_hz_q16);
    push(EngineCommand::Type::ChorusConfig, 1, cfg.chorus.depth_ms_q16);
    push(EngineCommand::Type::ChorusConfig, 2, cfg.chorus.wet_gain);
    push(EngineCommand::Type::DelayConfig, 0, cfg.delay.time_ms_q16);
    push(EngineCommand::Type::DelayConfig, 1, cfg.delay.feedback_q16);
    push(EngineCommand::Type::DelayConfig, 2, cfg.delay.wet_gain);
    push(EngineCommand::Type::DelayConfig, 3, cfg.delay.ping_pong ? GAIN_ONE : 0);
    push(EngineCommand::Type::DelayConfig, 4, cfg.delay.sync_beats_q16);
    push(EngineCommand::Type::SidechainConfig, 0, cfg.sidechain.source);
    push(EngineCommand::Type::SidechainConfig, 1, cfg.sidechain.note);
    push(EngineCommand::Type::SidechainConfig, 2, static_cast<i32>(cfg.sidechain.targets));
    push(EngineCommand::Type::SidechainConfig, 4, cfg.sidechain.attack_ms_q16);
    push(EngineCommand::Type::SidechainConfig, 5, cfg.sidechain.hold_ms_q16);
    push(EngineCommand::Type::SidechainConfig, 6, cfg.sidechain.release_ms_q16);
    push(EngineCommand::Type::SidechainConfig, 3, cfg.sidechain.depth);  // last: arms it
    for (u8 i = 0; i < 12; ++i) {
        EngineCommand c;
        c.type = EngineCommand::Type::TuningCent;
        c.d1 = i;
        c.v = cfg.tuning_cents_q16[i];
        engine.queue().push(c);
    }
    for (u8 ch = 0; ch < 16; ++ch) {
        const ChannelEqParams& e = cfg.channel_eq[ch];
        const i32 vals[4] = {e.low_db_q16, e.mid_db_q16, e.mid_hz_q16, e.high_db_q16};
        for (u8 k = 0; k < 4; ++k) {
            EngineCommand c;
            c.type = EngineCommand::Type::ChannelEqParam;
            c.slot = ch;
            c.d1 = k;
            c.v = vals[k];
            engine.queue().push(c);
        }
    }
    push_master_config(engine, cfg.master);
}

}  // namespace msynth
