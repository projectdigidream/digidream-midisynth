#pragma once
//
// JSON -> PatchBank / DrumKit (plan §13 schema, §11 error policy). Loaders
// never crash and always produce something: bad values are clamped with a
// warning, a broken layer falls back to a plain sine, a broken patch to the
// fallback patch. Control thread only.
//
#include <string>
#include <vector>

#include "patch_bank.h"
#include "wavetable_cache.h"

namespace msynth {

struct LoadResult {
    bool ok = true;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;

    void warn(const std::string& m) { warnings.push_back(m); }
    void error(const std::string& m) {
        errors.push_back(m);
        ok = false;
    }
};

// Tempo-sync spec -> cycle length in beats (Q16), 0 when absent or bad.
// "1/4", "1/8T" (triplet, x2/3), "1/8." (dotted, x3/2), "2/1". Shared by
// patch LFOs and fx.json's delay time.
i32 parse_sync_beats(const std::string& s, LoadResult& r, const std::string& where);

// Parse one patch object (the §13 schema) into `out`.
LoadResult load_patch(const JsonValue& doc, WaveTableCache& cache, Patch& out);

// Bank file: { "version": 1, "programs": { "0": {patch}, ... },
//              "fallback": {patch} }. Missing fallback -> built-in default.
LoadResult load_patch_bank(const char* json, size_t len, WaveTableCache& cache, PatchBank& out);

// Drum kit file: { "version": 1, "notes": { "36": {patch + oneShot/
// exclusiveGroup/pitchNote}, ... } }.
LoadResult load_drum_kit(const char* json, size_t len, WaveTableCache& cache, DrumKit& out);

// The defined, decent-sounding default every unknown program maps to (§17).
void make_builtin_fallback(WaveTableCache& cache, Patch& out);

}  // namespace msynth
