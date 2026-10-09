//
// midisynth_render: offline SMF -> WAV renderer.
//
//   midisynth_render <song.mid> <out.wav> [--seconds N] [--bank bank.json]
//                    [--drums kit.json] [--dither] [--hash] [--loop]
//
// With --hash, prints "sha256 <hex>" of the raw PCM to stdout (the golden
// harness consumes this).
//
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "../synth/patch_loader.h"
#include "../synth/song_config.h"
#include "../track/track.h"
#include "audio_output_offline.h"
#include "sha256.h"
#include "wav_writer.h"

using namespace msynth;

static bool read_file(const std::string& path, std::vector<u8>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

static void print_report(const char* what, const std::vector<std::string>& warnings,
                         const std::vector<std::string>& errors) {
    for (const auto& w : warnings) std::fprintf(stderr, "[%s] warning: %s\n", what, w.c_str());
    for (const auto& e : errors) std::fprintf(stderr, "[%s] error: %s\n", what, e.c_str());
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: %s <song.mid> <out.wav> [--seconds N] [--bank b.json] "
                     "[--drums k.json] [--fx cfg.json] [--master m.json] [--dither] [--hash] [--loop]\n",
                     argv[0]);
        return 2;
    }
    const std::string midi_path = argv[1];
    const std::string wav_path = argv[2];
    u64 seconds = 0;
    std::string bank_path, drums_path, fx_path, master_path;
    bool dither = false, want_hash = false, loop = false;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seconds" && i + 1 < argc) seconds = static_cast<u64>(std::atoll(argv[++i]));
        else if (a == "--bank" && i + 1 < argc) bank_path = argv[++i];
        else if (a == "--drums" && i + 1 < argc) drums_path = argv[++i];
        else if (a == "--fx" && i + 1 < argc) fx_path = argv[++i];
        else if (a == "--master" && i + 1 < argc) master_path = argv[++i];
        else if (a == "--dither") dither = true;
        else if (a == "--hash") want_hash = true;
        else if (a == "--loop") loop = true;
        else {
            std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
            return 2;
        }
    }

    tables_init();
    WaveTableCache cache;
    PatchBank bank;
    DrumKit kit;

    if (!bank_path.empty()) {
        std::vector<u8> data;
        if (!read_file(bank_path, data)) {
            std::fprintf(stderr, "cannot read %s\n", bank_path.c_str());
            return 1;
        }
        LoadResult r = load_patch_bank(reinterpret_cast<const char*>(data.data()), data.size(),
                                       cache, bank);
        print_report("bank", r.warnings, r.errors);
    } else {
        make_builtin_fallback(cache, bank.fallback());
    }
    if (!drums_path.empty()) {
        std::vector<u8> data;
        if (!read_file(drums_path, data)) {
            std::fprintf(stderr, "cannot read %s\n", drums_path.c_str());
            return 1;
        }
        LoadResult r = load_drum_kit(reinterpret_cast<const char*>(data.data()), data.size(),
                                     cache, kit);
        print_report("drums", r.warnings, r.errors);
    }

    std::vector<u8> smf;
    if (!read_file(midi_path, smf)) {
        std::fprintf(stderr, "cannot read %s\n", midi_path.c_str());
        return 1;
    }

    // Baked track? Its embedded content replaces --bank/--drums/--fx.
    TrackData track;
    const bool baked = is_track(smf.data(), smf.size());
    if (baked) {
        LoadResult tr = load_track(smf.data(), smf.size(), track);
        print_report("track", tr.warnings, tr.errors);
        if (!tr.ok) return 1;
        bank = PatchBank{};
        kit = DrumKit{};
        LoadResult br = load_patch_bank(track.bank_json.data(), track.bank_json.size(), cache, bank);
        print_report("track bank", br.warnings, br.errors);
        LoadResult kr = load_drum_kit(track.kit_json.data(), track.kit_json.size(), cache, kit);
        print_report("track kit", kr.warnings, kr.errors);
    }

    OfflineRenderer renderer;
    renderer.engine().set_content(&bank, &kit);
    renderer.engine().output().dither = dither;
    if (baked) push_song_config(renderer.engine(), track.fx);

    if (!fx_path.empty()) {
        std::vector<u8> data;
        if (!read_file(fx_path, data)) {
            std::fprintf(stderr, "cannot read %s\n", fx_path.c_str());
            return 1;
        }
        SongFxConfig cfg;
        LoadResult r = load_song_config(reinterpret_cast<const char*>(data.data()),
                                        data.size(), cfg);
        print_report("fx", r.warnings, r.errors);
        if (r.ok) push_song_config(renderer.engine(), cfg);
    }
    if (!master_path.empty()) {  // overrides the track's / fx.json's master
        std::vector<u8> data;
        if (!read_file(master_path, data)) {
            std::fprintf(stderr, "cannot read %s\n", master_path.c_str());
            return 1;
        }
        MasterParams mp;
        LoadResult r = load_master_config(reinterpret_cast<const char*>(data.data()), data.size(), mp);
        print_report("master", r.warnings, r.errors);
        if (r.ok) push_master_config(renderer.engine(), mp);
    }

    SongLoadReport rep = baked
        ? renderer.director().active().load_parsed(std::move(track.song))
        : renderer.director().load_song(smf.data(), smf.size());
    print_report("song", rep.warnings, rep.errors);
    renderer.director().set_loop_enabled(loop);
    renderer.director().play();

    const Song& song = renderer.director().active().song();
    u64 frames = seconds > 0 ? seconds * SAMPLE_RATE
                             : song.parsed.end_sample + 2 * SAMPLE_RATE;  // + release/reverb tail
    frames = (frames / BUFFER_FRAMES) * BUFFER_FRAMES;

    std::vector<i16> pcm;
    pcm.reserve(static_cast<size_t>(frames) * 2);
    renderer.render(pcm, frames);

    if (!write_wav16(wav_path, pcm, SAMPLE_RATE, 2)) {
        std::fprintf(stderr, "cannot write %s\n", wav_path.c_str());
        return 1;
    }
    if (want_hash)
        std::printf("sha256 %s\n", sha256_hex(pcm.data(), pcm.size() * 2).c_str());

    const PoolDiagnostics& d = renderer.engine().pool_diag();
    std::fprintf(stderr, "rendered %llu frames; peak voices %d, steals %lld, drops %lld\n",
                 static_cast<unsigned long long>(frames), d.peak_voices,
                 static_cast<long long>(d.steal_count), static_cast<long long>(d.dropped_notes));
    return 0;
}
