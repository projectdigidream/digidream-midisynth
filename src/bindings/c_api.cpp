#include "c_api.h"

#include <cstring>
#include <string>

#include "../sequencer/music_director.h"
#include "../synth/patch_loader.h"
#include "../synth/song_config.h"
#include "../track/track.h"

using namespace msynth;

struct msynth_ctx {
    WaveTableCache cache;
    PatchBank bank;
    DrumKit kit;
    SynthEngine engine;
    MusicDirector director;
    bool has_content_hash = false;
    u8 content_hash[32] = {};

    msynth_ctx() : director(engine) {
        tables_init();
        make_builtin_fallback(cache, bank.fallback());
        engine.set_content(&bank, &kit);
    }
};

namespace {

void write_report(char* report, size_t cap, const std::vector<std::string>& warnings,
                  const std::vector<std::string>& errors) {
    if (!report || cap == 0) return;
    std::string joined;
    for (const auto& e : errors) joined += "error: " + e + "\n";
    for (const auto& w : warnings) joined += "warning: " + w + "\n";
    const size_t n = joined.size() < cap - 1 ? joined.size() : cap - 1;
    std::memcpy(report, joined.data(), n);
    report[n] = '\0';
}

u8 live_slot(const msynth_ctx* ctx, int ch) {
    return static_cast<u8>((const_cast<msynth_ctx*>(ctx)->director.active().bank() ? 16 : 0) +
                           (ch & 15));
}

void push_cmd(msynth_ctx* ctx, EngineCommand c) { ctx->engine.queue().push(c); }

// Voices hold pointers into the bank/kit being replaced: fast-release them
// and render a few silent blocks so the ramps finish BEFORE the swap frees
// the old content (same rule as the embind build).
void release_and_drain(msynth_ctx* ctx) {
    ctx->engine.fast_release_all_voices();
    i16 tmp[CONTROL_BLOCK * 2];
    for (int i = 0; i < 4; ++i) ctx->engine.render(tmp, CONTROL_BLOCK);
}

void push_note_cmd(msynth_ctx* ctx, EngineCommand::Type t, int ch, int d1, int d2) {
    EngineCommand c;
    c.type = t;
    c.slot = live_slot(ctx, ch);
    c.d1 = static_cast<u8>(d1 & 127);
    c.d2 = static_cast<u8>(d2 & 127);
    push_cmd(ctx, c);
}

}  // namespace

extern "C" {

msynth_ctx* msynth_create(void) { return new msynth_ctx(); }
void msynth_destroy(msynth_ctx* ctx) { delete ctx; }

int msynth_load_bank_json(msynth_ctx* ctx, const char* json, size_t len, char* report,
                          size_t cap) {
    PatchBank fresh;
    LoadResult r = load_patch_bank(json, len, ctx->cache, fresh);
    write_report(report, cap, r.warnings, r.errors);
    if (!r.ok) return -1;
    release_and_drain(ctx);
    ctx->bank = std::move(fresh);
    ctx->engine.set_content(&ctx->bank, &ctx->kit);
    return 0;
}

int msynth_load_drums_json(msynth_ctx* ctx, const char* json, size_t len, char* report,
                           size_t cap) {
    DrumKit fresh;
    LoadResult r = load_drum_kit(json, len, ctx->cache, fresh);
    write_report(report, cap, r.warnings, r.errors);
    if (!r.ok) return -1;
    release_and_drain(ctx);
    ctx->kit = std::move(fresh);
    ctx->engine.set_content(&ctx->bank, &ctx->kit);
    return 0;
}

int msynth_load_song(msynth_ctx* ctx, const uint8_t* smf, size_t len, char* report,
                     size_t cap) {
    SongLoadReport r = ctx->director.load_song(smf, len);
    write_report(report, cap, r.warnings, r.errors);
    return r.ok ? 0 : -1;
}

int msynth_load_song_config_json(msynth_ctx* ctx, const char* json, size_t len, char* report,
                                 size_t cap) {
    SongFxConfig cfg;
    LoadResult r = load_song_config(json, len, cfg);
    write_report(report, cap, r.warnings, r.errors);
    if (!r.ok) return -1;
    push_song_config(ctx->engine, cfg);
    return 0;
}

int msynth_load_track(msynth_ctx* ctx, const uint8_t* track, size_t len, char* report,
                      size_t cap) {
    TrackData data;
    LoadResult r = load_track(track, len, data);
    if (!r.ok) {
        write_report(report, cap, r.warnings, r.errors);
        return -1;
    }
    PatchBank bank;
    DrumKit kit;
    LoadResult br = load_patch_bank(data.bank_json.data(), data.bank_json.size(), ctx->cache, bank);
    LoadResult kr = load_drum_kit(data.kit_json.data(), data.kit_json.size(), ctx->cache, kit);
    for (auto& w : br.warnings) r.warn("bank: " + w);
    for (auto& w : kr.warnings) r.warn("kit: " + w);
    if (!br.ok || !kr.ok) {
        for (auto& e : br.errors) r.error("bank: " + e);
        for (auto& e : kr.errors) r.error("kit: " + e);
        write_report(report, cap, r.warnings, r.errors);
        return -1;
    }
    release_and_drain(ctx);
    ctx->bank = std::move(bank);
    ctx->kit = std::move(kit);
    ctx->engine.set_content(&ctx->bank, &ctx->kit);
    push_song_config(ctx->engine, data.fx);
    std::memcpy(ctx->content_hash, data.meta.content_hash, 32);
    ctx->has_content_hash = true;
    ctx->director.active().load_parsed(std::move(data.song));
    write_report(report, cap, r.warnings, r.errors);
    return 0;
}

int msynth_crossfade_track(msynth_ctx* ctx, const uint8_t* track, size_t len, uint32_t fade_ms,
                           char* report, size_t cap) {
    TrackData data;
    LoadResult r = load_track(track, len, data);
    if (!r.ok) {
        write_report(report, cap, r.warnings, r.errors);
        return -1;
    }
    // Content stays; warn when the incoming track was baked against
    // different bank/kit content.
    if (ctx->has_content_hash &&
        std::memcmp(ctx->content_hash, data.meta.content_hash, 32) != 0)
        r.warn("incoming track was baked with different bank/kit content; "
               "it will play with the currently loaded instruments");
    ctx->director.crossfade_to_parsed(std::move(data.song), fade_ms);
    write_report(report, cap, r.warnings, r.errors);
    return 0;
}

int msynth_crossfade_to(msynth_ctx* ctx, const uint8_t* smf, size_t len, uint32_t fade_ms,
                        char* report, size_t cap) {
    SongLoadReport r = ctx->director.crossfade_to(smf, len, fade_ms);
    write_report(report, cap, r.warnings, r.errors);
    return r.ok ? 0 : -1;
}

void msynth_play(msynth_ctx* ctx) { ctx->director.play(); }
void msynth_pause(msynth_ctx* ctx) { ctx->director.pause(); }
void msynth_stop(msynth_ctx* ctx) { ctx->director.stop(); }
void msynth_seek_ms(msynth_ctx* ctx, uint32_t ms) { ctx->director.seek_ms(ms); }
void msynth_set_loop(msynth_ctx* ctx, int enabled) { ctx->director.set_loop_enabled(enabled != 0); }
void msynth_set_loop_ticks(msynth_ctx* ctx, uint32_t s, uint32_t e) {
    ctx->director.set_loop_ticks(s, e);
}
void msynth_set_tempo_scale_milli(msynth_ctx* ctx, int milli) {
    ctx->director.set_tempo_scale(static_cast<gain_t>(static_cast<i64>(milli) * GAIN_ONE / 1000));
}
void msynth_set_master_volume_milli(msynth_ctx* ctx, int milli) {
    ctx->director.set_master_volume(
        static_cast<gain_t>(static_cast<i64>(milli) * GAIN_ONE / 1000));
}
void msynth_channel_mute(msynth_ctx* ctx, int channel, int muted, uint32_t fade_ms) {
    ctx->director.set_channel_mute(static_cast<u8>(channel), muted != 0, fade_ms);
}
void msynth_duck_milli(msynth_ctx* ctx, int target_milli, uint32_t ramp_ms) {
    ctx->director.duck(static_cast<gain_t>(static_cast<i64>(target_milli) * GAIN_ONE / 1000),
                       ramp_ms);
}
void msynth_set_dither(msynth_ctx* ctx, int enabled) {
    EngineCommand c;
    c.type = EngineCommand::Type::DitherEnable;
    c.d1 = enabled ? 1 : 0;
    push_cmd(ctx, c);
}

void msynth_note_on(msynth_ctx* ctx, int ch, int note, int vel) {
    push_note_cmd(ctx, EngineCommand::Type::NoteOn, ch, note, vel);
}
void msynth_note_off(msynth_ctx* ctx, int ch, int note) {
    push_note_cmd(ctx, EngineCommand::Type::NoteOff, ch, note, 0);
}
void msynth_cc(msynth_ctx* ctx, int ch, int controller, int value) {
    push_note_cmd(ctx, EngineCommand::Type::CC, ch, controller, value);
}
void msynth_program_change(msynth_ctx* ctx, int ch, int program) {
    push_note_cmd(ctx, EngineCommand::Type::Program, ch, program, 0);
}
void msynth_pitch_bend(msynth_ctx* ctx, int ch, int value) {
    EngineCommand c;
    c.type = EngineCommand::Type::PitchBend;
    c.slot = live_slot(ctx, ch);
    c.v = clamp_i32(value, -8192, 8191);
    push_cmd(ctx, c);
}
void msynth_channel_aftertouch(msynth_ctx* ctx, int ch, int pressure) {
    push_note_cmd(ctx, EngineCommand::Type::Aftertouch, ch, pressure, 0);
}

void msynth_render(msynth_ctx* ctx, int16_t* out, int frames) {
    ctx->director.advance(static_cast<u32>(frames));
    ctx->engine.render(out, frames);
}

int msynth_playing(const msynth_ctx* ctx) {
    return const_cast<msynth_ctx*>(ctx)->director.active().playing() ? 1 : 0;
}
uint32_t msynth_time_ms(const msynth_ctx* ctx) { return ctx->director.time_ms(); }
uint32_t msynth_duration_ms(const msynth_ctx* ctx) {
    return static_cast<uint32_t>(
        const_cast<msynth_ctx*>(ctx)->director.active().song().parsed.end_sample * 1000 /
        SAMPLE_RATE);
}
void msynth_bar_beat(const msynth_ctx* ctx, uint32_t* bar, uint32_t* beat) {
    u32 b = 0, t = 0;
    ctx->director.bar_beat(b, t);
    if (bar) *bar = b;
    if (beat) *beat = t;
}
int msynth_active_voices(const msynth_ctx* ctx) { return ctx->engine.pool_diag().active_voices; }
int msynth_peak_voices(const msynth_ctx* ctx) { return ctx->engine.pool_diag().peak_voices; }
int64_t msynth_steal_count(const msynth_ctx* ctx) { return ctx->engine.pool_diag().steal_count; }
int64_t msynth_dropped_notes(const msynth_ctx* ctx) {
    return ctx->engine.pool_diag().dropped_notes;
}

void msynth_set_beat_callback(msynth_ctx* ctx, msynth_beat_cb cb, void* user) {
    ctx->director.set_beat_callback(cb, user);
}

}  // extern "C"
