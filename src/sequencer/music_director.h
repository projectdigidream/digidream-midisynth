#pragma once
//
// Game-facing music control (plan §10): two sequencer instances over one
// engine for crossfades, channel mute fades, ducking, tempo scaling,
// musical-boundary transitions. Control thread only.
//
#include "sequencer.h"

namespace msynth {

class MusicDirector {
  public:
    explicit MusicDirector(SynthEngine& engine) : engine_(&engine) {
        seq_[0].attach(&engine, 0);
        seq_[1].attach(&engine, 1);
    }

    // ---- Playback lifecycle ----
    SongLoadReport load_song(const u8* smf, size_t len) {
        return active().load(smf, len);
    }
    void play() { active().play(); }
    void pause() { active().pause(); }
    void stop() { active().stop(); }
    void seek_ms(u32 ms) { active().seek_ms(ms); }
    void set_loop_enabled(bool on) { active().set_loop_enabled(on); }
    void set_loop_ticks(u32 s, u32 e) { active().set_loop_ticks(s, e); }
    void set_tempo_scale(gain_t rate_q16) {
        seq_[0].set_tempo_scale(rate_q16);
        seq_[1].set_tempo_scale(rate_q16);
    }

    void set_master_volume(gain_t g) {
        EngineCommand c;
        c.type = EngineCommand::Type::MasterGain;
        c.v = g;
        engine_->queue().push(c);
    }

    // ---- Interactive layers ----
    void set_channel_mute(u8 channel, bool muted, u32 fade_ms) {
        EngineCommand c;
        c.type = EngineCommand::Type::ChannelFade;
        c.slot = static_cast<u8>((active_ ? 16 : 0) + (channel & 15));
        c.v = muted ? 0 : GAIN_ONE;
        c.v2 = static_cast<i32>(fade_ms * CONTROL_RATE / 1000);
        engine_->queue().push(c);
    }

    // ---- Ducking ----
    void duck(gain_t target_q16, u32 ramp_ms) {
        EngineCommand c;
        c.type = EngineCommand::Type::Duck;
        c.v = target_q16;
        c.v2 = static_cast<i32>(ramp_ms * CONTROL_RATE / 1000);
        engine_->queue().push(c);
    }

    // ---- Crossfade ----
    SongLoadReport crossfade_to(const u8* smf, size_t len, u32 fade_ms);
    // Baked-track path: crossfade into a pre-parsed song. NOTE: the engine's
    // loaded bank/kit stay as they are -- the incoming song must share them
    // (slices cut from the same parent track always do; compare
    // TrackMeta::content_hash otherwise).
    SongLoadReport crossfade_to_parsed(ParsedSong&& parsed, u32 fade_ms);
    // Defer the crossfade to the next bar boundary of the current song.
    void queue_crossfade_at_next_bar(const u8* smf, size_t len, u32 fade_ms);

    // ---- Cue transitions (horizontal resequencing) ----
    enum class TransitionMode : u8 {
        AtLoopEnd,   // hard switch exactly when the current piece's loop ends
        AtNextBar,   // hard switch on the next bar line
        Immediate,   // crossfade now (fade_ms > 0) or hard load (fade_ms == 0)
    };
    // Content stays loaded: the incoming song must share it (sibling slices
    // do). Queued modes leave the current song untouched until the boundary.
    void transition(ParsedSong&& parsed, TransitionMode mode, u32 fade_ms);
    void cancel_transition() noexcept { active().cancel_queued(); pending_ = false; }
    bool transition_queued() const noexcept {
        return const_cast<MusicDirector*>(this)->active().queued() || pending_;
    }
    u32 ms_to_boundary() const noexcept {
        return static_cast<u32>(
            const_cast<MusicDirector*>(this)->active().samples_to_boundary() * 1000 / SAMPLE_RATE);
    }
    void set_piece_callback(Sequencer::PieceCallback cb, void* user) noexcept {
        seq_[0].set_piece_callback(cb, user);
        seq_[1].set_piece_callback(cb, user);
    }

    // ---- Live editing passthrough ----
    void replace_song(ParsedSong&& parsed, const ReplaceOptions& opt = {}) {
        active().replace_song_keep_position(std::move(parsed), opt);
    }

    bool crossfading() const noexcept { return fade_blocks_left_ > 0; }

    // ---- Drive (call once per render buffer, before engine.render) ----
    void advance(u32 frames);

    // ---- Position queries ----
    u32 time_ms() const { return const_cast<MusicDirector*>(this)->active().time_ms(); }
    void bar_beat(u32& bar, u32& beat) const {
        const_cast<MusicDirector*>(this)->active().bar_beat(bar, beat);
    }
    void set_beat_callback(Sequencer::BeatCallback cb, void* user) {
        seq_[0].set_beat_callback(cb, user);
        seq_[1].set_beat_callback(cb, user);
    }
    Sequencer& active() noexcept { return seq_[active_]; }
    Sequencer& inactive() noexcept { return seq_[active_ ^ 1]; }

  private:
    void fade_bank(u8 bank, gain_t target, i32 blocks);
    void begin_crossfade(u32 fade_ms);

    SynthEngine* engine_;
    Sequencer seq_[2];
    int active_ = 0;
    i64 fade_blocks_left_ = 0;

    // Pending musical-boundary transition.
    std::vector<u8> pending_smf_;
    u32 pending_fade_ms_ = 0;
    bool pending_ = false;
    u64 pending_at_song_sample_ = 0;
};

}  // namespace msynth
