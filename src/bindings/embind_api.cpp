//
// JS-facing API (embind). One WebSynth object owns the whole stack: content,
// engine, director. Designed to live INSIDE the AudioWorklet's global scope
// (module imported by the worklet processor); the main thread talks to it
// through the worklet's MessagePort, so no SharedArrayBuffer / COOP-COEP is
// needed -- the plan's §12 goal, achieved without -sAUDIO_WORKLET.
//
// Content loading happens between process() callbacks (worklet thread, not
// during rendering) -- fine for an authoring tool; a game preloads before
// starting playback.
//
#include <emscripten/bind.h>

#include <cstring>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "../output/fit.h"
#include "../output/sha256.h"
#include "../sequencer/music_director.h"
#include "../synth/patch_loader.h"
#include "../track/track.h"
#include "../midi/smf_writer.h"
#include "../synth/song_config.h"

using namespace msynth;

namespace {

// JSON string literal with escapes (content documents can hold anything).
std::string json_string(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += "\"";
    return out;
}

std::string report_to_json(bool ok, const std::vector<std::string>& warnings,
                           const std::vector<std::string>& errors) {
    auto esc = [](const std::string& s) {
        std::string out;
        for (char c : s) {
            if (c == '"' || c == '\\') out += '\\';
            if (c == '\n') { out += "\\n"; continue; }
            out += c;
        }
        return out;
    };
    std::string j = std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"warnings\":[";
    for (size_t i = 0; i < warnings.size(); ++i)
        j += (i ? ",\"" : "\"") + esc(warnings[i]) + "\"";
    j += "],\"errors\":[";
    for (size_t i = 0; i < errors.size(); ++i) j += (i ? ",\"" : "\"") + esc(errors[i]) + "\"";
    return j + "]}";
}

class WebSynth {
  public:
    WebSynth() : director_(engine_) {
        tables_init();
        director_.set_piece_callback([](void* u) { ++static_cast<WebSynth*>(u)->piece_switches_; },
                                     this);
        make_builtin_fallback(cache_, bank_.fallback());
        engine_.set_content(&bank_, &kit_);
        render_buf_.resize(BUFFER_FRAMES * 2);
    }

    // ---- Content (JSON as UTF-8 strings) ----
    std::string load_bank(const std::string& json) {
        PatchBank fresh;
        LoadResult r = load_patch_bank(json.data(), json.size(), cache_, fresh);
        if (r.ok) {
            // Silence voices holding pointers into the old bank, then swap.
            engine_.queue().push(all_off_cmd(0));
            engine_.queue().push(all_off_cmd(1));
            drain_one_block();
            bank_ = std::move(fresh);
            engine_.set_content(&bank_, &kit_);
        }
        return report_to_json(r.ok, r.warnings, r.errors);
    }
    // Hot-swap a single program without touching the rest of the bank:
    // only voices on the edited patch get a fast release (authoring UX --
    // knob tweaks don't silence the song).
    std::string load_program(int prog, const std::string& json) {
        std::string err;
        JsonPtr doc = json_parse(json.data(), json.size(), &err);
        if (!doc || !doc->is_obj())
            return report_to_json(false, {}, {"JSON parse failed: " + err});
        auto patch = std::make_unique<Patch>();
        LoadResult r = load_patch(*doc, cache_, *patch);
        if (r.ok) {
            const u8 p = static_cast<u8>(prog & 127);
            if (const Patch* old = bank_.exact(p)) {
                engine_.fast_release_patch(old);
                drain_one_block();
            }
            bank_.set_program(p, std::move(patch));
        }
        return report_to_json(r.ok, r.warnings, r.errors);
    }
    std::string load_drum_note(int note, const std::string& json) {
        std::string err;
        JsonPtr doc = json_parse(json.data(), json.size(), &err);
        if (!doc || !doc->is_obj())
            return report_to_json(false, {}, {"JSON parse failed: " + err});
        auto patch = std::make_unique<Patch>();
        LoadResult r = load_patch(*doc, cache_, *patch);
        if (r.ok) {
            const u8 n = static_cast<u8>(note & 127);
            if (const Patch* old = kit_.lookup(n)) {
                engine_.fast_release_patch(old);
                drain_one_block();
            }
            kit_.set_note(n, std::move(patch));
        }
        return report_to_json(r.ok, r.warnings, r.errors);
    }

    std::string load_drums(const std::string& json) {
        DrumKit fresh;
        LoadResult r = load_drum_kit(json.data(), json.size(), cache_, fresh);
        if (r.ok) {
            engine_.queue().push(all_off_cmd(0));
            engine_.queue().push(all_off_cmd(1));
            drain_one_block();
            kit_ = std::move(fresh);
            engine_.set_content(&bank_, &kit_);
        }
        return report_to_json(r.ok, r.warnings, r.errors);
    }

    // ---- Song bytes: JS writes into the prepared buffer via HEAPU8 ----
    uintptr_t song_buffer_prepare(int size) {
        song_buf_.assign(static_cast<size_t>(size), 0);
        return reinterpret_cast<uintptr_t>(song_buf_.data());
    }
    std::string load_song_from_buffer() {
        if (is_track(song_buf_.data(), song_buf_.size())) return load_track_from_buffer();
        SongLoadReport r = director_.load_song(song_buf_.data(), song_buf_.size());
        return report_to_json(r.ok, r.warnings, r.errors);
    }
    std::string load_song_config(const std::string& json) {
        SongFxConfig cfg;
        LoadResult r = ::load_song_config(json.data(), json.size(), cfg);
        if (r.ok) push_song_config(engine_, cfg);
        return report_to_json(r.ok, r.warnings, r.errors);
    }
    // Baked track: song_buf_ holds the .mstk bytes.
    std::string load_track_from_buffer() {
        TrackData data;
        LoadResult r = ::load_track(song_buf_.data(), song_buf_.size(), data);
        if (r.ok) {
            PatchBank fresh_bank;
            DrumKit fresh_kit;
            LoadResult br = load_patch_bank(data.bank_json.data(), data.bank_json.size(),
                                            cache_, fresh_bank);
            LoadResult kr = load_drum_kit(data.kit_json.data(), data.kit_json.size(),
                                          cache_, fresh_kit);
            for (auto& w : br.warnings) r.warn("bank: " + w);
            for (auto& w : kr.warnings) r.warn("kit: " + w);
            if (br.ok && kr.ok) {
                engine_.queue().push(all_off_cmd(0));
                engine_.queue().push(all_off_cmd(1));
                drain_one_block();
                bank_ = std::move(fresh_bank);
                kit_ = std::move(fresh_kit);
                engine_.set_content(&bank_, &kit_);
                push_song_config(engine_, data.fx);
                last_track_bank_ = data.bank_json;
                last_track_kit_ = data.kit_json;
                last_track_fx_ = data.fx;
                std::memcpy(content_hash_, data.meta.content_hash, 32);
                has_content_hash_ = true;
                loaded_track_name_ = data.meta.name;
                director_.active().load_parsed(std::move(data.song));
            } else {
                for (auto& err2 : br.errors) r.error("bank: " + err2);
                for (auto& err2 : kr.errors) r.error("kit: " + err2);
            }
        }
        return report_to_json(r.ok, r.warnings, r.errors);
    }
    // Bake: song_buf_ holds SMF bytes; content comes as JSON strings. The
    // result waits in baked_ for the caller to copy out.
    std::string bake_track_from_buffer(const std::string& bank_json,
                                       const std::string& kit_json,
                                       const std::string& fx_json,
                                       const std::string& name, int verify_seconds) {
        BakeOptions opts;
        opts.name = name;
        if (!fx_json.empty()) {
            opts.fx_json = fx_json.data();
            opts.fx_json_len = fx_json.size();
        }
        opts.verify_seconds = static_cast<u32>(verify_seconds < 0 ? 0 : verify_seconds);
        LoadResult r = ::bake_track(song_buf_.data(), song_buf_.size(), bank_json.data(),
                                    bank_json.size(), kit_json.data(), kit_json.size(), opts,
                                    baked_);
        return report_to_json(r.ok, r.warnings, r.errors);
    }
    uintptr_t baked_track_ptr() { return reinterpret_cast<uintptr_t>(baked_.data()); }
    int baked_track_size() { return static_cast<int>(baked_.size()); }

    // Embedded content of the last loaded .mstk (DAW import): bank/kit JSON
    // text plus the resolved fx + master as a song-config document.
    std::string track_content_json() {
        auto q = [](i32 v) { return master_to_json_number(v); };
        std::string fx = "{\"version\":1,\"reverb\":{\"roomSize\":" + q(last_track_fx_.reverb.room_size) +
                         ",\"damping\":" + q(last_track_fx_.reverb.damping) +
                         ",\"width\":" + q(last_track_fx_.reverb.width) +
                         ",\"wetGain\":" + q(last_track_fx_.reverb.wet_gain) +
                         "},\"chorus\":{\"rateHz\":" + q(last_track_fx_.chorus.rate_hz_q16) +
                         ",\"depthMs\":" + q(last_track_fx_.chorus.depth_ms_q16) +
                         ",\"wetGain\":" + q(last_track_fx_.chorus.wet_gain) +
                         "},\"master\":" + master_to_json(last_track_fx_.master, false) + "}";
        return "{\"name\":" + json_string(loaded_track_name_) + ",\"bankJson\":" + json_string(last_track_bank_) +
               ",\"kitJson\":" + json_string(last_track_kit_) + ",\"fx\":" + fx + "}";
    }

    // ---- Mastering ----
    void master_param(int id, int v) {
        if (id < 0 || id >= static_cast<int>(MasterParamId::COUNT)) return;
        EngineCommand c;
        c.type = EngineCommand::Type::MasterParam;
        c.d1 = static_cast<u8>(id);
        c.v = v;
        engine_.queue().push(c);
    }
    std::string load_master_config(const std::string& json) {
        MasterParams p;
        LoadResult r = ::load_master_config(json.data(), json.size(), p);
        if (r.ok) push_master_config(engine_, p);
        return report_to_json(r.ok, r.warnings, r.errors);
    }
    // Current chain parameters as master.json text (export).
    std::string master_json() { return master_to_json(engine_.master().params(), true); }
    // Level meters since the last call, as JSON. msL/msR are mean squares in
    // units of 2^32 per 1.0^2 (JS: rms = sqrt(ms / 2^32)).
    std::string take_meters_json() {
        const MasterMeters m = engine_.take_master_meters();
        const u64 ms_l = m.samples ? m.sumsq_l / m.samples : 0;
        const u64 ms_r = m.samples ? m.sumsq_r / m.samples : 0;
        std::string s = "{\"peakL\":" + std::to_string(m.peak_l) +
                        ",\"peakR\":" + std::to_string(m.peak_r) +
                        ",\"msL\":" + std::to_string(ms_l) + ",\"msR\":" + std::to_string(ms_r) +
                        ",\"samples\":" + std::to_string(m.samples) +
                        ",\"compGr\":" + std::to_string(m.comp_gr) +
                        ",\"limGain\":" + std::to_string(m.lim_gain) +
                        ",\"clips\":" + std::to_string(m.clips) +
                        ",\"enabled\":" + (engine_.master().enabled() ? "true" : "false") +
                        ",\"latency\":" + std::to_string(engine_.master().latency_samples()) + "}";
        return s;
    }

    // ---- DAW support ----
    // Parse song_buf_ (.mid or .mstk -> song only) into a ParsedSong.
    bool parse_song_buffer(ParsedSong& out, LoadResult& r) {
        if (is_track(song_buf_.data(), song_buf_.size())) {
            TrackData td;
            LoadResult lr = ::load_track(song_buf_.data(), song_buf_.size(), td);
            r.warnings = lr.warnings;
            if (!lr.ok) { r.errors = lr.errors; r.ok = false; return false; }
            if (has_content_hash_ && std::memcmp(content_hash_, td.meta.content_hash, 32) != 0)
                r.warn("track was baked with different content; playing with loaded instruments");
            out = std::move(td.song);
            return true;
        }
        ParseResult pr = smf_parse(song_buf_.data(), song_buf_.size(), out);
        r.warnings = pr.warnings;
        if (!pr.ok) { r.errors = pr.errors; r.ok = false; return false; }
        return true;
    }
    // Live edit: swap the song keeping position (voice/controller diff repair).
    std::string replace_song_from_buffer() {
        ParsedSong parsed;
        LoadResult r;
        if (!parse_song_buffer(parsed, r)) return report_to_json(false, r.warnings, r.errors);
        director_.replace_song(std::move(parsed));
        return report_to_json(true, r.warnings, r.errors);
    }
    // Cue transition: mode 0 = at loop end, 1 = at next bar, 2 = immediate.
    std::string queue_track_from_buffer(int mode, int fade_ms) {
        ParsedSong parsed;
        LoadResult r;
        if (!parse_song_buffer(parsed, r)) return report_to_json(false, r.warnings, r.errors);
        const auto m = mode == 1 ? MusicDirector::TransitionMode::AtNextBar
                     : mode == 2 ? MusicDirector::TransitionMode::Immediate
                                 : MusicDirector::TransitionMode::AtLoopEnd;
        director_.transition(std::move(parsed), m, static_cast<u32>(fade_ms < 0 ? 0 : fade_ms));
        return report_to_json(true, r.warnings, r.errors);
    }
    void cancel_queued() { director_.cancel_transition(); }
    bool queued() { return director_.transition_queued(); }
    int ms_to_boundary() { return static_cast<int>(director_.ms_to_boundary()); }
    int piece_switches() { return piece_switches_; }
    // Export the active song as SMF into baked_ (reuse the pointer/size getters).
    std::string export_smf() {
        LoadResult r = smf_write(director_.active().song().parsed, baked_);
        return report_to_json(r.ok, r.warnings, r.errors);
    }

    // ---- Cutter support ----
    // Timeline info for the ACTIVE song (works for .mid and .mstk loads):
    // bar grid (ticks + samples), duration, loop, and a 240-bucket note-on
    // density strip for the timeline visualization.
    std::string track_info_json() {
        const Song& s = director_.active().song();
        const ParsedSong& p = s.parsed;
        std::string j = "{\"name\":\"" + loaded_track_name_ + "\"";
        j += ",\"durationMs\":" + std::to_string(p.end_sample * 1000 / SAMPLE_RATE);
        j += ",\"endTick\":" + std::to_string(p.end_tick);
        j += ",\"loopStartTick\":" + std::to_string(s.loop_start_sample ? p.loop_start_tick : 0);
        j += ",\"loopEndTick\":" + std::to_string(p.loop_end_tick);
        j += ",\"barTicks\":[";
        std::string samples = "],\"barSamples\":[";
        u32 tick = 0;
        int guard = 0;
        bool first = true;
        while (tick < p.end_tick && guard++ < 20000) {
            if (!first) { j += ','; samples += ','; }
            first = false;
            j += std::to_string(tick);
            samples += std::to_string(p.tempo.tick_to_sample(tick));
            const u32 next = s.next_bar_tick(tick);
            if (next <= tick) break;
            tick = next;
        }
        j += samples + "],\"density\":[";
        constexpr int BUCKETS = 240;
        int density[BUCKETS] = {};
        if (p.end_sample > 0)
            for (const MidiEvent& ev : p.events)
                if (ev.is_channel() && ev.kind() == 0x90 && ev.d2 > 0) {
                    int b = static_cast<int>(ev.sample * BUCKETS / (p.end_sample + 1));
                    density[b < BUCKETS ? b : BUCKETS - 1]++;
                }
        for (int i = 0; i < BUCKETS; ++i) j += (i ? "," : "") + std::to_string(density[i]);
        j += "]}";
        return j;
    }
    // Cut [startTick, endTick) of the track in song_buf_ into baked_.
    std::string cut_track_from_buffer(int start_tick, int end_tick, const std::string& name) {
        CutOptions opts;
        opts.name = name;
        LoadResult r = ::cut_track(song_buf_.data(), song_buf_.size(),
                                   static_cast<u32>(start_tick < 0 ? 0 : start_tick),
                                   static_cast<u32>(end_tick < 0 ? 0 : end_tick), opts, baked_);
        return report_to_json(r.ok, r.warnings, r.errors);
    }
    // Crossfade into the track in song_buf_ (content stays loaded; slices
    // from the same parent share it -- mismatches warn).
    std::string crossfade_track_from_buffer(int fade_ms) {
        TrackData data;
        LoadResult r = ::load_track(song_buf_.data(), song_buf_.size(), data);
        if (!r.ok) return report_to_json(false, r.warnings, r.errors);
        if (has_content_hash_ &&
            std::memcmp(content_hash_, data.meta.content_hash, 32) != 0)
            r.warn("incoming track was baked with different content; playing with "
                   "the currently loaded instruments");
        director_.crossfade_to_parsed(std::move(data.song), static_cast<u32>(fade_ms));
        return report_to_json(true, r.warnings, r.errors);
    }

    std::string crossfade_from_buffer(int fade_ms) {
        // .mstk accepted wherever .mid is; the track path also adds the
        // content-hash mismatch warning.
        if (is_track(song_buf_.data(), song_buf_.size()))
            return crossfade_track_from_buffer(fade_ms);
        SongLoadReport r = director_.crossfade_to(song_buf_.data(), song_buf_.size(),
                                                  static_cast<u32>(fade_ms));
        return report_to_json(r.ok, r.warnings, r.errors);
    }

    // ---- Transport ----
    void play() { director_.play(); }
    void pause() { director_.pause(); }
    void stop() { director_.stop(); }
    void seek_ms(int ms) { director_.seek_ms(static_cast<u32>(ms)); }
    void set_loop(bool on) { director_.set_loop_enabled(on); }
    void set_loop_ticks(int start_tick, int end_tick) {
        director_.set_loop_ticks(static_cast<u32>(start_tick < 0 ? 0 : start_tick),
                                 static_cast<u32>(end_tick < 0 ? 0 : end_tick));
    }
    void set_tempo_scale_milli(int milli) {
        director_.set_tempo_scale(static_cast<gain_t>(
            static_cast<i64>(milli) * GAIN_ONE / 1000));
    }
    void set_master_volume_milli(int milli) {
        director_.set_master_volume(static_cast<gain_t>(static_cast<i64>(milli) * GAIN_ONE / 1000));
    }
    void channel_mute(int ch, bool muted, int fade_ms) {
        director_.set_channel_mute(static_cast<u8>(ch), muted, static_cast<u32>(fade_ms));
    }
    void duck_milli(int milli, int ramp_ms) {
        director_.duck(static_cast<gain_t>(static_cast<i64>(milli) * GAIN_ONE / 1000),
                       static_cast<u32>(ramp_ms));
    }
    void set_dither(bool on) {
        EngineCommand c;
        c.type = EngineCommand::Type::DitherEnable;
        c.d1 = on ? 1 : 0;
        engine_.queue().push(c);
    }

    // ---- Live input (authoring tool: Web MIDI / virtual keyboard) ----
    // Channel slots 0..15 = the active song bank's channels.
    void note_on(int ch, int note, int vel) {
        push_cmd(EngineCommand::Type::NoteOn, ch, note, vel);
    }
    void note_off(int ch, int note) { push_cmd(EngineCommand::Type::NoteOff, ch, note, 0); }
    void cc(int ch, int num, int val) { push_cmd(EngineCommand::Type::CC, ch, num, val); }
    void program_change(int ch, int prog) {
        push_cmd(EngineCommand::Type::Program, ch, prog, 0);
    }
    void pitch_bend(int ch, int value) {  // -8192..8191
        EngineCommand c;
        c.type = EngineCommand::Type::PitchBend;
        c.slot = live_slot(ch);
        c.v = value;
        engine_.queue().push(c);
    }
    void aftertouch(int ch, int pressure) {
        push_cmd(EngineCommand::Type::Aftertouch, ch, pressure, 0);
    }

    // ---- Effects config (song-level, from the tool) ----
    void reverb_param(int id, int milli) { fx_param(EngineCommand::Type::ReverbConfig, id, milli); }
    void chorus_param(int id, int milli) { fx_param(EngineCommand::Type::ChorusConfig, id, milli); }

    // ---- Render: `frames` interleaved i16 stereo; returns heap pointer ----
    uintptr_t render(int frames) {
        if (frames <= 0) return 0;
        if (static_cast<size_t>(frames) * 2 > render_buf_.size())
            render_buf_.resize(static_cast<size_t>(frames) * 2);
        director_.advance(static_cast<u32>(frames));
        engine_.render(render_buf_.data(), frames);
        return reinterpret_cast<uintptr_t>(render_buf_.data());
    }

    // ---- Position / diagnostics ----
    bool playing() { return director_.active().playing(); }
    int time_ms() { return static_cast<int>(director_.time_ms()); }
    int bar() {
        u32 b, t;
        director_.bar_beat(b, t);
        return static_cast<int>(b);
    }
    int beat() {
        u32 b, t;
        director_.bar_beat(b, t);
        return static_cast<int>(t);
    }
    int duration_ms() {
        return static_cast<int>(director_.active().song().parsed.end_sample * 1000 / SAMPLE_RATE);
    }
    int active_voices() { return engine_.pool_diag().active_voices; }
    int peak_voices() { return engine_.pool_diag().peak_voices; }
    double steal_count() { return static_cast<double>(engine_.pool_diag().steal_count); }
    double dropped_notes() { return static_cast<double>(engine_.pool_diag().dropped_notes); }
    int queue_depth() { return static_cast<int>(engine_.queue().size()); }
    int channel_notes(int ch) { return engine_.active_note_count(live_slot(ch)); }
    int channel_program(int ch) { return engine_.channel(live_slot(ch)).program; }
    std::string active_note_list(int ch) {
        u8 buf[128];
        const int n = engine_.active_notes(live_slot(ch), buf, 128);
        std::string s;
        for (int i = 0; i < n; ++i) {
            if (i) s += ',';
            s += std::to_string(buf[i]);
        }
        return s;
    }

  private:
    u8 live_slot(int ch) {
        return static_cast<u8>((director_.active().bank() ? 16 : 0) + (ch & 15));
    }
    void push_cmd(EngineCommand::Type t, int ch, int d1, int d2) {
        EngineCommand c;
        c.type = t;
        c.slot = live_slot(ch);
        c.d1 = static_cast<u8>(d1 & 127);
        c.d2 = static_cast<u8>(d2 & 127);
        engine_.queue().push(c);
    }
    void fx_param(EngineCommand::Type t, int id, int milli) {
        EngineCommand c;
        c.type = t;
        c.d1 = static_cast<u8>(id);
        c.v = static_cast<i32>(static_cast<i64>(milli) * GAIN_ONE / 1000);
        engine_.queue().push(c);
    }
    static EngineCommand all_off_cmd(int bank) {
        EngineCommand c;
        c.type = EngineCommand::Type::AllSoundOffBank;
        c.d1 = static_cast<u8>(bank);
        return c;
    }
    // Render a few silent blocks so queued kill commands apply and the fast
    // releases finish before content pointers are swapped.
    void drain_one_block() {
        i16 tmp[CONTROL_BLOCK * 2];
        for (int i = 0; i < 4; ++i) engine_.render(tmp, CONTROL_BLOCK);
    }

    WaveTableCache cache_;
    PatchBank bank_;
    DrumKit kit_;
    SynthEngine engine_;
    MusicDirector director_;
    std::vector<u8> song_buf_;
    std::vector<u8> baked_;
    bool has_content_hash_ = false;
    u8 content_hash_[32] = {};
    std::string loaded_track_name_ = "song";
    std::string last_track_bank_, last_track_kit_;
    SongFxConfig last_track_fx_;
    int piece_switches_ = 0;
    std::vector<i16> render_buf_;
};

}  // namespace


// ---------------------------------------------------------------------------
// Patch fitting for the DAW's Fit panel. Same optimiser and loss as
// midisynth_fit, so a fit done in the browser equals a fit done on the CLI.
// One restart per call: the caller drives the loop so it can show progress
// and stay responsive.
// ---------------------------------------------------------------------------
class PatchFitter {
  public:
    PatchFitter() = default;

    // JS writes float samples (48 kHz mono) into the heap and passes the ptr.
    void setTarget(uintptr_t ptr, int len) {
        const float* p = reinterpret_cast<const float*>(ptr);
        target_.assign(p, p + std::max(0, len));
        raw_ = target_;
    }

    std::string archList() const {
        std::string s = "[";
        bool first = true;
        for (const auto& a : msynth::fit::architectures()) {
            if (!first) s += ",";
            first = false;
            s += "{\"name\":\"" + std::string(a.name) + "\",\"seconds\":" +
                 std::to_string(a.seconds) + ",\"params\":[";
            for (size_t i = 0; i < a.params.size(); ++i) {
                const auto& pm = a.params[i];
                if (i) s += ",";
                s += "{\"name\":\"" + std::string(pm.name) + "\",\"lo\":" +
                     std::to_string(pm.lo) + ",\"hi\":" + std::to_string(pm.hi) +
                     ",\"log\":" + (pm.log ? "true" : "false") + "}";
            }
            s += "]}";
        }
        return s + "]";
    }

    // window_ms 0 = the architecture's own length. A recording's room tail is
    // not something the synth can reproduce, so fitting it wastes the budget.
    bool configure(std::string arch, float stft, float env, float bands, float window_ms) {
        const auto all = msynth::fit::architectures();
        arch_ = -1;
        for (size_t i = 0; i < all.size(); ++i)
            if (arch == all[i].name) arch_ = static_cast<int>(i);
        if (arch_ < 0) return false;
        archs_ = all;
        w_.stft = stft;
        w_.env = env;
        w_.bands = bands;
        const float secs = window_ms > 0 ? window_ms / 1000.0f : archs_[arch_].seconds;
        frames_ = static_cast<size_t>(secs * SAMPLE_RATE);
        target_ = raw_;
        msynth::fit::prepare(target_, frames_);
        rend_ = std::make_unique<msynth::fit::Renderer>(secs);
        return true;
    }

    // One restart. Returns JSON with the patch and the loss broken out per
    // term, so the panel can show which part of the objective is unhappy.
    std::string fitOnce(int iters, int seed) {
        if (arch_ < 0 || !rend_) return "{\"error\":\"no architecture\"}";
        float loss = 0;
        const auto u = msynth::fit::fit_once(archs_[arch_], target_, *rend_, iters,
                                             static_cast<unsigned>(seed), loss, w_);
        const std::string js = msynth::fit::build_json(archs_[arch_], u);
        std::vector<float> got;
        rend_->render(js, got);
        const auto parts = msynth::fit::loss_parts(target_, got, w_);
        std::string s = "{\"loss\":" + std::to_string(parts.total) +
                        ",\"stft\":" + std::to_string(parts.stft) +
                        ",\"env\":" + std::to_string(parts.env) +
                        ",\"bands\":" + std::to_string(parts.bands) + ",\"params\":{";
        for (size_t i = 0; i < u.size(); ++i) {
            if (i) s += ",";
            s += "\"" + std::string(archs_[arch_].params[i].name) + "\":" +
                 std::to_string(msynth::fit::denorm(archs_[arch_].params[i], u[i]));
        }
        return s + "},\"patch\":" + js + "}";
    }

    // Band profiles of target and candidate, for the preview plot.
    std::string analyze(std::string patch_json) {
        std::vector<float> got;
        if (arch_ < 0 || !rend_ || !rend_->render(patch_json, got)) return "{\"error\":\"render failed\"}";
        const auto parts = msynth::fit::loss_parts(target_, got, w_);
        const auto ta = msynth::fit::band_profile(target_), ca = msynth::fit::band_profile(got);
        auto arr = [](const std::vector<float>& v) {
            std::string s = "[";
            for (size_t i = 0; i < v.size(); ++i) { if (i) s += ","; s += std::to_string(v[i]); }
            return s + "]";
        };
        return "{\"loss\":" + std::to_string(parts.total) + ",\"stft\":" +
               std::to_string(parts.stft) + ",\"env\":" + std::to_string(parts.env) +
               ",\"bands\":" + std::to_string(parts.bands) + ",\"target\":" + arr(ta) +
               ",\"candidate\":" + arr(ca) + "}";
    }

    // Render a candidate into the heap for audition / waveform preview.
    int renderTo(std::string patch_json, uintptr_t ptr, int max_len) {
        std::vector<float> got;
        if (!rend_ || !rend_->render(patch_json, got)) return 0;
        const int n = std::min<int>(max_len, static_cast<int>(got.size()));
        std::copy(got.begin(), got.begin() + n, reinterpret_cast<float*>(ptr));
        return n;
    }
    int targetTo(uintptr_t ptr, int max_len) {
        const int n = std::min<int>(max_len, static_cast<int>(target_.size()));
        std::copy(target_.begin(), target_.begin() + n, reinterpret_cast<float*>(ptr));
        return n;
    }
    int frames() const { return static_cast<int>(frames_); }

  private:
    std::vector<msynth::fit::Arch> archs_;
    int arch_ = -1;
    std::vector<float> raw_, target_;
    msynth::fit::LossWeights w_;
    std::unique_ptr<msynth::fit::Renderer> rend_;
    size_t frames_ = 0;
};

EMSCRIPTEN_BINDINGS(midisynth) {
    emscripten::class_<WebSynth>("WebSynth")
        .constructor<>()
        .function("loadBank", &WebSynth::load_bank)
        .function("loadDrums", &WebSynth::load_drums)
        .function("loadProgram", &WebSynth::load_program)
        .function("loadDrumNote", &WebSynth::load_drum_note)
        .function("songBufferPrepare", &WebSynth::song_buffer_prepare)
        .function("loadSongFromBuffer", &WebSynth::load_song_from_buffer)
        .function("crossfadeFromBuffer", &WebSynth::crossfade_from_buffer)
        .function("loadSongConfig", &WebSynth::load_song_config)
        .function("loadTrackFromBuffer", &WebSynth::load_track_from_buffer)
        .function("bakeTrackFromBuffer", &WebSynth::bake_track_from_buffer)
        .function("bakedTrackPtr", &WebSynth::baked_track_ptr)
        .function("bakedTrackSize", &WebSynth::baked_track_size)
        .function("trackInfoJson", &WebSynth::track_info_json)
        .function("trackContentJson", &WebSynth::track_content_json)
        .function("masterParam", &WebSynth::master_param)
        .function("loadMasterConfig", &WebSynth::load_master_config)
        .function("masterJson", &WebSynth::master_json)
        .function("takeMetersJson", &WebSynth::take_meters_json)
        .function("replaceSongFromBuffer", &WebSynth::replace_song_from_buffer)
        .function("queueTrackFromBuffer", &WebSynth::queue_track_from_buffer)
        .function("cancelQueued", &WebSynth::cancel_queued)
        .function("queued", &WebSynth::queued)
        .function("msToBoundary", &WebSynth::ms_to_boundary)
        .function("pieceSwitches", &WebSynth::piece_switches)
        .function("exportSmf", &WebSynth::export_smf)
        .function("cutTrackFromBuffer", &WebSynth::cut_track_from_buffer)
        .function("crossfadeTrackFromBuffer", &WebSynth::crossfade_track_from_buffer)
        .function("play", &WebSynth::play)
        .function("pause", &WebSynth::pause)
        .function("stop", &WebSynth::stop)
        .function("seekMs", &WebSynth::seek_ms)
        .function("setLoop", &WebSynth::set_loop)
        .function("setLoopTicks", &WebSynth::set_loop_ticks)
        .function("setTempoScaleMilli", &WebSynth::set_tempo_scale_milli)
        .function("setMasterVolumeMilli", &WebSynth::set_master_volume_milli)
        .function("channelMute", &WebSynth::channel_mute)
        .function("duckMilli", &WebSynth::duck_milli)
        .function("setDither", &WebSynth::set_dither)
        .function("noteOn", &WebSynth::note_on)
        .function("noteOff", &WebSynth::note_off)
        .function("cc", &WebSynth::cc)
        .function("programChange", &WebSynth::program_change)
        .function("pitchBend", &WebSynth::pitch_bend)
        .function("aftertouch", &WebSynth::aftertouch)
        .function("reverbParam", &WebSynth::reverb_param)
        .function("chorusParam", &WebSynth::chorus_param)
        .function("render", &WebSynth::render)
        .function("playing", &WebSynth::playing)
        .function("timeMs", &WebSynth::time_ms)
        .function("bar", &WebSynth::bar)
        .function("beat", &WebSynth::beat)
        .function("durationMs", &WebSynth::duration_ms)
        .function("activeVoices", &WebSynth::active_voices)
        .function("peakVoices", &WebSynth::peak_voices)
        .function("stealCount", &WebSynth::steal_count)
        .function("droppedNotes", &WebSynth::dropped_notes)
        .function("queueDepth", &WebSynth::queue_depth)
        .function("channelNotes", &WebSynth::channel_notes)
        .function("channelProgram", &WebSynth::channel_program)
        .function("activeNoteList", &WebSynth::active_note_list);

    emscripten::class_<PatchFitter>("PatchFitter")
        .constructor<>()
        .function("setTarget", &PatchFitter::setTarget)
        .function("archList", &PatchFitter::archList)
        .function("configure", &PatchFitter::configure)
        .function("fitOnce", &PatchFitter::fitOnce)
        .function("analyze", &PatchFitter::analyze)
        .function("renderTo", &PatchFitter::renderTo)
        .function("targetTo", &PatchFitter::targetTo)
        .function("frames", &PatchFitter::frames);
}
