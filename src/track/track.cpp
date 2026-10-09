#include "track.h"

#include <cstring>

#include "../output/audio_output_offline.h"
#include "../output/sha256.h"
#include "../synth/patch_loader.h"

namespace msynth {
namespace {

// ---- little-endian byte IO -------------------------------------------------
struct Writer {
    std::vector<u8>& out;
    void u8v(u8 v) { out.push_back(v); }
    void u16v(u16 v) { for (int i = 0; i < 2; ++i) out.push_back(static_cast<u8>(v >> (8 * i))); }
    void u32v(u32 v) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(v >> (8 * i))); }
    void u64v(u64 v) { for (int i = 0; i < 8; ++i) out.push_back(static_cast<u8>(v >> (8 * i))); }
    void i32v(i32 v) { u32v(static_cast<u32>(v)); }
    void blob(const void* p, size_t n) {
        const u8* b = static_cast<const u8*>(p);
        out.insert(out.end(), b, b + n);
    }
    void str(const std::string& s) {
        u32v(static_cast<u32>(s.size()));
        blob(s.data(), s.size());
    }
};

struct Reader {
    const u8* p;
    const u8* end;
    bool ok = true;

    bool need(size_t n) {
        if (static_cast<size_t>(end - p) < n) { ok = false; return false; }
        return true;
    }
    size_t remaining() const { return static_cast<size_t>(end - p); }
    u8 u8v() { if (!need(1)) return 0; return *p++; }
    u16 u16v() {
        if (!need(2)) return 0;
        u16 v = static_cast<u16>(p[0] | (p[1] << 8));
        p += 2;
        return v;
    }
    u32 u32v() {
        if (!need(4)) return 0;
        u32 v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<u32>(p[i]) << (8 * i);
        p += 4;
        return v;
    }
    u64 u64v() {
        if (!need(8)) return 0;
        u64 v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<u64>(p[i]) << (8 * i);
        p += 8;
        return v;
    }
    i32 i32v() { return static_cast<i32>(u32v()); }
    std::string str(u32 max_len) {
        const u32 n = u32v();
        if (n > max_len || !need(n)) { ok = false; return {}; }
        std::string s(reinterpret_cast<const char*>(p), n);
        p += n;
        return s;
    }
};

constexpr u32 TAG_SONG = 0x474E4F53u;  // "SONG"
constexpr u32 TAG_BANK = 0x4B4E4142u;  // "BANK"
constexpr u32 TAG_KIT = 0x2054494Bu;   // "KIT "
constexpr u32 TAG_FX = 0x20205846u;    // "FX  "
constexpr u32 TAG_MSTR = 0x5254534Du;  // "MSTR"
constexpr u32 TAG_META = 0x4154454Du;  // "META"

// ---- instrument usage scan (mirrors the tool's scanner) --------------------
void scan_usage(const ParsedSong& song, u8 programs[16], u8 drums[16]) {
    u8 ch_prog[16] = {};
    std::memset(programs, 0, 16);
    std::memset(drums, 0, 16);
    for (const MidiEvent& e : song.events) {
        if (!e.is_channel()) continue;
        const u8 ch = e.channel();
        if (e.kind() == 0xC0) ch_prog[ch] = e.d1 & 127;
        else if (e.kind() == 0x90 && e.d2 > 0) {
            if (ch == 9) drums[e.d1 >> 3] |= static_cast<u8>(1 << (e.d1 & 7));
            else programs[ch_prog[ch] >> 3] |= static_cast<u8>(1 << (ch_prog[ch] & 7));
        }
    }
}
bool bit(const u8* bm, int i) { return (bm[i >> 3] >> (i & 7)) & 1; }

LoadResult bake_payload_impl(const ParsedSong& song, const std::string& shaken_bank,
                             const std::string& shaken_kit, const SongFxConfig& fx,
                             TrackMeta& meta, u32 verify_seconds, std::vector<u8>& out,
                             LoadResult rep);

// ---- section payloads ------------------------------------------------------
void write_song(Writer& w, const ParsedSong& s) {
    w.u32v(s.tempo.division());
    w.u32v(s.end_tick);
    w.u64v(s.end_sample);
    w.u8v(static_cast<u8>((s.has_loop_start ? 1 : 0) | (s.has_loop_end ? 2 : 0)));
    w.u32v(s.loop_start_tick);
    w.u32v(s.loop_end_tick);
    const auto& segs = s.tempo.segments();
    w.u32v(static_cast<u32>(segs.size()));
    for (const auto& t : segs) {
        w.u32v(t.start_tick);
        w.u64v(t.start_sample);
        w.u32v(t.us_per_quarter);
        w.u64v(t.samples_per_tick_fp);
    }
    w.u32v(static_cast<u32>(s.events.size()));
    for (const auto& e : s.events) {
        w.u64v(e.sample);
        w.u32v(e.tick);
        w.u8v(e.status);
        w.u8v(e.d1);
        w.u8v(e.d2);
        w.u16v(e.track);
        w.u32v(e.seq);
    }
}

bool read_song(Reader& r, ParsedSong& s, std::string& why) {
    const u32 division = r.u32v();
    s.end_tick = r.u32v();
    s.end_sample = r.u64v();
    const u8 loop_flags = r.u8v();
    s.has_loop_start = loop_flags & 1;
    s.has_loop_end = (loop_flags & 2) != 0;
    s.loop_start_tick = r.u32v();
    s.loop_end_tick = r.u32v();
    const u32 nsegs = r.u32v();
    if (nsegs == 0 || nsegs > 1u << 20) { why = "bad tempo segment count"; return false; }
    // Rebuild the tempo map through its own constructor-equivalent: the baked
    // anchors are authoritative, so feed the raw segments back in.
    std::vector<std::pair<u32, u32>> tempos;
    std::vector<TempoSegment> raw(nsegs);
    for (auto& t : raw) {
        t.start_tick = r.u32v();
        t.start_sample = r.u64v();
        t.us_per_quarter = r.u32v();
        t.samples_per_tick_fp = r.u64v();
        tempos.emplace_back(t.start_tick, t.us_per_quarter);
    }
    s.tempo.build(division, tempos);
    if (!r.ok) { why = "truncated tempo map"; return false; }
    // The rebuilt map must reproduce the baked anchors exactly (same
    // canonical conversion); if not, the file is corrupt or from a
    // diverging build.
    if (s.tempo.segments().size() != raw.size()) { why = "tempo map mismatch"; return false; }
    for (size_t i = 0; i < raw.size(); ++i) {
        const auto& a = s.tempo.segments()[i];
        const auto& b = raw[i];
        if (a.start_tick != b.start_tick || a.start_sample != b.start_sample ||
            a.samples_per_tick_fp != b.samples_per_tick_fp) {
            why = "tempo map anchors diverge (corrupt file or incompatible build)";
            return false;
        }
    }
    const u32 nevents = r.u32v();
    if (nevents > 1u << 24) { why = "bad event count"; return false; }
    s.events.resize(nevents);
    for (auto& e : s.events) {
        e.sample = r.u64v();
        e.tick = r.u32v();
        e.status = r.u8v();
        e.d1 = r.u8v();
        e.d2 = r.u8v();
        e.track = r.u16v();
        e.seq = r.u32v();
    }
    if (!r.ok) { why = "truncated event list"; return false; }
    return true;
}

// Tree-shake a bank/kit JSON document down to the used entries.
std::string shake_bank(const JsonValue& doc, const u8 programs[16], LoadResult& rep) {
    JsonValue out;
    out.type = JsonValue::Type::Object;
    auto num = std::make_shared<JsonValue>();
    num->type = JsonValue::Type::Number;
    num->num_int = 1;
    num->num_q16 = GAIN_ONE;
    out.obj["version"] = num;
    if (const JsonValue* fb = doc.get("fallback"))
        out.obj["fallback"] = std::make_shared<JsonValue>(*fb);
    auto progs = std::make_shared<JsonValue>();
    progs->type = JsonValue::Type::Object;
    const JsonValue* src = doc.get("programs");
    for (int p = 0; p < 128; ++p) {
        if (!bit(programs, p)) continue;
        const JsonValue* v = src ? src->get(std::to_string(p)) : nullptr;
        if (v) progs->obj[std::to_string(p)] = std::make_shared<JsonValue>(*v);
        else rep.warn("program " + std::to_string(p) + " is used but unauthored (fallback)");
    }
    out.obj["programs"] = progs;
    return json_serialize(out);
}

std::string shake_kit(const JsonValue& doc, const u8 drums[16], LoadResult& rep) {
    JsonValue out;
    out.type = JsonValue::Type::Object;
    auto num = std::make_shared<JsonValue>();
    num->type = JsonValue::Type::Number;
    num->num_int = 1;
    num->num_q16 = GAIN_ONE;
    out.obj["version"] = num;
    auto notes = std::make_shared<JsonValue>();
    notes->type = JsonValue::Type::Object;
    const JsonValue* src = doc.get("notes");
    for (int n = 0; n < 128; ++n) {
        if (!bit(drums, n)) continue;
        const JsonValue* v = src ? src->get(std::to_string(n)) : nullptr;
        if (v) notes->obj[std::to_string(n)] = std::make_shared<JsonValue>(*v);
        else rep.warn("drum note " + std::to_string(n) + " is used but unmapped (will be SILENT)");
    }
    out.obj["notes"] = notes;
    return json_serialize(out);
}

}  // namespace

// ---------------------------------------------------------------------------
// Cutting.

namespace {

// Chased channel state at a tick position: what must be re-emitted at the
// top of a slice so it opens sounding right (§10's seek chase, applied at
// bake time).
struct ChaseState {
    int program = -1;              // -1 = never set
    int cc[128];                   // -1 = never set
    int bend = -1;                 // raw 14-bit, -1 = never set
    int bend_range_semis = -1;     // via RPN 0
    ChaseState() { for (int& v : cc) v = -1; }
};

}  // namespace

LoadResult cut_song(const ParsedSong& src, u32 start_tick, u32 end_tick, ParsedSong& out) {
    LoadResult rep;
    out = ParsedSong{};
    if (end_tick <= start_tick) {
        rep.error("cut: empty range");
        return rep;
    }
    if (start_tick > src.end_tick) {
        rep.error("cut: start beyond the song");
        return rep;
    }
    if (end_tick > src.end_tick) {
        rep.warn("cut: end clamped to song end");
        end_tick = src.end_tick;
        if (end_tick <= start_tick) {
            rep.error("cut: empty range after clamp");
            return rep;
        }
    }

    // ---- Chase pass over everything before start_tick ----
    ChaseState chase[16];
    int timesig_num = -1, timesig_denom_log2 = 2;
    int open_before[16][128] = {};  // note-on depth entering the cut
    for (const MidiEvent& e : src.events) {
        if (e.tick >= start_tick) break;
        if (e.status == MidiEvent::META_TIME_SIG) {
            timesig_num = e.d1;
            timesig_denom_log2 = e.d2;
            continue;
        }
        if (!e.is_channel()) continue;
        const u8 ch = e.channel();
        switch (e.kind()) {
            case 0x90: open_before[ch][e.d1]++; break;
            case 0x80: if (open_before[ch][e.d1]) open_before[ch][e.d1]--; break;
            case 0xB0:
                // Track RPN selection to capture bend range; store plain CCs.
                if (e.d1 == 6 && chase[ch].cc[101] == 0 && chase[ch].cc[100] == 0)
                    chase[ch].bend_range_semis = e.d2;
                else if (e.d1 != 6 && e.d1 != 38 && e.d1 != 98 && e.d1 != 99 &&
                         e.d1 != 100 && e.d1 != 101 && e.d1 != 120 && e.d1 != 121 &&
                         e.d1 != 123)
                    chase[ch].cc[e.d1] = e.d2;
                if (e.d1 == 100 || e.d1 == 101 || e.d1 == 98 || e.d1 == 99)
                    chase[ch].cc[e.d1] = e.d2;
                break;
            case 0xC0: chase[ch].program = e.d1; break;
            case 0xE0: chase[ch].bend = (static_cast<int>(e.d2) << 7) | e.d1; break;
            default: break;
        }
    }

    // ---- Tempo: value in effect at start + changes inside the range ----
    std::vector<std::pair<u32, u32>> tempos;
    {
        const auto& segs = src.tempo.segments();
        u32 at_start = 500000;
        for (const auto& s : segs)
            if (s.start_tick <= start_tick) at_start = s.us_per_quarter;
            else break;
        tempos.emplace_back(0, at_start);
        for (const auto& s : segs)
            if (s.start_tick > start_tick && s.start_tick < end_tick)
                tempos.emplace_back(s.start_tick - start_tick, s.us_per_quarter);
    }
    out.tempo.build(src.tempo.division(), tempos);

    // ---- Emit the chased state at tick 0, deterministic order ----
    u32 seq = 0;
    auto emit = [&](u8 status, u8 d1, u8 d2) {
        MidiEvent e;
        e.tick = 0;
        e.status = status;
        e.d1 = d1;
        e.d2 = d2;
        e.track = 0;
        e.seq = seq++;
        out.events.push_back(e);
    };
    if (timesig_num > 0)
        emit(MidiEvent::META_TIME_SIG, static_cast<u8>(timesig_num),
             static_cast<u8>(timesig_denom_log2));
    for (int ch = 0; ch < 16; ++ch) {
        const ChaseState& c = chase[ch];
        if (c.program >= 0) emit(static_cast<u8>(0xC0 | ch), static_cast<u8>(c.program), 0);
        for (int n = 0; n < 128; ++n) {
            if (n == 98 || n == 99 || n == 100 || n == 101) continue;  // RPN plumbing
            if (c.cc[n] >= 0)
                emit(static_cast<u8>(0xB0 | ch), static_cast<u8>(n), static_cast<u8>(c.cc[n]));
        }
        if (c.bend_range_semis >= 0) {
            emit(static_cast<u8>(0xB0 | ch), 101, 0);
            emit(static_cast<u8>(0xB0 | ch), 100, 0);
            emit(static_cast<u8>(0xB0 | ch), 6, static_cast<u8>(c.bend_range_semis));
            emit(static_cast<u8>(0xB0 | ch), 101, 127);  // RPN null: leave clean state
            emit(static_cast<u8>(0xB0 | ch), 100, 127);
        }
        if (c.bend >= 0 && c.bend != 8192) {
            emit(static_cast<u8>(0xE0 | ch), static_cast<u8>(c.bend & 0x7F),
                 static_cast<u8>((c.bend >> 7) & 0x7F));
        }
    }

    // ---- Copy the range, re-based; drop orphan note-offs ----
    const u32 span = end_tick - start_tick;
    int open_inside[16][128] = {};
    int dropped_orphans = 0, dropped_note_ons = 0;
    for (const MidiEvent& e : src.events) {
        if (e.tick < start_tick) continue;
        if (e.tick >= end_tick) break;
        MidiEvent n = e;
        n.tick = e.tick - start_tick;
        n.track = 0;
        if (e.is_channel()) {
            const u8 ch = e.channel();
            if (e.kind() == 0x90) {
                open_inside[ch][e.d1]++;
            } else if (e.kind() == 0x80) {
                if (open_inside[ch][e.d1] > 0) {
                    open_inside[ch][e.d1]--;
                } else if (open_before[ch][e.d1] > 0) {
                    open_before[ch][e.d1]--;
                    ++dropped_orphans;
                    continue;  // belongs to a pre-cut note-on we didn't carry
                }
            }
        }
        n.seq = seq++;  // dense: assigned only for events actually kept
        out.events.push_back(n);
    }
    (void)dropped_note_ons;
    if (dropped_orphans)
        rep.warn("cut: dropped " + std::to_string(dropped_orphans) +
                 " note-off(s) belonging to notes that started before the cut");

    // ---- Close notes still open at the cut end ----
    int closed = 0;
    for (int ch = 0; ch < 16; ++ch)
        for (int n = 0; n < 128; ++n)
            while (open_inside[ch][n] > 0) {
                open_inside[ch][n]--;
                MidiEvent e;
                e.tick = span;
                e.status = static_cast<u8>(0x80 | ch);
                e.d1 = static_cast<u8>(n);
                e.track = 0;
                e.seq = seq++;
                out.events.push_back(e);
                ++closed;
            }
    if (closed)
        rep.warn("cut: closed " + std::to_string(closed) + " note(s) still open at the cut end");

    // ---- Bake sample positions through the canonical conversion ----
    out.end_tick = span;
    for (auto& e : out.events) e.sample = out.tempo.tick_to_sample(e.tick);
    out.end_sample = out.tempo.tick_to_sample(span);
    // The slice loops whole by default (gameplay-ready piece).
    out.has_loop_start = out.has_loop_end = false;
    out.loop_start_tick = 0;
    out.loop_end_tick = span;
    return rep;
}

LoadResult cut_track(const u8* track, size_t len, u32 start_tick, u32 end_tick,
                     const CutOptions& opts, std::vector<u8>& out) {
    LoadResult rep;
    TrackData parent;
    {
        LoadResult lr = load_track(track, len, parent);
        if (!lr.ok) return lr;
        for (auto& w : lr.warnings) rep.warn(w);
    }
    ParsedSong slice;
    {
        LoadResult cr = cut_song(parent.song, start_tick, end_tick, slice);
        for (auto& w : cr.warnings) rep.warn(w);
        if (!cr.ok) {
            for (auto& e : cr.errors) rep.error(e);
            return rep;
        }
    }
    // Slices carry the parent's embedded content VERBATIM -- no re-shaking.
    // Every slice of one parent therefore shares the same content hash, and
    // a game can crossfade freely between them over one loaded bank/kit.
    // (The parent bake already tree-shook and warned about its content.)
    TrackMeta meta;
    meta.name = opts.name.empty() ? parent.meta.name + "_cut" : opts.name;
    meta.duration_ms = static_cast<u32>(slice.end_sample * 1000 / SAMPLE_RATE);
    scan_usage(slice, meta.programs_used, meta.drum_notes_used);  // info only
    return bake_payload_impl(slice, parent.bank_json, parent.kit_json, parent.fx, meta,
                             opts.verify_seconds, out, rep);
}

bool is_track(const u8* data, size_t len) {
    if (len < 4) return false;
    u32 magic = 0;
    for (int i = 0; i < 4; ++i) magic |= static_cast<u32>(data[i]) << (8 * i);
    return magic == TRACK_MAGIC;
}

LoadResult bake_track(const u8* smf, size_t smf_len, const char* bank_json, size_t bank_len,
                      const char* kit_json, size_t kit_len, const BakeOptions& opts,
                      std::vector<u8>& out) {
    LoadResult rep;
    out.clear();

    // 1) Parse and validate every source; bake fails loudly where load can't.
    ParsedSong song;
    ParseResult pr = smf_parse(smf, smf_len, song);
    for (auto& w : pr.warnings) rep.warn("smf: " + w);
    if (!pr.ok) {
        for (auto& e : pr.errors) rep.error("smf: " + e);
        return rep;
    }
    if (opts.loop_start_tick >= 0) {
        song.has_loop_start = true;
        song.loop_start_tick = static_cast<u32>(opts.loop_start_tick);
    }
    if (opts.loop_end_tick >= 0) {
        song.has_loop_end = true;
        song.loop_end_tick = static_cast<u32>(opts.loop_end_tick);
    }

    std::string jerr;
    JsonPtr bank_doc = json_parse(bank_json, bank_len, &jerr);
    if (!bank_doc || !bank_doc->is_obj()) {
        rep.error("bank: JSON parse failed: " + jerr);
        return rep;
    }
    jerr.clear();
    JsonPtr kit_doc = json_parse(kit_json, kit_len, &jerr);
    if (!kit_doc || !kit_doc->is_obj()) {
        rep.error("kit: JSON parse failed: " + jerr);
        return rep;
    }

    // 2) Tree-shake to what the song actually uses.
    TrackMeta meta;
    meta.name = opts.name.empty() ? "track" : opts.name;
    meta.duration_ms = static_cast<u32>(song.end_sample * 1000 / SAMPLE_RATE);
    scan_usage(song, meta.programs_used, meta.drum_notes_used);
    const std::string shaken_bank = shake_bank(*bank_doc, meta.programs_used, rep);
    const std::string shaken_kit = shake_kit(*kit_doc, meta.drum_notes_used, rep);

    // 3) Validate the shaken content through the real loaders (catches every
    //    clamp/warning the runtime would hit, at bake time instead).
    {
        WaveTableCache cache;
        PatchBank bank;
        DrumKit kit;
        LoadResult br = load_patch_bank(shaken_bank.data(), shaken_bank.size(), cache, bank);
        for (auto& w : br.warnings) rep.warn("bank: " + w);
        if (!br.ok) {
            for (auto& e : br.errors) rep.error("bank: " + e);
            return rep;
        }
        LoadResult kr = load_drum_kit(shaken_kit.data(), shaken_kit.size(), cache, kit);
        for (auto& w : kr.warnings) rep.warn("kit: " + w);
        if (!kr.ok) {
            for (auto& e : kr.errors) rep.error("kit: " + e);
            return rep;
        }
    }

    SongFxConfig fx;
    if (opts.fx_json) {
        LoadResult fr = load_song_config(opts.fx_json, opts.fx_json_len, fx);
        for (auto& w : fr.warnings) rep.warn("fx: " + w);
        if (!fr.ok) {
            for (auto& e : fr.errors) rep.error("fx: " + e);
            return rep;
        }
    }
    if (opts.master_json) {
        LoadResult mr = load_master_config(opts.master_json, opts.master_json_len, fx.master);
        for (auto& w : mr.warnings) rep.warn("master: " + w);
        if (!mr.ok) {
            for (auto& e : mr.errors) rep.error("master: " + e);
            return rep;
        }
    }

    return bake_payload_impl(song, shaken_bank, shaken_kit, fx, meta, opts.verify_seconds, out,
                             rep);
}

namespace {
LoadResult bake_payload_impl(const ParsedSong& song, const std::string& shaken_bank,
                             const std::string& shaken_kit, const SongFxConfig& fx,
                             TrackMeta& meta, u32 verify_seconds, std::vector<u8>& out,
                             LoadResult rep) {
    out.clear();  // callers may reuse a buffer (the WASM binding does)
    // Content hash: lets consumers know when two tracks share loaded content.
    {
        Sha256 ch;
        ch.update(shaken_bank.data(), shaken_bank.size());
        ch.update("\0", 1);
        ch.update(shaken_kit.data(), shaken_kit.size());
        const std::string hex = ch.hex_digest();
        auto nib = [](char c) -> u8 { return static_cast<u8>(c <= '9' ? c - '0' : c - 'a' + 10); };
        for (int i = 0; i < 32; ++i)
            meta.content_hash[i] = static_cast<u8>((nib(hex[2 * i]) << 4) | nib(hex[2 * i + 1]));
    }

    // Optional determinism seal: render through the real engine and embed
    // the PCM hash.
    if (verify_seconds > 0) {
        WaveTableCache cache;
        PatchBank bank;
        DrumKit kit;
        load_patch_bank(shaken_bank.data(), shaken_bank.size(), cache, bank);
        load_drum_kit(shaken_kit.data(), shaken_kit.size(), cache, kit);
        OfflineRenderer renderer;
        renderer.engine().set_content(&bank, &kit);
        push_song_config(renderer.engine(), fx);
        ParsedSong copy = song;  // renderer owns its own
        renderer.director().active().load_parsed(std::move(copy));
        renderer.director().play();
        std::vector<i16> pcm;
        const u64 frames =
            (static_cast<u64>(verify_seconds) * SAMPLE_RATE / BUFFER_FRAMES) * BUFFER_FRAMES;
        renderer.render(pcm, frames);
        Sha256 h;
        h.update(pcm.data(), pcm.size() * 2);
        const std::string hex = h.hex_digest();
        for (int i = 0; i < 32; ++i) {
            auto nib = [&](char c) -> u8 {
                return static_cast<u8>(c <= '9' ? c - '0' : c - 'a' + 10);
            };
            meta.render_hash[i] = static_cast<u8>((nib(hex[2 * i]) << 4) | nib(hex[2 * i + 1]));
        }
        meta.has_render_hash = true;
        meta.render_seconds = verify_seconds;
    }

    // Serialize sections.
    std::vector<u8> payload;
    Writer w{payload};
    struct Sec { u32 tag; u32 off; u32 size; };
    std::vector<Sec> secs;
    auto section = [&](u32 tag, auto&& fill) {
        const u32 off = static_cast<u32>(payload.size());
        fill();
        secs.push_back({tag, off, static_cast<u32>(payload.size()) - off});
    };
    section(TAG_SONG, [&] { write_song(w, song); });
    section(TAG_BANK, [&] { w.str(shaken_bank); });
    section(TAG_KIT, [&] { w.str(shaken_kit); });
    section(TAG_FX, [&] {
        w.i32v(fx.reverb.room_size);
        w.i32v(fx.reverb.damping);
        w.i32v(fx.reverb.width);
        w.i32v(fx.reverb.wet_gain);
        w.i32v(fx.chorus.rate_hz_q16);
        w.i32v(fx.chorus.depth_ms_q16);
        w.i32v(fx.chorus.wet_gain);
        // Appended in later versions (readers treat them as optional):
        w.i32v(fx.delay.time_ms_q16);
        w.i32v(fx.delay.feedback_q16);
        w.i32v(fx.delay.wet_gain);
        w.i32v(fx.delay.ping_pong ? 1 : 0);
        for (int i = 0; i < 12; ++i) w.i32v(fx.tuning_cents_q16[i]);
        for (int ch = 0; ch < 16; ++ch) {
            w.i32v(fx.channel_eq[ch].low_db_q16);
            w.i32v(fx.channel_eq[ch].mid_db_q16);
            w.i32v(fx.channel_eq[ch].mid_hz_q16);
            w.i32v(fx.channel_eq[ch].high_db_q16);
        }
        // Tempo-synced delay, reverb send conditioning, sidechain ducking.
        w.i32v(fx.delay.sync_beats_q16);
        w.i32v(fx.reverb.pre_delay_ms_q16);
        w.i32v(fx.reverb.high_pass_hz_q16);
        w.i32v(fx.sidechain.source);
        w.i32v(fx.sidechain.note);
        w.i32v(static_cast<i32>(fx.sidechain.targets));
        w.i32v(fx.sidechain.depth);
        w.i32v(fx.sidechain.attack_ms_q16);
        w.i32v(fx.sidechain.hold_ms_q16);
        w.i32v(fx.sidechain.release_ms_q16);
    });
    if (fx.master.enabled) {
        // Only when engaged: bypass tracks stay byte-identical to pre-MSTR
        // bakes. Payload: count, then one i32 per MasterParamId (append-only).
        section(TAG_MSTR, [&] {
            const u32 n = static_cast<u32>(MasterParamId::COUNT);
            w.u32v(n);
            for (u32 i = 0; i < n; ++i) w.i32v(fx.master.get(static_cast<MasterParamId>(i)));
        });
    }
    section(TAG_META, [&] {
        w.str(meta.name);
        w.u32v(meta.duration_ms);
        w.blob(meta.programs_used, 16);
        w.blob(meta.drum_notes_used, 16);
        w.u8v(meta.has_render_hash ? 1 : 0);
        w.blob(meta.render_hash, 32);
        w.u32v(meta.render_seconds);
        w.blob(meta.content_hash, 32);
    });

    // 6) Header + integrity hash over the payload.
    Sha256 h;
    h.update(payload.data(), payload.size());
    const std::string hex = h.hex_digest();

    Writer hw{out};
    hw.u32v(TRACK_MAGIC);
    hw.u16v(TRACK_VERSION);
    hw.u16v(0);  // flags
    hw.blob(hex.data(), 64);
    hw.u32v(static_cast<u32>(secs.size()));
    for (const auto& s : secs) {
        hw.u32v(s.tag);
        hw.u32v(s.off);
        hw.u32v(s.size);
    }
    out.insert(out.end(), payload.begin(), payload.end());
    return rep;
}
}  // namespace

LoadResult load_track(const u8* data, size_t len, TrackData& out) {
    LoadResult rep;
    out = TrackData{};
    Reader r{data, data + len};
    if (r.u32v() != TRACK_MAGIC) {
        rep.error("not a track file (bad magic)");
        return rep;
    }
    const u16 version = r.u16v();
    r.u16v();  // flags
    if (version > TRACK_VERSION) {
        rep.error("track version " + std::to_string(version) + " is newer than this build");
        return rep;
    }
    if (!r.need(64)) {
        rep.error("truncated header");
        return rep;
    }
    std::string stored_hash(reinterpret_cast<const char*>(r.p), 64);
    r.p += 64;
    const u32 nsecs = r.u32v();
    if (!r.ok || nsecs > 64) {
        rep.error("bad section table");
        return rep;
    }
    struct Sec { u32 tag, off, size; };
    std::vector<Sec> secs(nsecs);
    for (auto& s : secs) {
        s.tag = r.u32v();
        s.off = r.u32v();
        s.size = r.u32v();
    }
    if (!r.ok) {
        rep.error("truncated section table");
        return rep;
    }
    const u8* payload = r.p;
    const size_t payload_len = static_cast<size_t>(data + len - payload);

    // Integrity: trusted-or-rejected.
    if (sha256_hex(payload, payload_len) != stored_hash) {
        rep.error("payload hash mismatch (corrupt track)");
        return rep;
    }

    auto find = [&](u32 tag) -> const Sec* {
        for (const auto& s : secs)
            if (s.tag == tag) return &s;
        return nullptr;
    };
    auto sec_reader = [&](const Sec* s) {
        return Reader{payload + s->off, payload + s->off + s->size};
    };
    for (const auto& s : secs) {
        if (static_cast<u64>(s.off) + s.size > payload_len) {
            rep.error("section out of bounds");
            return rep;
        }
    }

    const Sec* song_sec = find(TAG_SONG);
    const Sec* bank_sec = find(TAG_BANK);
    const Sec* kit_sec = find(TAG_KIT);
    if (!song_sec || !bank_sec || !kit_sec) {
        rep.error("missing required section");
        return rep;
    }

    {
        Reader sr = sec_reader(song_sec);
        std::string why;
        if (!read_song(sr, out.song, why)) {
            rep.error("song: " + why);
            return rep;
        }
    }
    {
        Reader br = sec_reader(bank_sec);
        out.bank_json = br.str(1u << 24);
        Reader kr = sec_reader(kit_sec);
        out.kit_json = kr.str(1u << 24);
        if (!br.ok || !kr.ok) {
            rep.error("bad content section");
            return rep;
        }
    }
    if (const Sec* fs = find(TAG_FX)) {
        Reader fr = sec_reader(fs);
        out.fx.reverb.room_size = fr.i32v();
        out.fx.reverb.damping = fr.i32v();
        out.fx.reverb.width = fr.i32v();
        out.fx.reverb.wet_gain = fr.i32v();
        out.fx.chorus.rate_hz_q16 = fr.i32v();
        out.fx.chorus.depth_ms_q16 = fr.i32v();
        out.fx.chorus.wet_gain = fr.i32v();
        if (!fr.ok) rep.warn("fx section truncated, using defaults");
        // Optional tail (delay + tuning) appended by newer bakers.
        if (fr.ok && fr.remaining() >= 16 * 4) {
            out.fx.delay.time_ms_q16 = fr.i32v();
            out.fx.delay.feedback_q16 = fr.i32v();
            out.fx.delay.wet_gain = fr.i32v();
            out.fx.delay.ping_pong = fr.i32v() != 0;
            for (int i = 0; i < 12; ++i) out.fx.tuning_cents_q16[i] = fr.i32v();
            if (fr.remaining() >= 16 * 4 * 4) {
                for (int ch = 0; ch < 16; ++ch) {
                    out.fx.channel_eq[ch].low_db_q16 = fr.i32v();
                    out.fx.channel_eq[ch].mid_db_q16 = fr.i32v();
                    out.fx.channel_eq[ch].mid_hz_q16 = fr.i32v();
                    out.fx.channel_eq[ch].high_db_q16 = fr.i32v();
                }
                if (fr.remaining() >= 10 * 4) {
                    out.fx.delay.sync_beats_q16 = fr.i32v();
                    out.fx.reverb.pre_delay_ms_q16 = fr.i32v();
                    out.fx.reverb.high_pass_hz_q16 = fr.i32v();
                    out.fx.sidechain.source = fr.i32v();
                    out.fx.sidechain.note = fr.i32v();
                    out.fx.sidechain.targets = static_cast<u32>(fr.i32v());
                    out.fx.sidechain.depth = fr.i32v();
                    out.fx.sidechain.attack_ms_q16 = fr.i32v();
                    out.fx.sidechain.hold_ms_q16 = fr.i32v();
                    out.fx.sidechain.release_ms_q16 = fr.i32v();
                }
            }
        }
    }
    out.fx.master = MasterParams{};
    if (const Sec* ms = find(TAG_MSTR)) {
        Reader mr = sec_reader(ms);
        const u32 n = mr.u32v();
        if (n > 256) {
            rep.error("bad master section");
            return rep;
        }
        MasterParams m;
        for (u32 i = 0; i < n && mr.ok; ++i) {
            const i32 v = mr.i32v();
            if (i < static_cast<u32>(MasterParamId::COUNT)) m.set(static_cast<MasterParamId>(i), v);
            // newer writers may append ids we don't know: ignored
        }
        if (!mr.ok) rep.warn("master section truncated, mastering bypassed");
        else out.fx.master = m;
    }
    if (const Sec* ms = find(TAG_META)) {
        Reader mr = sec_reader(ms);
        out.meta.name = mr.str(4096);
        out.meta.duration_ms = mr.u32v();
        if (mr.need(16)) { std::memcpy(out.meta.programs_used, mr.p, 16); mr.p += 16; }
        if (mr.need(16)) { std::memcpy(out.meta.drum_notes_used, mr.p, 16); mr.p += 16; }
        out.meta.has_render_hash = mr.u8v() != 0;
        if (mr.need(32)) { std::memcpy(out.meta.render_hash, mr.p, 32); mr.p += 32; }
        out.meta.render_seconds = mr.u32v();
        if (mr.need(32)) { std::memcpy(out.meta.content_hash, mr.p, 32); mr.p += 32; }
        if (!mr.ok) rep.warn("meta section truncated");
    }
    return rep;
}

}  // namespace msynth
