//
// midisynth_play: native realtime playback through miniaudio, driven entirely
// through the C API -- which makes this both the reference embedding example
// and the exercise of plan §11's runtime diagnostics (voices, steals, drops,
// and CPU time per render callback, min/avg/max over a 1s window).
//
//   midisynth_play <song.mid> [--bank b.json] [--drums k.json] [--fx cfg.json] [--master m.json]
//                  [--loop] [--tempo milli] [--volume milli] [--seek ms]
//
// The audio callback calls msynth_render (which advances the sequencer too,
// exactly like the AudioWorklet build); everything else is the main thread
// talking through the C API's queued commands. Ctrl+C stops.
//
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "../bindings/c_api.h"
#include "../track/track.h"
#include "miniaudio/miniaudio.h"

namespace {

std::atomic<bool> g_running{true};
void on_sigint(int) { g_running = false; }

// Render-callback timing (plan §11): lock-free snapshot for the main thread.
struct RenderStats {
    std::atomic<long long> min_ns{1LL << 60}, max_ns{0}, sum_ns{0}, calls{0};
    void record(long long ns) {
        long long m = min_ns.load(std::memory_order_relaxed);
        while (ns < m && !min_ns.compare_exchange_weak(m, ns)) {}
        m = max_ns.load(std::memory_order_relaxed);
        while (ns > m && !max_ns.compare_exchange_weak(m, ns)) {}
        sum_ns.fetch_add(ns, std::memory_order_relaxed);
        calls.fetch_add(1, std::memory_order_relaxed);
    }
    void reset() {
        min_ns = 1LL << 60;
        max_ns = 0;
        sum_ns = 0;
        calls = 0;
    }
};

struct CallbackState {
    msynth_ctx* synth;
    RenderStats stats;
};

void audio_callback(ma_device* device, void* output, const void*, ma_uint32 frames) {
    auto* st = static_cast<CallbackState*>(device->pUserData);
    const auto t0 = std::chrono::steady_clock::now();
    // miniaudio may ask for a count that is not a multiple of the engine's
    // 64-frame block; render whole blocks and zero the (rare) remainder.
    const ma_uint32 whole = frames - (frames % 64);
    msynth_render(st->synth, static_cast<int16_t*>(output), static_cast<int>(whole));
    if (whole < frames)
        std::memset(static_cast<int16_t*>(output) + whole * 2, 0, (frames - whole) * 4);
    st->stats.record(std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now() - t0)
                         .count());
}

bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

void print_report(const char* what, const char* report) {
    if (report[0]) std::fprintf(stderr, "[%s]\n%s", what, report);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <song.mid> [--bank b.json] [--drums k.json] [--fx cfg.json] [--master m.json]\n"
                     "       [--loop] [--tempo milli] [--volume milli] [--seek ms]\n",
                     argv[0]);
        return 2;
    }
    std::string bank_path, drums_path, fx_path, master_path;
    bool loop = false;
    int tempo_milli = 1000, volume_milli = 0, seek_ms = 0;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--bank" && i + 1 < argc) bank_path = argv[++i];
        else if (a == "--drums" && i + 1 < argc) drums_path = argv[++i];
        else if (a == "--fx" && i + 1 < argc) fx_path = argv[++i];
        else if (a == "--master" && i + 1 < argc) master_path = argv[++i];
        else if (a == "--loop") loop = true;
        else if (a == "--tempo" && i + 1 < argc) tempo_milli = std::atoi(argv[++i]);
        else if (a == "--volume" && i + 1 < argc) volume_milli = std::atoi(argv[++i]);
        else if (a == "--seek" && i + 1 < argc) seek_ms = std::atoi(argv[++i]);
        else {
            std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
            return 2;
        }
    }

    msynth_ctx* synth = msynth_create();
    char report[8192];

    auto load_json = [&](const std::string& path, const char* what,
                         int (*fn)(msynth_ctx*, const char*, size_t, char*, size_t)) {
        if (path.empty()) return true;
        std::vector<uint8_t> data;
        if (!read_file(path, data)) {
            std::fprintf(stderr, "cannot read %s\n", path.c_str());
            return false;
        }
        const int rc = fn(synth, reinterpret_cast<const char*>(data.data()), data.size(),
                          report, sizeof(report));
        print_report(what, report);
        return rc == 0;
    };
    if (!load_json(bank_path, "bank", msynth_load_bank_json)) return 1;
    if (!load_json(drums_path, "drums", msynth_load_drums_json)) return 1;
    if (!load_json(fx_path, "fx", msynth_load_song_config_json)) return 1;

    std::vector<uint8_t> smf;
    if (!read_file(argv[1], smf)) {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    const bool baked = msynth::is_track(smf.data(), smf.size());
    const int load_rc = baked
        ? msynth_load_track(synth, smf.data(), smf.size(), report, sizeof(report))
        : msynth_load_song(synth, smf.data(), smf.size(), report, sizeof(report));
    if (load_rc != 0) {
        print_report(baked ? "track" : "song", report);
        return 1;
    }
    print_report(baked ? "track" : "song", report);
    if (baked && (!bank_path.empty() || !drums_path.empty() || !fx_path.empty()))
        std::fprintf(stderr, "note: .mstk embeds its own content; --bank/--drums/--fx ignored\n");
    // --master applies on top of whatever the song/track brought (auditioning
    // a mastering preset against a baked track).
    if (!load_json(master_path, "master", msynth_load_master_json)) return 1;

    msynth_set_loop(synth, loop);
    msynth_set_tempo_scale_milli(synth, tempo_milli);
    if (volume_milli > 0) msynth_set_master_volume_milli(synth, volume_milli);
    if (seek_ms > 0) msynth_seek_ms(synth, static_cast<uint32_t>(seek_ms));
    msynth_play(synth);

    CallbackState state{synth, {}};
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_s16;
    cfg.playback.channels = 2;
    cfg.sampleRate = 48000;
    cfg.periodSizeInFrames = 512;
    cfg.dataCallback = audio_callback;
    cfg.pUserData = &state;

    ma_device device;
    if (ma_device_init(nullptr, &cfg, &device) != MA_SUCCESS) {
        std::fprintf(stderr, "audio device init failed\n");
        return 1;
    }
    if (ma_device_start(&device) != MA_SUCCESS) {
        std::fprintf(stderr, "audio device start failed\n");
        ma_device_uninit(&device);
        return 1;
    }
    std::signal(SIGINT, on_sigint);
    std::printf("playing %s at %u Hz (Ctrl+C to stop)\n", argv[1], device.sampleRate);

    while (g_running && (msynth_playing(synth) || loop)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        uint32_t bar, beat;
        msynth_bar_beat(synth, &bar, &beat);
        const long long calls = state.stats.calls.load();
        const long long avg = calls ? state.stats.sum_ns.load() / calls : 0;
        std::printf(
            "\r%6u.%01us/%u.%01us  bar %u·%u  voices %d (peak %d)  steals %lld drops %lld"
            "  render %lld/%lld/%lld us   ",
            msynth_time_ms(synth) / 1000, msynth_time_ms(synth) % 1000 / 100,
            msynth_duration_ms(synth) / 1000, msynth_duration_ms(synth) % 1000 / 100, bar + 1,
            beat + 1, msynth_active_voices(synth), msynth_peak_voices(synth),
            static_cast<long long>(msynth_steal_count(synth)),
            static_cast<long long>(msynth_dropped_notes(synth)),
            calls ? state.stats.min_ns.load() / 1000 : 0, avg / 1000,
            state.stats.max_ns.load() / 1000);
        std::fflush(stdout);
        state.stats.reset();
    }
    std::printf("\n");

    msynth_stop(synth);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // drain fast releases
    ma_device_uninit(&device);
    msynth_destroy(synth);
    return 0;
}
