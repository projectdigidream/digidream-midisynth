#include "music_director.h"

namespace msynth {

void MusicDirector::fade_bank(u8 bank, gain_t target, i32 blocks) {
    for (int ch = 0; ch < NUM_MIDI_CHANNELS; ++ch) {
        EngineCommand c;
        c.type = EngineCommand::Type::ChannelFade;
        c.slot = static_cast<u8>((bank ? 16 : 0) + ch);
        c.v = target;
        c.v2 = blocks;
        engine_->queue().push(c);
    }
}

SongLoadReport MusicDirector::crossfade_to(const u8* smf, size_t len, u32 fade_ms) {
    SongLoadReport rep = inactive().load(smf, len);
    begin_crossfade(fade_ms);
    return rep;
}

SongLoadReport MusicDirector::crossfade_to_parsed(ParsedSong&& parsed, u32 fade_ms) {
    SongLoadReport rep = inactive().load_parsed(std::move(parsed));
    begin_crossfade(fade_ms);
    return rep;
}

void MusicDirector::begin_crossfade(u32 fade_ms) {
    const u8 old_bank = static_cast<u8>(active_);
    const u8 new_bank = static_cast<u8>(active_ ^ 1);
    i32 blocks = static_cast<i32>(fade_ms * CONTROL_RATE / 1000);
    if (blocks < 1) blocks = 1;

    // Outgoing voices lose steal protection -- they're fading out anyway.
    EngineCommand c;
    c.type = EngineCommand::Type::SetOutgoingBank;
    c.d1 = old_bank;
    engine_->queue().push(c);

    // New song's channels start silent and ramp up; old song's ramp down.
    fade_bank(new_bank, 0, 1);
    fade_bank(new_bank, GAIN_ONE, blocks);
    fade_bank(old_bank, 0, blocks);

    inactive().set_loop_enabled(true);  // game default: music loops
    inactive().play();
    active_ ^= 1;
    fade_blocks_left_ = blocks;
}

void MusicDirector::queue_crossfade_at_next_bar(const u8* smf, size_t len, u32 fade_ms) {
    pending_smf_.assign(smf, smf + len);
    pending_fade_ms_ = fade_ms;
    Sequencer& seq = active();
    const u32 target_tick = seq.song().next_bar_tick(seq.current_tick());
    pending_at_song_sample_ = seq.song().parsed.tempo.tick_to_sample(target_tick);
    pending_ = true;
}

void MusicDirector::advance(u32 frames) {
    // Fire a pending musical-boundary transition once the active song
    // reaches (or wraps past) the boundary.
    if (pending_) {
        const u64 pos = active().song_pos_samples();
        if (pos >= pending_at_song_sample_ || !active().playing()) {
            pending_ = false;
            crossfade_to(pending_smf_.data(), pending_smf_.size(), pending_fade_ms_);
        }
    }

    const u64 now = engine_->now();
    seq_[0].advance(now, frames);
    seq_[1].advance(now, frames);

    if (fade_blocks_left_ > 0) {
        fade_blocks_left_ -= frames / CONTROL_BLOCK;
        if (fade_blocks_left_ <= 0) {
            fade_blocks_left_ = 0;
            // Fade complete: silence and stop the outgoing song for good.
            Sequencer& out = inactive();
            out.stop();
            EngineCommand c;
            c.type = EngineCommand::Type::SetOutgoingBank;
            c.d1 = 0xFF;
            engine_->queue().push(c);
        }
    }
}

}  // namespace msynth
