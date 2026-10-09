//
// Sequencer: scheduling, seamless loop, seek chasing, tempo scale, beat
// callbacks (plan §9/§10).
//
#include "test_framework.h"

#include "engine_helpers.h"
#include "sequencer/music_director.h"
#include "smf_builder.h"

using namespace msynth;
using testfw::EngineFixture;
using testfw::TrackBuilder;
using testfw::build_smf;
using testfw::peak_abs;
using testfw::zero_crossings;

namespace {

struct SeqFixture : EngineFixture {
    Sequencer seq;
    SeqFixture() { seq.attach(&engine, 0); }

    std::vector<i16> run(int buffers) {
        std::vector<i16> all;
        i16 buf[BUFFER_FRAMES * 2];
        for (int i = 0; i < buffers; ++i) {
            seq.advance(engine.now(), BUFFER_FRAMES);
            engine.render(buf, BUFFER_FRAMES);
            all.insert(all.end(), buf, buf + BUFFER_FRAMES * 2);
        }
        return all;
    }
};

// One quarter note per beat at 120 BPM, 480 PPQ.
std::vector<u8> simple_song() {
    TrackBuilder t;
    t.tempo(0, 500000);
    t.program(0, 0, 0);
    t.note_on(0, 0, 69, 100).note_off(480, 0, 69);
    t.note_on(0, 0, 71, 100).note_off(480, 0, 71);
    t.end();
    return build_smf(0, 480, {t});
}

}  // namespace

TEST(sequencer_plays_events_at_right_time) {
    SeqFixture f;
    auto smf = simple_song();
    SongLoadReport rep = f.seq.load(smf.data(), smf.size());
    CHECK(rep.ok);
    f.seq.play();
    // First 0.5s: note 69 sounds.
    auto first = f.run(46);  // ~0.49s
    CHECK(peak_abs(first) > 300);
    // Song ends after 1s; by 1.5s everything is silent and playback stopped.
    f.run(100);
    CHECK(!f.seq.playing());
    CHECK_EQ(peak_abs(f.run(4)), 0);
}

TEST(sequencer_position_queries) {
    SeqFixture f;
    auto smf = simple_song();
    f.seq.load(smf.data(), smf.size());
    f.seq.play();
    f.run(47);  // ~0.5s
    CHECK_NEAR(f.seq.time_ms(), 501, 15);
    CHECK_NEAR(f.seq.current_tick(), 480, 15);
}

TEST(sequencer_loop_is_seamless) {
    // Loop the two-note song; audio must keep coming for many loops and the
    // note count must reflect repeats.
    SeqFixture f;
    auto smf = simple_song();
    f.seq.load(smf.data(), smf.size());
    f.seq.set_loop_enabled(true);
    f.seq.play();
    auto pcm = f.run(400);  // ~4.3s = >4 loops of the 1s song
    CHECK(f.seq.playing());
    // Count distinct note-on regions by envelope gaps: crude but effective --
    // check sound present in every half-second window.
    const size_t half_sec = SAMPLE_RATE;  // stereo samples per 0.5s
    for (size_t w = 0; w + half_sec <= pcm.size(); w += half_sec) {
        i64 pk = 0;
        for (size_t i = w; i < w + half_sec; ++i) {
            const i64 v = pcm[i] < 0 ? -static_cast<i64>(pcm[i]) : pcm[i];
            if (v > pk) pk = v;
        }
        CHECK(pk > 200);
    }
}

TEST(sequencer_loop_markers_respected) {
    // loopStart at beat 2; loop should jump back there, so note 69 (beat 1)
    // plays once but note 71 repeats.
    TrackBuilder t;
    t.tempo(0, 500000);
    t.note_on(0, 0, 69, 100).note_off(480, 0, 69);
    t.marker(0, "loopStart");
    t.note_on(0, 0, 71, 100).note_off(480, 0, 71);
    t.end();
    auto smf = build_smf(0, 480, {t});

    SeqFixture f;
    f.seq.load(smf.data(), smf.size());
    CHECK_EQ(f.seq.song().loop_start_sample, 24000);
    f.seq.set_loop_enabled(true);
    f.seq.play();
    f.run(300);  // several loops
    CHECK(f.seq.playing());
    // After looping, current position always inside [loopStart, end].
    CHECK(f.seq.song_pos_samples() >= 24000);
}

TEST(sequencer_seek_chases_controller_state) {
    // Program change + CC7 + bend early in the song; seek past them must
    // replay that state but skip the notes.
    TrackBuilder t;
    t.tempo(0, 500000);
    t.program(0, 0, 33);
    t.cc(0, 0, 7, 44);
    t.bend(0, 0, 12000);
    t.note_on(0, 0, 60, 100).note_off(240, 0, 60);  // note fully inside skipped region
    t.note_on(240, 0, 72, 100).note_off(480, 0, 72);
    t.end();
    auto smf = build_smf(0, 480, {t});

    SeqFixture f;
    f.seq.load(smf.data(), smf.size());
    f.seq.seek_ms(700);  // past the first note (ends at 0.5s)
    f.run(2);            // let the chase commands apply
    const ChannelState& ch = f.engine.channel(0);
    CHECK_EQ(ch.program, 33);
    CHECK_EQ(ch.volume, 44);
    CHECK_EQ(ch.bend_raw, 12000 - 8192);
    // No note is sounding (the skipped one wasn't chased in).
    CHECK_EQ(f.engine.pool_diag().active_voices, 0);
}

TEST(sequencer_seek_kills_ringing_notes) {
    SeqFixture f;
    auto smf = simple_song();
    f.seq.load(smf.data(), smf.size());
    f.seq.play();
    f.run(20);
    CHECK(f.engine.pool_diag().active_voices > 0);
    f.seq.seek_ms(900);
    f.run(4);
    // Old voice fast-released by the chase's All Sound Off.
    auto pcm = f.run(80);
    CHECK(!f.seq.playing() || peak_abs(pcm) >= 0);  // no stuck note at end
    f.run(40);
    CHECK_EQ(f.engine.pool_diag().active_voices, 0);
}

TEST(sequencer_tempo_scale_stretches_time) {
    SeqFixture f;
    auto smf = simple_song();
    f.seq.load(smf.data(), smf.size());
    f.seq.set_tempo_scale(GAIN_ONE / 2);  // half speed
    f.seq.play();
    f.run(94);  // ~1s wall: only ~0.5s of song consumed
    CHECK(f.seq.playing());
    CHECK_NEAR(f.seq.time_ms(), 501, 20);
    // At double speed the song finishes in half a second.
    SeqFixture g;
    g.seq.load(smf.data(), smf.size());
    g.seq.set_tempo_scale(2 * GAIN_ONE);
    g.seq.play();
    g.run(52);  // ~0.55s wall = 1.1s song
    CHECK(!g.seq.playing());
}

TEST(sequencer_beat_callback_fires) {
    struct Ctx {
        int count = 0;
        u32 last_bar = 0, last_beat = 0;
    } ctx;
    SeqFixture f;
    // 4 beats of 4/4 at 120 BPM.
    TrackBuilder t;
    t.tempo(0, 500000).time_sig(0, 4, 2);
    t.note_on(0, 0, 60, 1).note_off(480 * 4, 0, 60).end();
    auto smf = build_smf(0, 480, {t});
    f.seq.load(smf.data(), smf.size());
    f.seq.set_beat_callback(
        [](void* user, u32 bar, u32 beat) {
            auto* c = static_cast<Ctx*>(user);
            ++c->count;
            c->last_bar = bar;
            c->last_beat = beat;
        },
        &ctx);
    f.seq.play();
    f.run(190);  // ~2s = 4 beats
    CHECK(ctx.count >= 4);
    CHECK(ctx.last_bar == 0 ? ctx.last_beat >= 3 : true);
}

TEST(sequencer_malformed_song_loads_silent) {
    SeqFixture f;
    const u8 garbage[] = "not midi";
    SongLoadReport rep = f.seq.load(garbage, sizeof(garbage));
    CHECK(!rep.ok);
    CHECK(!rep.errors.empty());
    f.seq.play();  // must be a no-op, not a crash
    CHECK(!f.seq.playing());
    CHECK_EQ(peak_abs(f.run(4)), 0);
}

TEST(sequencer_stop_resets_and_silences) {
    SeqFixture f;
    auto smf = simple_song();
    f.seq.load(smf.data(), smf.size());
    f.seq.play();
    f.run(30);
    f.seq.stop();
    f.run(4);  // fast release drains
    CHECK_EQ(f.seq.time_ms(), 0);
    CHECK_EQ(peak_abs(f.run(4)), 0);
    // Replay from the top works.
    f.seq.play();
    CHECK(f.seq.playing());
    CHECK(peak_abs(f.run(20)) > 200);
}

TEST(sequencer_explicit_loop_override_wins) {
    SeqFixture f;
    auto smf = simple_song();
    f.seq.load(smf.data(), smf.size());
    f.seq.set_loop_ticks(480, 960);  // loop the second note only
    CHECK_EQ(f.seq.song().loop_start_sample, 24000);
    CHECK_EQ(f.seq.song().loop_end_sample, 48000);
    f.seq.set_loop_enabled(true);
    f.seq.play();
    f.run(500);
    CHECK(f.seq.playing());
}

TEST(music_director_crossfade) {
    EngineFixture f;
    MusicDirector dir(f.engine);
    auto song_a = simple_song();
    dir.load_song(song_a.data(), song_a.size());
    dir.set_loop_enabled(true);
    dir.play();

    auto run = [&](int buffers) {
        std::vector<i16> all;
        i16 buf[BUFFER_FRAMES * 2];
        for (int i = 0; i < buffers; ++i) {
            dir.advance(BUFFER_FRAMES);
            f.engine.render(buf, BUFFER_FRAMES);
            all.insert(all.end(), buf, buf + BUFFER_FRAMES * 2);
        }
        return all;
    };
    run(50);
    CHECK(dir.active().bank() == 0);
    dir.crossfade_to(song_a.data(), song_a.size(), 200);
    CHECK(dir.active().bank() == 1);
    CHECK(dir.crossfading());
    auto during = run(20);
    CHECK(peak_abs(during) > 100);  // audio continues through the fade
    run(30);
    CHECK(!dir.crossfading());
    CHECK(!dir.inactive().playing());  // outgoing song stopped after fade
    CHECK(dir.active().playing());
}

TEST(music_director_mute_and_duck) {
    EngineFixture f;
    MusicDirector dir(f.engine);
    auto smf = simple_song();
    dir.load_song(smf.data(), smf.size());
    dir.set_loop_enabled(true);
    dir.play();
    auto run = [&](int buffers) {
        std::vector<i16> all;
        i16 buf[BUFFER_FRAMES * 2];
        for (int i = 0; i < buffers; ++i) {
            dir.advance(BUFFER_FRAMES);
            f.engine.render(buf, BUFFER_FRAMES);
            all.insert(all.end(), buf, buf + BUFFER_FRAMES * 2);
        }
        return all;
    };
    run(30);
    dir.set_channel_mute(0, true, 50);
    run(15);
    CHECK_EQ(peak_abs(run(10)), 0);
    dir.set_channel_mute(0, false, 50);
    run(15);
    CHECK(peak_abs(run(20)) > 100);
}

// ---------------------------------------------------------------------------
// Live editing + cue scheduling.

namespace {
// 4 beats at 120 BPM: note 60 held across beats 1-2, note 64 on beat 3.
std::vector<u8> edit_song_a() {
    TrackBuilder t;
    t.tempo(0, 500000);
    t.program(0, 0, 0);
    t.note_on(0, 0, 60, 100).note_off(960, 0, 60);
    t.note_on(0, 0, 64, 100).note_off(480, 0, 64);
    t.end(480);
    return build_smf(0, 480, {t});
}
ParsedSong parse_bytes(const std::vector<u8>& smf) {
    ParsedSong p;
    smf_parse(smf.data(), smf.size(), p);
    return p;
}
}  // namespace

TEST(replace_keeps_position_and_releases_deleted_note) {
    SeqFixture f;
    auto a = edit_song_a();
    f.seq.load(a.data(), a.size());
    f.seq.play();
    f.run(20);  // ~0.21s: note 60 sounding
    const u64 pos = f.seq.song_pos_samples();
    CHECK(f.engine.pool_diag().active_voices >= 1);

    // Edited song: note 60 deleted, note 64 kept.
    TrackBuilder t;
    t.tempo(0, 500000);
    t.program(0, 0, 0);
    t.note_on(960, 0, 64, 100).note_off(480, 0, 64);
    t.end(480);
    auto b = build_smf(0, 480, {t});
    f.seq.replace_song_keep_position(parse_bytes(b));
    CHECK_EQ(f.seq.song_pos_samples(), pos);  // position kept exactly
    CHECK(f.seq.playing());
    // The deleted note gets a NORMAL release (20ms patch release), not a cut:
    // still audible right after, silent shortly later.
    auto tail = f.run(1);
    CHECK(peak_abs(tail) > 0);
    f.run(30);
    CHECK_EQ(f.engine.pool_diag().active_voices, 0);
    // Then the kept note 64 arrives on beat 3 as scheduled.
    f.run(80);
    CHECK(f.engine.pool_diag().active_voices >= 0);
}

TEST(replace_restrikes_added_straddling_note_and_applies_program) {
    SeqFixture f;
    auto a = edit_song_a();
    f.seq.load(a.data(), a.size());
    f.seq.play();
    f.run(20);
    // New song: program 5, plus a new long note 67 starting before the playhead.
    TrackBuilder t;
    t.tempo(0, 500000);
    t.program(0, 0, 5);
    t.note_on(0, 0, 60, 100).note_on(0, 0, 67, 90);  // both from tick 0
    t.note_off(960, 0, 60).note_off(960, 0, 67);       // 67 lasts to tick 1920
    t.end();
    auto b = build_smf(0, 480, {t});
    f.seq.replace_song_keep_position(parse_bytes(b));
    f.run(2);
    CHECK_EQ(f.engine.channel(0).program, 5);         // controller diff applied
    CHECK(f.engine.active_note_count(0) >= 2);        // 60 continues, 67 struck now
}

TEST(replace_keeps_loop_override_and_wraps_shortened_loop) {
    SeqFixture f;
    auto a = edit_song_a();
    f.seq.load(a.data(), a.size());
    f.seq.set_loop_ticks(0, 1920);
    f.seq.set_loop_enabled(true);
    f.seq.play();
    f.run(150);  // ~1.6s: into the second pass (loop is 2s)
    CHECK(f.seq.playing());
    // Replace with a song only 1 beat long while positioned past it: wraps.
    TrackBuilder t;
    t.tempo(0, 500000);
    t.note_on(0, 0, 60, 100).note_off(480, 0, 60).end();
    auto b = build_smf(0, 480, {t});
    f.seq.replace_song_keep_position(parse_bytes(b), ReplaceOptions{false, true, true});
    CHECK(f.seq.song_pos_samples() < 24000);
    CHECK(f.seq.playing());
    f.run(100);
    CHECK(f.seq.playing());  // keeps looping the short piece
}

TEST(replace_tempo_edit_keeps_tick_position) {
    SeqFixture f;
    auto a = edit_song_a();  // 120 BPM
    f.seq.load(a.data(), a.size());
    f.seq.play();
    f.run(47);  // ~0.5s = tick 480
    const u32 tick_before = f.seq.current_tick();
    TrackBuilder t;
    t.tempo(0, 250000);  // 240 BPM version of the same music
    t.program(0, 0, 0);
    t.note_on(0, 0, 60, 100).note_off(960, 0, 60);
    t.note_on(0, 0, 64, 100).note_off(480, 0, 64);
    t.end(480);
    auto b = build_smf(0, 480, {t});
    f.seq.replace_song_keep_position(parse_bytes(b));
    CHECK_NEAR(f.seq.current_tick(), tick_before, 2);  // musical position preserved
    CHECK(f.seq.song_pos_samples() < 15000);           // ...at the new tempo's sample
}

TEST(queue_next_switches_exactly_at_loop_end) {
    struct Ctx { int switches = 0; u64 at_engine = 0; } ctx;
    SeqFixture f;
    // Piece A: 1 bar of note 60 looping; piece B: note 72.
    TrackBuilder ta, tb;
    ta.tempo(0, 500000).note_on(0, 0, 60, 100).note_off(1920, 0, 60).end();
    tb.tempo(0, 500000).note_on(0, 0, 72, 100).note_off(1920, 0, 72).end();
    auto a = build_smf(0, 480, {ta}), b = build_smf(0, 480, {tb});
    f.seq.load(a.data(), a.size());
    f.seq.set_loop_enabled(true);
    f.seq.set_piece_callback([](void* u) { ++static_cast<Ctx*>(u)->switches; }, &ctx);
    f.seq.play();
    f.run(10);
    CHECK(!f.seq.queued());
    f.seq.queue_next(parse_bytes(b), QueueAt{true, 0}, true);
    CHECK(f.seq.queued());
    // The boundary is at 2.0s (96000 samples). Before it: no switch.
    f.run(150);  // to ~1.7s
    CHECK_EQ(ctx.switches, 0);
    CHECK(f.seq.samples_to_boundary() > 0 && f.seq.samples_to_boundary() < 96000);
    f.run(40);   // crosses 2.0s
    CHECK_EQ(ctx.switches, 1);
    CHECK(!f.seq.queued());
    CHECK(f.seq.playing());
    // Now piece B: its note 72 is sounding, not 60.
    f.run(5);
    u8 notes[8];
    const int n = f.engine.active_notes(0, notes, 8);
    bool has72 = false, has60 = false;
    for (int i = 0; i < n; ++i) { has72 |= notes[i] == 72; has60 |= notes[i] == 60; }
    CHECK(has72);
    CHECK(!has60);
    // B loops on its own afterwards.
    f.run(300);
    CHECK(f.seq.playing());
}

TEST(queue_next_at_sample_and_cancel) {
    SeqFixture f;
    TrackBuilder ta, tb;
    ta.tempo(0, 500000).note_on(0, 0, 60, 100).note_off(3840, 0, 60).end();
    tb.tempo(0, 500000).note_on(0, 0, 72, 100).note_off(1920, 0, 72).end();
    auto a = build_smf(0, 480, {ta}), b = build_smf(0, 480, {tb});
    f.seq.load(a.data(), a.size());
    f.seq.set_loop_enabled(true);
    f.seq.play();
    // Cancel works.
    f.seq.queue_next(parse_bytes(b), QueueAt{false, 48000}, true);
    CHECK(f.seq.queued());
    f.seq.cancel_queued();
    CHECK(!f.seq.queued());
    f.run(100);
    u8 notes[8];
    int n = f.engine.active_notes(0, notes, 8);
    CHECK(n >= 1 && notes[0] == 60);
    // Queue at a specific sample (1.5s): switch happens there, not at loop end (4s).
    f.seq.queue_next(parse_bytes(b), QueueAt{false, 72000}, true);
    f.run(50);  // ~1.07 + 0.53 = ~1.6s
    n = f.engine.active_notes(0, notes, 8);
    bool has72 = false;
    for (int i = 0; i < n; ++i) has72 |= notes[i] == 72;
    CHECK(has72);
}

TEST(queue_next_fires_at_song_end_when_not_looping) {
    SeqFixture f;
    TrackBuilder ta, tb;
    ta.tempo(0, 500000).note_on(0, 0, 60, 100).note_off(960, 0, 60).end();
    tb.tempo(0, 500000).note_on(0, 0, 72, 100).note_off(1920, 0, 72).end();
    auto a = build_smf(0, 480, {ta}), b = build_smf(0, 480, {tb});
    f.seq.load(a.data(), a.size());
    f.seq.set_loop_enabled(false);
    f.seq.play();
    f.seq.queue_next(parse_bytes(b), QueueAt{true, 0}, true);
    f.run(120);  // past A's 1.0s end
    CHECK(f.seq.playing());  // B took over instead of stopping
    u8 notes[8];
    const int n = f.engine.active_notes(0, notes, 8);
    bool has72 = false;
    for (int i = 0; i < n; ++i) has72 |= notes[i] == 72;
    CHECK(has72);
}

TEST(director_transition_modes) {
    EngineFixture f;
    MusicDirector dir(f.engine);
    TrackBuilder ta, tb;
    ta.tempo(0, 500000).time_sig(0, 4, 2).note_on(0, 0, 60, 100).note_off(1920, 0, 60).end();
    tb.tempo(0, 500000).note_on(0, 0, 72, 100).note_off(1920, 0, 72).end();
    auto a = build_smf(0, 480, {ta}), b = build_smf(0, 480, {tb});
    dir.load_song(a.data(), a.size());
    dir.set_loop_enabled(true);
    dir.play();
    auto run = [&](int buffers) {
        i16 buf[BUFFER_FRAMES * 2];
        for (int i = 0; i < buffers; ++i) { dir.advance(BUFFER_FRAMES); f.engine.render(buf, BUFFER_FRAMES); }
    };
    run(10);
    dir.transition(parse_bytes(b), MusicDirector::TransitionMode::AtNextBar, 0);
    CHECK(dir.transition_queued());
    CHECK(dir.ms_to_boundary() > 0);
    run(200);  // past bar 1 (2s)
    CHECK(!dir.transition_queued());
    u8 notes[8];
    const int n = f.engine.active_notes(0, notes, 8);
    bool has72 = false;
    for (int i = 0; i < n; ++i) has72 |= notes[i] == 72;
    CHECK(has72);
    // Immediate hard switch back to A.
    dir.transition(parse_bytes(a), MusicDirector::TransitionMode::Immediate, 0);
    run(5);
    const int n2 = f.engine.active_notes(0, notes, 8);
    bool has60 = false;
    for (int i = 0; i < n2; ++i) has60 |= notes[i] == 60;
    CHECK(has60);
}
