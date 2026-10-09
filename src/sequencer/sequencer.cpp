#include "sequencer.h"

#include <algorithm>

#include "chase_state.h"

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
    loop_override_ = false;
    cancel_queued();
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
    loop_override_ = true;
    ov_start_tick_ = start_tick;
    ov_end_tick_ = end_tick;
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
            c.d2 = e.d2;  // release velocity
            break;
        case 0xA0:
            c.type = EngineCommand::Type::PolyAftertouch;
            c.d1 = e.d1;
            c.d2 = e.d2;
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

// Feed the current tempo to the engine (tempo-synced LFOs). Idempotent: only
// pushes when the value changes.
void Sequencer::push_tempo_at(u64 song_sample) {
    if (!engine_ || !song_.loaded) return;
    const auto& segs = song_.parsed.tempo.segments();
    u32 uspq = 500000;
    for (const auto& s : segs)
        if (s.start_sample <= song_sample) uspq = s.us_per_quarter; else break;
    if (uspq == 0) uspq = 500000;
    const gain_t bpm_q16 = static_cast<gain_t>((60000000ULL << GAIN_SHIFT) / uspq);
    if (bpm_q16 == last_tempo_bpm_q16_) return;
    last_tempo_bpm_q16_ = bpm_q16;
    EngineCommand c;
    c.type = EngineCommand::Type::TempoBpm;
    c.v = bpm_q16;
    push(c, 0);
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
    push_tempo_at(song_sample);
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

    push_tempo_at(song_pos_);

    while (remaining > 0) {
        u64 window_end = song_pos_ + remaining;
        bool looping = false;
        if (loop_enabled_ && song_.loop_end_sample > song_pos_ &&
            window_end >= song_.loop_end_sample) {
            window_end = song_.loop_end_sample;
            looping = true;
        }
        if (has_pending_ && !pending_at_.at_loop_end && pending_at_.sample > song_pos_ &&
            window_end > pending_at_.sample) {
            window_end = pending_at_.sample;
            looping = false;
        }
        // A non-looping piece with a queued successor: stop the window at
        // the song end so the switch lands exactly there.
        if (has_pending_ && !loop_enabled_ && song_.parsed.end_sample > song_pos_ &&
            window_end > song_.parsed.end_sample)
            window_end = song_.parsed.end_sample;

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
            // A queued sample at/after the loop end can never be reached by a
            // looping piece; the loop end IS that boundary.
            if (has_pending_ && (pending_at_.at_loop_end ||
                                 pending_at_.sample >= song_.loop_end_sample)) {
                // Cue change: the queued piece takes over exactly here, and
                // the rest of this buffer already plays it.
                adopt_pending_at_boundary();
                continue;
            }
            // Sample-accurate seamless jump: cursor and position move, voices
            // and effect tails keep ringing (plan §9).
            song_pos_ = song_.loop_start_sample;
            cursor_ = song_.loop_start_index;
            continue;
        }
        if (has_pending_ && !pending_at_.at_loop_end && song_pos_ >= pending_at_.sample) {
            adopt_pending_at_boundary();
            continue;
        }
        if (song_pos_ >= song_.parsed.end_sample && !loop_enabled_) {
            if (has_pending_) {  // end of a non-looping piece is a boundary too
                adopt_pending_at_boundary();
                continue;
            }
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

// ---------------------------------------------------------------------------
// Live editing.

void Sequencer::release_open_notes_at(u64 song_sample) {
    SongChase st;
    collect_chase(song_.parsed, true, song_sample, 0, st);
    for (int ch = 0; ch < 16; ++ch)
        for (int n = 0; n < 128; ++n)
            if (st.open[ch][n] > 0) {
                EngineCommand c;
                c.type = EngineCommand::Type::NoteOff;
                c.slot = static_cast<u8>((bank_ ? 16 : 0) + ch);
                c.d1 = static_cast<u8>(n);
                push(c, 0);
            }
}

void Sequencer::replace_song_keep_position(ParsedSong&& parsed, const ReplaceOptions& opt) {
    if (!engine_) {
        song_ = Song{};
        song_.parsed = std::move(parsed);
        song_.finalize();
        return;
    }
    // 1) Musical position: identical tempo maps keep the sample position
    //    exactly; a tempo edit maps through ticks instead.
    u64 new_pos = song_pos_;
    {
        const auto& a = song_.parsed.tempo.segments();
        const auto& b = parsed.tempo.segments();
        bool same = a.size() == b.size();
        for (size_t i = 0; same && i < a.size(); ++i)
            same = a[i].start_tick == b[i].start_tick &&
                   a[i].samples_per_tick_fp == b[i].samples_per_tick_fp;
        if (!same) new_pos = parsed.tempo.tick_to_sample(song_.parsed.tempo.sample_to_tick(song_pos_));
    }

    // 2) Note-state diff at the playhead: normal releases for notes that no
    //    longer exist, optional strikes for notes that newly straddle it.
    SongChase old_st, new_st;
    collect_chase(song_.parsed, true, song_pos_, 0, old_st);
    collect_chase(parsed, true, new_pos, 0, new_st);
    for (int ch = 0; ch < 16; ++ch) {
        const u8 slot = static_cast<u8>((bank_ ? 16 : 0) + ch);
        for (int n = 0; n < 128; ++n) {
            if (old_st.open[ch][n] > 0 && new_st.open[ch][n] == 0) {
                EngineCommand c;
                c.type = EngineCommand::Type::NoteOff;
                c.slot = slot;
                c.d1 = static_cast<u8>(n);
                push(c, 0);
            } else if (opt.restrike_new_notes && new_st.open[ch][n] > 0 && old_st.open[ch][n] == 0) {
                // Velocity of the last straddling note-on in the new song.
                u8 vel = 100;
                for (const MidiEvent& e : parsed.events) {
                    if (e.sample >= new_pos) break;
                    if (e.is_channel() && e.channel() == ch && e.kind() == 0x90 && e.d1 == n) vel = e.d2;
                }
                EngineCommand c;
                c.type = EngineCommand::Type::NoteOn;
                c.slot = slot;
                c.d1 = static_cast<u8>(n);
                c.d2 = vel;
                push(c, 0);
            }
        }
        // 3) Controller diff: only what changed, in the chase order.
        if (opt.chase_controllers) {
            const ChannelChase& o = old_st.ch[ch];
            const ChannelChase& w = new_st.ch[ch];
            if (w.program >= 0 && w.program != o.program) {
                EngineCommand c;
                c.type = EngineCommand::Type::Program;
                c.slot = slot;
                c.d1 = static_cast<u8>(w.program);
                push(c, 0);
            }
            for (int cc = 0; cc < 128; ++cc) {
                if (chase_is_plumbing_cc(static_cast<u8>(cc))) continue;
                if (w.cc[cc] >= 0 && w.cc[cc] != o.cc[cc]) {
                    EngineCommand c;
                    c.type = EngineCommand::Type::CC;
                    c.slot = slot;
                    c.d1 = static_cast<u8>(cc);
                    c.d2 = static_cast<u8>(w.cc[cc]);
                    push(c, 0);
                }
            }
            if (w.bend_range_semis >= 0 && w.bend_range_semis != o.bend_range_semis) {
                const u8 seq[][2] = {{101, 0}, {100, 0}, {6, static_cast<u8>(w.bend_range_semis)},
                                     {101, 127}, {100, 127}};
                for (const auto& p : seq) {
                    EngineCommand c;
                    c.type = EngineCommand::Type::CC;
                    c.slot = slot;
                    c.d1 = p[0];
                    c.d2 = p[1];
                    push(c, 0);
                }
            }
            const int nb = w.bend >= 0 ? w.bend : 8192;
            const int ob = o.bend >= 0 ? o.bend : 8192;
            if (nb != ob) {
                EngineCommand c;
                c.type = EngineCommand::Type::PitchBend;
                c.slot = slot;
                c.v = nb - 8192;
                push(c, 0);
            }
        }
    }

    // 4) Swap, keep the loop override, re-derive, reposition the cursor.
    song_ = Song{};
    song_.parsed = std::move(parsed);
    if (loop_override_ && opt.keep_loop_override) {
        song_.parsed.loop_start_tick = ov_start_tick_;
        song_.parsed.loop_end_tick = ov_end_tick_;
        song_.parsed.has_loop_start = song_.parsed.has_loop_end = true;
    }
    song_.finalize();
    song_pos_ = new_pos;
    if (song_pos_ > song_.parsed.end_sample) song_pos_ = song_.parsed.end_sample;
    if (loop_enabled_ && song_.loop_end_sample > song_.loop_start_sample &&
        song_pos_ >= song_.loop_end_sample)
        song_pos_ = song_.loop_start_sample;  // loop shortened past the playhead: wrap now
    const auto& ev = song_.parsed.events;
    cursor_ = static_cast<size_t>(
        std::lower_bound(ev.begin(), ev.end(), song_pos_,
                         [](const MidiEvent& e, u64 s) { return e.sample < s; }) -
        ev.begin());
    frac_acc_ = 0;
    last_beat_key_ = 0xFFFFFFFFu;
    if (!loop_enabled_ && song_pos_ >= song_.parsed.end_sample) playing_ = false;
}

// ---------------------------------------------------------------------------
// Cue scheduling.

void Sequencer::queue_next(ParsedSong&& parsed, QueueAt at, bool loop_new) {
    pending_ = std::move(parsed);
    pending_at_ = at;
    pending_loop_ = loop_new;
    has_pending_ = true;
}

u64 Sequencer::samples_to_boundary() const noexcept {
    if (!song_.loaded) return 0;
    u64 boundary = song_.parsed.end_sample;
    if (loop_enabled_ && song_.loop_end_sample > song_pos_) boundary = song_.loop_end_sample;
    if (has_pending_ && !pending_at_.at_loop_end && pending_at_.sample > song_pos_ &&
        pending_at_.sample < boundary)
        boundary = pending_at_.sample;
    return boundary > song_pos_ ? boundary - song_pos_ : 0;
}

void Sequencer::adopt_pending_at_boundary() {
    // Notes the outgoing piece left open at the boundary get a normal
    // release (slices already close theirs; original songs may not).
    release_open_notes_at(song_pos_);
    ParsedSong next = std::move(pending_);
    has_pending_ = false;
    pending_ = ParsedSong{};
    loop_override_ = false;
    song_ = Song{};
    song_.parsed = std::move(next);
    song_.finalize();
    song_pos_ = 0;
    cursor_ = 0;
    frac_acc_ = 0;
    last_beat_key_ = 0xFFFFFFFFu;
    loop_enabled_ = pending_loop_;
    playing_ = true;
    if (piece_cb_) piece_cb_(piece_cb_user_);
}

}  // namespace msynth
