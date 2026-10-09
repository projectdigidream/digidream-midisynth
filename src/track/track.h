#pragma once
//
// .mstk -- the baked track format: one self-contained music deliverable, the
// music analogue of the engine's mapc-baked maps. A track bakes together:
//
//   SONG  pre-parsed merged event list with baked sample positions, tempo
//         map (cumulative anchors), loop points -- no SMF parsing at load
//   BANK  tree-shaken patch bank JSON: fallback + ONLY the programs the
//         song actually plays notes with
//   KIT   tree-shaken drum kit JSON: only the notes the song hits
//   FX    resolved song-level reverb/chorus parameters
//   MSTR  mastering chain parameters (optional; absent = bypass)
//   META  name, duration, instrument bitmaps, optional reference-render
//         sha256 (a per-track determinism seal, checkable after transport)
//
// The container carries a sha256 of its payload; a corrupt baked file is
// REJECTED outright (baked deliverables are trusted-or-rejected -- salvage
// is for source SMFs, not for compiled artifacts).
//
// All integers little-endian, fixed widths. Format version bumps on any
// layout change; readers reject newer majors.
//
#include <string>
#include <vector>

#include "../midi/smf_parser.h"
#include "../synth/song_config.h"

namespace msynth {

inline constexpr u32 TRACK_MAGIC = 0x4B54534Du;  // "MSTK" little-endian
inline constexpr u16 TRACK_VERSION = 1;

struct TrackMeta {
    std::string name;
    u32 duration_ms = 0;
    u8 programs_used[16] = {};   // bitmap over 0..127 (melodic)
    u8 drum_notes_used[16] = {}; // bitmap over 0..127 (channel 10)
    bool has_render_hash = false;
    u8 render_hash[32] = {};     // sha256 of a reference render
    u32 render_seconds = 0;      // length of that reference render
    // sha256 over the embedded bank+kit JSON: two tracks with equal content
    // hashes can crossfade sharing one engine's loaded content (slices cut
    // from the same parent always match).
    u8 content_hash[32] = {};
};

struct TrackData {
    ParsedSong song;            // ready for Sequencer::load_parsed
    std::string bank_json;      // tree-shaken; feed to load_patch_bank
    std::string kit_json;       // tree-shaken; feed to load_drum_kit
    SongFxConfig fx;
    TrackMeta meta;
};

struct BakeOptions {
    std::string name;                 // defaults to "track"
    const char* fx_json = nullptr;    // optional song FX config
    size_t fx_json_len = 0;
    const char* master_json = nullptr;  // optional master.json (overrides fx "master")
    size_t master_json_len = 0;
    // Explicit loop override in ticks (plan §9 priority 1); baked in so the
    // deliverable is self-contained. -1 = keep what the SMF declares.
    i64 loop_start_tick = -1;
    i64 loop_end_tick = -1;
    // When > 0, the baker renders this many seconds through the real engine
    // and embeds the PCM sha256 into META (determinism seal).
    u32 verify_seconds = 0;
};

// Bake sources into a track. On success `out` holds the file bytes.
// Warnings surface everything worth knowing: programs resolving to the
// fallback, drum notes with no kit mapping (they will be SILENT), SMF
// salvage notes.
LoadResult bake_track(const u8* smf, size_t smf_len, const char* bank_json,
                      size_t bank_len, const char* kit_json, size_t kit_len,
                      const BakeOptions& opts, std::vector<u8>& out);

// Parse + verify a baked track. Errors reject the whole file (no salvage).
LoadResult load_track(const u8* data, size_t len, TrackData& out);

// True if the buffer starts with the track magic (CLIs use this to accept
// either .mid or .mstk through the same argument).
bool is_track(const u8* data, size_t len);

// ---- Cutting: slice a track into a smaller gameplay-ready track -----------
struct CutOptions {
    std::string name;
    u32 verify_seconds = 0;
};

// Extract [start_tick, end_tick) of a parsed song into a self-contained
// song re-based at tick/sample 0:
//   * controller state (program, CCs, bend, RPN bend range/tuning, time
//     signature, tempo) is CHASED to the cut point and emitted at tick 0,
//     so the slice opens sounding exactly as the parent did there;
//   * note-ons before the cut are not carried in; their note-offs inside
//     the range are dropped (they'd otherwise clip re-struck notes);
//   * notes still open at the cut end get synthetic note-offs there;
//   * the slice loops whole by default (loopable gameplay piece).
LoadResult cut_song(const ParsedSong& src, u32 start_tick, u32 end_tick, ParsedSong& out);

// Cut a baked track: slice the song, re-tree-shake the embedded content for
// the slice, and bake a new .mstk.
LoadResult cut_track(const u8* track, size_t len, u32 start_tick, u32 end_tick,
                     const CutOptions& opts, std::vector<u8>& out);

}  // namespace msynth
