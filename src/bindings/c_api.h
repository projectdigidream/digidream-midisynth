#ifndef MIDISYNTH_C_API_H
#define MIDISYNTH_C_API_H
/*
 * Plain C API for native engine integration (plan §2 bindings/). Mirrors the
 * embind surface: one opaque context owns content, engine and music director.
 *
 * Threading contract: msynth_render() is the audio-thread entry point; every
 * other call is control-thread. Control calls communicate with rendering
 * through the internal lock-free queue, EXCEPT the loaders (bank/drums/song/
 * config), which must not run concurrently with msynth_render() -- call them
 * from the audio thread's context or while rendering is paused (same rule the
 * AudioWorklet build follows).
 *
 * Loader-style calls return 0 on success, nonzero on error, and write a
 * newline-separated warning/error report into `report` (truncated to
 * `report_cap`, always NUL-terminated when report_cap > 0; pass NULL/0 to
 * skip).
 *
 * Fixed-point boundary: "milli" parameters are thousandths (1000 = 1.0).
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct msynth_ctx msynth_ctx;

msynth_ctx* msynth_create(void);
void msynth_destroy(msynth_ctx* ctx);

/* ---- Content (control thread, not concurrent with render) ---- */
int msynth_load_bank_json(msynth_ctx* ctx, const char* json, size_t len,
                          char* report, size_t report_cap);
int msynth_load_drums_json(msynth_ctx* ctx, const char* json, size_t len,
                           char* report, size_t report_cap);
int msynth_load_song(msynth_ctx* ctx, const uint8_t* smf, size_t len,
                     char* report, size_t report_cap);
int msynth_load_song_config_json(msynth_ctx* ctx, const char* json, size_t len,
                                 char* report, size_t report_cap);
int msynth_crossfade_to(msynth_ctx* ctx, const uint8_t* smf, size_t len,
                        uint32_t fade_ms, char* report, size_t report_cap);
/* Load a baked .mstk track: embedded bank/kit/fx/song replace the current
 * content in one call (the deliverable path for games). */
int msynth_load_track(msynth_ctx* ctx, const uint8_t* track, size_t len,
                      char* report, size_t report_cap);
/* Crossfade into a baked track's song. The currently loaded bank/kit stay:
 * the incoming track must share them (slices cut from one parent always do;
 * a content-hash mismatch is reported as a warning, not an error). */
int msynth_crossfade_track(msynth_ctx* ctx, const uint8_t* track, size_t len,
                           uint32_t fade_ms, char* report, size_t report_cap);

/* ---- Transport / music direction (queued; any thread) ---- */
void msynth_play(msynth_ctx* ctx);
void msynth_pause(msynth_ctx* ctx);
void msynth_stop(msynth_ctx* ctx);
void msynth_seek_ms(msynth_ctx* ctx, uint32_t ms);
void msynth_set_loop(msynth_ctx* ctx, int enabled);
void msynth_set_loop_ticks(msynth_ctx* ctx, uint32_t start_tick, uint32_t end_tick);
void msynth_set_tempo_scale_milli(msynth_ctx* ctx, int milli);   /* 250..4000 */
void msynth_set_master_volume_milli(msynth_ctx* ctx, int milli);
void msynth_channel_mute(msynth_ctx* ctx, int channel, int muted, uint32_t fade_ms);
void msynth_duck_milli(msynth_ctx* ctx, int target_milli, uint32_t ramp_ms);
void msynth_set_dither(msynth_ctx* ctx, int enabled);

/* ---- Live MIDI-style input (queued) ---- */
void msynth_note_on(msynth_ctx* ctx, int channel, int note, int velocity);
void msynth_note_off(msynth_ctx* ctx, int channel, int note);
void msynth_cc(msynth_ctx* ctx, int channel, int controller, int value);
void msynth_program_change(msynth_ctx* ctx, int channel, int program);
void msynth_pitch_bend(msynth_ctx* ctx, int channel, int value /* -8192..8191 */);
void msynth_channel_aftertouch(msynth_ctx* ctx, int channel, int pressure);

/* ---- Rendering (audio thread) ----
 * Interleaved stereo int16 at 48000 Hz; frames must be a multiple of 64. */
void msynth_render(msynth_ctx* ctx, int16_t* out_interleaved, int frames);

/* ---- Position / diagnostics ---- */
int msynth_playing(const msynth_ctx* ctx);
uint32_t msynth_time_ms(const msynth_ctx* ctx);
uint32_t msynth_duration_ms(const msynth_ctx* ctx);
void msynth_bar_beat(const msynth_ctx* ctx, uint32_t* bar, uint32_t* beat);
int msynth_active_voices(const msynth_ctx* ctx);
int msynth_peak_voices(const msynth_ctx* ctx);
int64_t msynth_steal_count(const msynth_ctx* ctx);
int64_t msynth_dropped_notes(const msynth_ctx* ctx);

/* Beat callback: fired from within msynth_render (audio thread!) whenever the
 * bar/beat changes. Keep the callback trivial or NULL it. */
typedef void (*msynth_beat_cb)(void* user, uint32_t bar, uint32_t beat);
void msynth_set_beat_callback(msynth_ctx* ctx, msynth_beat_cb cb, void* user);

#ifdef __cplusplus
}
#endif
#endif /* MIDISYNTH_C_API_H */
