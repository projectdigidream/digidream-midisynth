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

struct ReplaceOptions {
    bool keep_loop_override = true;   // re-apply set_loop_ticks() to the new song
    bool restrike_new_notes = true;   // notes now straddling the playhead are struck
    bool chase_controllers = true;    // apply program/CC/bend differences at the playhead
};

struct QueueAt {
    bool at_loop_end = true;  // else: at `sample` (song position)
    u64 sample = 0;
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

    // ---- Live editing: swap the song under a running transport ----
    // Keeps playing/loop/rate state and the musical position, recomputes the
    // cursor, and repairs voice + controller state by DIFFING old vs new song
    // at the playhead (normal note releases, never All Sound Off). Control
    // thread; never concurrent with render().
    void replace_song_keep_position(ParsedSong&& parsed, const ReplaceOptions& opt = {});

    // ---- Cue scheduling: switch to another piece at a boundary ----
    // The pending song replaces this one exactly when the current piece
    // reaches its loop end (or song end when not looping) -- inside the same
    // advance() call, so the new piece's first events land on the boundary
    // sample. Notes still open at the boundary get a normal release.
    void queue_next(ParsedSong&& parsed, QueueAt at, bool loop_new = true);
    void cancel_queued() noexcept { has_pending_ = false; pending_ = ParsedSong{}; }
    bool queued() const noexcept { return has_pending_; }
    // Song samples until the next boundary (loop end / song end / queued
    // sample), for UI countdowns. 0 if nothing is loaded.
    u64 samples_to_boundary() const noexcept;

    using PieceCallback = void (*)(void* user);
    void set_piece_callback(PieceCallback cb, void* user) noexcept {
        piece_cb_ = cb;
        piece_cb_user_ = user;
    }

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
    void push_tempo_at(u64 song_sample);  // tempo feed for synced LFOs
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
    gain_t last_tempo_bpm_q16_ = 0;

    // set_loop_ticks() memory, re-applied across replace_song_keep_position.
    bool loop_override_ = false;
    u32 ov_start_tick_ = 0, ov_end_tick_ = 0;

    // Cue scheduling.
    bool has_pending_ = false;
    ParsedSong pending_;
    QueueAt pending_at_;
    bool pending_loop_ = true;
    PieceCallback piece_cb_ = nullptr;
    void* piece_cb_user_ = nullptr;

    void adopt_pending_at_boundary();  // called from advance() at the boundary
    void release_open_notes_at(u64 song_sample);
};

}  // namespace msynth
