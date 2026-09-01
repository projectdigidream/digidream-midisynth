#include "sequencer.h"

#include <algorithm>

namespace msynth {

SongLoadReport Sequencer::load(const u8* smf_data, size_t len) {
    ParsedSong parsed;
    ParseResult pr = smf_parse(smf_data, len, parsed);
    if (!pr.ok) parsed = ParsedSong{};  // error policy: silent song, no crash
    SongLoadReport rep = load_parsed(std::move(parsed));
    rep.warnings = pr.warnings;
    rep.errors = pr.errors;
    rep.ok = pr.ok;
    if (!pr.ok) song_.loaded = false;  // play() stays a no-op
    return rep;
}

SongLoadReport Sequencer::load_parsed(ParsedSong&& parsed) {
    SongLoadReport rep;
    rep.ok = true;
    playing_ = false;
    song_ = Song{};
    song_.parsed = std::move(parsed);
    song_.finalize();
    song_pos_ = 0;
    frac_acc_ = 0;
    cursor_ = 0;
    last_beat_key_ = 0xFFFFFFFFu;
    if (engine_) {
        EngineCommand c;
        c.type = EngineCommand::Type::AllSoundOffBank;
        c.d1 = bank_;
        push(c, 0);
        c.type = EngineCommand::Type::ResetBank;
        push(c, 0);
    }
    return rep;
}

void Sequencer::set_loop_ticks(u32 start_tick, u32 end_tick) {
    song_.parsed.loop_start_tick = start_tick;
    song_.parsed.loop_end_tick = end_tick;
    song_.parsed.has_loop_start = song_.parsed.has_loop_end = true;
    song_.finalize();
}

void Sequencer::stop() noexcept {
    playing_ = false;
    song_pos_ = 0;
    frac_acc_ = 0;
    cursor_ = 0;
    last_beat_key_ = 0xFFFFFFFFu;
    if (engine_) {
        EngineCommand c;
        c.type = EngineCommand::Type::AllSoundOffBank;
        c.d1 = bank_;
        push(c, 0);
    }
}

void Sequencer::push(EngineCommand c, u64 when) {
    c.when = when;
    engine_->queue().push(c);
}

void Sequencer::dispatch(const MidiEvent& e, u64 when) {
    if (!e.is_channel()) return;  // metas are position data, not commands
    EngineCommand c;
    c.slot = static_cast<u8>((bank_ ? 16 : 0) + e.channel());
    switch (e.kind()) {
        case 0x90:
            c.type = EngineCommand::Type::NoteOn;
            c.d1 = e.d1;
            c.d2 = e.d2;
            break;
        case 0x80:
            c.type = EngineCommand::Type::NoteOff;
            c.d1 = e.d1;
            break;
        case 0xB0:
            c.type = EngineCommand::Type::CC;
            c.d1 = e.d1;
            c.d2 = e.d2;
            break;
        case 0xC0:
            c.type = EngineCommand::Type::Program;
            c.d1 = e.d1;
            break;
        case 0xD0:
            c.type = EngineCommand::Type::Aftertouch;
            c.d1 = e.d1;
            break;
        case 0xE0:
            c.type = EngineCommand::Type::PitchBend;
            c.v = (static_cast<i32>(e.d2) << 7 | e.d1) - 8192;
            break;
        default:
            return;
    }
    push(c, when);
}

void Sequencer::chase_to(u64 song_sample) {
    // After a seek, every channel's program/CC/bend state must be what it
    // would have been had the song played from the start (plan §10). Kill
    // sound, reset, then replay all non-note state changes.
    EngineCommand c;
    c.type = EngineCommand::Type::AllSoundOffBank;
    c.d1 = bank_;
    push(c, 0);
    c.type = EngineCommand::Type::ResetBank;
    push(c, 0);

    const auto& ev = song_.parsed.events;
    size_t i = 0;
    for (; i < ev.size() && ev[i].sample < song_sample; ++i) {
        const MidiEvent& e = ev[i];
        if (!e.is_channel()) continue;
        const u8 kind = e.kind();
        if (kind == 0x90 || kind == 0x80) continue;  // skip all note events
        if (kind == 0xB0 && (e.d1 == 120 || e.d1 == 123)) continue;
        dispatch(e, 0);
    }
    cursor_ = i;
    song_pos_ = song_sample;
    frac_acc_ = 0;
}

void Sequencer::seek_ms(u32 ms) {
    if (!song_.loaded || !engine_) return;
    u64 target = static_cast<u64>(ms) * SAMPLE_RATE / 1000;
    if (target > song_.parsed.end_sample) target = song_.parsed.end_sample;
    chase_to(target);
}

void Sequencer::advance(u64 engine_now, u32 frames) {
    if (!playing_ || !engine_ || !song_.loaded) return;

    const auto& ev = song_.parsed.events;

    // Song samples covered by this buffer at the current rate.
    const u64 adv_fp = static_cast<u64>(frames) * static_cast<u32>(rate_q16_) + frac_acc_;
    u64 remaining = adv_fp >> GAIN_SHIFT;
    frac_acc_ = static_cast<u32>(adv_fp & 0xFFFFu);

    // inv_rate: engine samples per song sample, Q16.
    const u64 inv_rate = (static_cast<u64>(GAIN_ONE) << GAIN_SHIFT) / static_cast<u32>(rate_q16_);
    u64 engine_off_fp = 0;  // engine samples consumed so far, Q16

    while (remaining > 0) {
        u64 window_end = song_pos_ + remaining;
        bool looping = false;
        if (loop_enabled_ && song_.loop_end_sample > song_pos_ &&
            window_end >= song_.loop_end_sample) {
            window_end = song_.loop_end_sample;
            looping = true;
        }

        // Dispatch events in [song_pos_, window_end).
        while (cursor_ < ev.size() && ev[cursor_].sample < window_end) {
            const MidiEvent& e = ev[cursor_];
            if (e.sample >= song_pos_) {
                const u64 delta_engine =
                    ((e.sample - song_pos_) * inv_rate + engine_off_fp) >> GAIN_SHIFT;
                dispatch(e, engine_now + delta_engine);
            }
            ++cursor_;
        }

        const u64 span = window_end - song_pos_;
        engine_off_fp += span * inv_rate;
        remaining -= span;
        song_pos_ = window_end;

        if (looping) {
            // Sample-accurate seamless jump: cursor and position move, voices
            // and effect tails keep ringing (plan §9).
            song_pos_ = song_.loop_start_sample;
            cursor_ = song_.loop_start_index;
            continue;
        }
        if (song_pos_ >= song_.parsed.end_sample && !loop_enabled_) {
            playing_ = false;
            break;
        }
    }

    // Beat callback (fired from the control thread, plan §10).
    if (beat_cb_) {
        u32 bar, beat;
        bar_beat(bar, beat);
        const u32 key = (bar << 8) | (beat & 0xFF);
        if (key != last_beat_key_) {
            last_beat_key_ = key;
            beat_cb_(beat_cb_user_, bar, beat);
        }
    }
}

}  // namespace msynth
