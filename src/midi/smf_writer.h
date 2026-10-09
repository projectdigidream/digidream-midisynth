#pragma once
//
// Standard MIDI File writer: the inverse of smf_parser. Round-trip contract:
// smf_parse(smf_write(song)) reproduces song.events (status/d1/d2/tick/
// track/seq/sample), tempo segments, loop points and end_tick/end_sample --
// after normalize_seq(), which renumbers seq densely per track the way the
// parser does.
//
// Writes format 1 (format 0 when there is a single track), full status bytes
// (no running status), every track index 0..max(track) (empty ones carry only
// an end-of-track), tempo and loop markers on track 0, time signatures on the
// track/seq position they came from.
//
#include <vector>

#include "../synth/patch_loader.h"  // LoadResult
#include "smf_parser.h"

namespace msynth {

// Sort by (tick, track, seq) and renumber seq densely per track -- the shape
// the parser produces. Call before smf_write on hand-built songs.
void normalize_seq(ParsedSong& song);

LoadResult smf_write(const ParsedSong& song, std::vector<u8>& out);

}  // namespace msynth
