//
// midisynth_bake: compile MIDI + patch JSON + kit JSON (+ FX config) into a
// self-contained .mstk track -- the music analogue of mapc.
//
//   midisynth_bake song.mid -o track.mstk --bank b.json --drums k.json
//                  [--fx cfg.json] [--master m.json] [--name X]
//                  [--loop-start-tick T --loop-end-tick T]
//                  [--verify-seconds N]     embed a reference render hash
//
//   midisynth_bake --check track.mstk      re-render and compare the
//                                          embedded hash (determinism seal)
//
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "../output/audio_output_offline.h"
#include "../output/sha256.h"
#include "../synth/patch_loader.h"
#include "../track/track.h"
#include "../midi/smf_writer.h"

using namespace msynth;

static bool read_file(const std::string& path, std::vector<u8>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

static void print_report(const LoadResult& r) {
    for (const auto& w : r.warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());
    for (const auto& e : r.errors) std::fprintf(stderr, "error: %s\n", e.c_str());
}

static int check_track(const std::string& path) {
    std::vector<u8> bytes;
    if (!read_file(path, bytes)) {
        std::fprintf(stderr, "cannot read %s\n", path.c_str());
        return 1;
    }
    TrackData track;
    LoadResult lr = load_track(bytes.data(), bytes.size(), track);
    print_report(lr);
    if (!lr.ok) return 1;
    if (!track.meta.has_render_hash) {
        std::fprintf(stderr, "%s carries no render hash (bake with --verify-seconds)\n",
                     path.c_str());
        return 1;
    }
    WaveTableCache cache;
    PatchBank bank;
    DrumKit kit;
    load_patch_bank(track.bank_json.data(), track.bank_json.size(), cache, bank);
    load_drum_kit(track.kit_json.data(), track.kit_json.size(), cache, kit);
    OfflineRenderer renderer;
    renderer.engine().set_content(&bank, &kit);
    push_song_config(renderer.engine(), track.fx);
    renderer.director().active().load_parsed(std::move(track.song));
    renderer.director().play();
    std::vector<i16> pcm;
    const u64 frames =
        (static_cast<u64>(track.meta.render_seconds) * SAMPLE_RATE / BUFFER_FRAMES) *
        BUFFER_FRAMES;
    renderer.render(pcm, frames);
    Sha256 h;
    h.update(pcm.data(), pcm.size() * 2);
    const std::string hex = h.hex_digest();
    char stored[65] = {};
    for (int i = 0; i < 32; ++i) std::snprintf(stored + 2 * i, 3, "%02x", track.meta.render_hash[i]);
    if (hex != stored) {
        std::fprintf(stderr, "DIVERGED: baked %s, rendered %s\n", stored, hex.c_str());
        return 1;
    }
    std::printf("ok: '%s' renders bit-identically to its baked seal (%us, %s)\n",
                track.meta.name.c_str(), track.meta.render_seconds, hex.c_str());
    return 0;
}

// --to-mid: write the SONG section of a track back out as a Standard MIDI
// File (format 1). Bank/kit/fx are not representable in SMF and are dropped.
static int track_to_mid(const char* in_path, const char* out_path) {
    std::vector<u8> bytes;
    if (!read_file(in_path, bytes)) {
        std::fprintf(stderr, "cannot read %s\n", in_path);
        return 1;
    }
    TrackData data;
    LoadResult lr = load_track(bytes.data(), bytes.size(), data);
    for (auto& w : lr.warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());
    if (!lr.ok) {
        for (auto& e : lr.errors) std::fprintf(stderr, "error: %s\n", e.c_str());
        return 1;
    }
    std::vector<u8> smf;
    LoadResult wr = smf_write(data.song, smf);
    for (auto& w : wr.warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());
    if (!wr.ok) {
        for (auto& e : wr.errors) std::fprintf(stderr, "error: %s\n", e.c_str());
        return 1;
    }
    std::ofstream f(out_path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(smf.data()), static_cast<std::streamsize>(smf.size()));
    if (!f) {
        std::fprintf(stderr, "cannot write %s\n", out_path);
        return 1;
    }
    std::printf("%s -> %s (%zu bytes, %zu events)\n", in_path, out_path, smf.size(),
                data.song.events.size());
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "--check") == 0) return check_track(argv[2]);
    if (argc >= 4 && std::strcmp(argv[1], "--to-mid") == 0) return track_to_mid(argv[2], argv[3]);
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s song.mid -o track.mstk --bank b.json --drums k.json\n"
                     "       [--fx cfg.json] [--master m.json] [--name X] [--loop-start-tick T --loop-end-tick T]\n"
                     "       [--verify-seconds N]\n"
                     "       %s --check track.mstk\n"
                     "       %s --to-mid track.mstk out.mid\n",
                     argv[0], argv[0], argv[0]);
        return 2;
    }
    std::string midi_path = argv[1], out_path, bank_path, drums_path, fx_path, master_path;
    BakeOptions opts;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-o" && i + 1 < argc) out_path = argv[++i];
        else if (a == "--bank" && i + 1 < argc) bank_path = argv[++i];
        else if (a == "--drums" && i + 1 < argc) drums_path = argv[++i];
        else if (a == "--fx" && i + 1 < argc) fx_path = argv[++i];
        else if (a == "--master" && i + 1 < argc) master_path = argv[++i];
        else if (a == "--name" && i + 1 < argc) opts.name = argv[++i];
        else if (a == "--loop-start-tick" && i + 1 < argc) opts.loop_start_tick = std::atoll(argv[++i]);
        else if (a == "--loop-end-tick" && i + 1 < argc) opts.loop_end_tick = std::atoll(argv[++i]);
        else if (a == "--verify-seconds" && i + 1 < argc)
            opts.verify_seconds = static_cast<u32>(std::atoi(argv[++i]));
        else {
            std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
            return 2;
        }
    }
    if (out_path.empty() || bank_path.empty() || drums_path.empty()) {
        std::fprintf(stderr, "-o, --bank and --drums are required\n");
        return 2;
    }
    if (opts.name.empty()) {
        size_t slash = midi_path.find_last_of('/');
        std::string base = slash == std::string::npos ? midi_path : midi_path.substr(slash + 1);
        size_t dot = base.find_last_of('.');
        opts.name = dot == std::string::npos ? base : base.substr(0, dot);
    }

    tables_init();
    std::vector<u8> smf, bank, drums, fx, master;
    if (!read_file(midi_path, smf)) { std::fprintf(stderr, "cannot read %s\n", midi_path.c_str()); return 1; }
    if (!read_file(bank_path, bank)) { std::fprintf(stderr, "cannot read %s\n", bank_path.c_str()); return 1; }
    if (!read_file(drums_path, drums)) { std::fprintf(stderr, "cannot read %s\n", drums_path.c_str()); return 1; }
    if (!fx_path.empty()) {
        if (!read_file(fx_path, fx)) { std::fprintf(stderr, "cannot read %s\n", fx_path.c_str()); return 1; }
        opts.fx_json = reinterpret_cast<const char*>(fx.data());
        opts.fx_json_len = fx.size();
    }
    if (!master_path.empty()) {
        if (!read_file(master_path, master)) { std::fprintf(stderr, "cannot read %s\n", master_path.c_str()); return 1; }
        opts.master_json = reinterpret_cast<const char*>(master.data());
        opts.master_json_len = master.size();
    }

    std::vector<u8> out;
    LoadResult r = bake_track(smf.data(), smf.size(), reinterpret_cast<const char*>(bank.data()),
                              bank.size(), reinterpret_cast<const char*>(drums.data()),
                              drums.size(), opts, out);
    print_report(r);
    if (!r.ok) return 1;

    std::ofstream f(out_path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (!f) { std::fprintf(stderr, "cannot write %s\n", out_path.c_str()); return 1; }

    // Report what got baked.
    TrackData track;
    load_track(out.data(), out.size(), track);
    int nprog = 0, ndrum = 0;
    for (int i = 0; i < 128; ++i) {
        nprog += (track.meta.programs_used[i >> 3] >> (i & 7)) & 1;
        ndrum += (track.meta.drum_notes_used[i >> 3] >> (i & 7)) & 1;
    }
    std::printf("baked '%s': %zu bytes (%zu events, %d programs, %d drum notes, %u.%us)%s\n",
                track.meta.name.c_str(), out.size(), track.song.events.size(), nprog, ndrum,
                track.meta.duration_ms / 1000, track.meta.duration_ms % 1000 / 100,
                track.meta.has_render_hash ? " +render seal" : "");
    return 0;
}
