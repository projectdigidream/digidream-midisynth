#pragma once
//
// Sequencer: walks a Song's baked-sample event list and pushes timestamped
// commands into the engine queue (plan §9/§10). Control-thread code; the
// audio thread only ever sees the queue.
//
// The clock is virtual: position derives from rendered sample count (never
// wall clock -- web AudioContext time can jump), and a tempo-scale multiplier
// stretches event scheduling without repitching (Appendix E).
//
#include "../synth/synth_engine.h"
#include "song.h"

namespace msynth {

struct SongLoadReport {
    bool ok = false;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

class Sequencer {
  public:
    using BeatCallback = void (*)(void* user, u32 bar, u32 beat);

    void attach(SynthEngine* engine, u8 bank) noexcept {
        engine_ = engine;
        bank_ = bank;
    }

    SongLoadReport load(const u8* smf_data, size_t len);
    // Baked-track path: adopt an already-parsed song (sample positions and
    // tempo anchors come straight from the .mstk file).
    SongLoadReport load_parsed(ParsedSong&& parsed);
    // Explicit loop override from the game (priority 1 in plan §9).
    void set_loop_ticks(u32 start_tick, u32 end_tick);

    void play() noexcept { playing_ = song_.loaded; }
    void pause() noexcept { playing_ = false; }
    void stop() noexcept;
    bool playing() const noexcept { return playing_; }

    void seek_ms(u32 ms);
    void set_loop_enabled(bool on) noexcept { loop_enabled_ = on; }
    // Playback-rate multiplier, Q16.16 (1.0 = normal). Clamped [1/4, 4].
    void set_tempo_scale(gain_t rate_q16) noexcept {
        rate_q16_ = clamp_i32(rate_q16, GAIN_ONE / 4, 4 * GAIN_ONE);
    }

    // Drive: schedule everything landing in [engine_now, engine_now+frames).
    // Call once per render buffer, before SynthEngine::render.
    void advance(u64 engine_now, u32 frames);

    // Position queries (game sync).
    u32 time_ms() const noexcept {
        return static_cast<u32>(song_pos_ * 1000 / SAMPLE_RATE);
    }
    u64 song_pos_samples() const noexcept { return song_pos_; }
    u32 current_tick() const noexcept { return song_.parsed.tempo.sample_to_tick(song_pos_); }
    void bar_beat(u32& bar, u32& beat) const noexcept {
        song_.bar_beat_at_tick(current_tick(), bar, beat);
    }
    void set_beat_callback(BeatCallback cb, void* user) noexcept {
        beat_cb_ = cb;
        beat_cb_user_ = user;
    }

    const Song& song() const noexcept { return song_; }
    Song& song_mutable() noexcept { return song_; }
    u8 bank() const noexcept { return bank_; }

  private:
    void chase_to(u64 song_sample);  // controller-state chase (plan §10)
    void dispatch(const MidiEvent& e, u64 when);
    void push(EngineCommand c, u64 when);

    SynthEngine* engine_ = nullptr;
    u8 bank_ = 0;
    Song song_;
    bool playing_ = false;
    bool loop_enabled_ = false;
    gain_t rate_q16_ = GAIN_ONE;

    u64 song_pos_ = 0;      // song samples
    u32 frac_acc_ = 0;      // Q16 fractional song-sample accumulator
    size_t cursor_ = 0;     // next event index

    BeatCallback beat_cb_ = nullptr;
    void* beat_cb_user_ = nullptr;
    u32 last_beat_key_ = 0xFFFFFFFFu;
};

}  // namespace msynth
