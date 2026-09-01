#pragma once
//
// Wavetable generator interface (control thread). A generator fills a
// WaveTable's every mip level from a JSON param object; per-mip harmonic caps
// (HARM_LIMIT) are the generator's responsibility so each mip stays
// band-limited for the pitch range it covers.
//
#include <string>

#include "json_min.h"
#include "wavetables.h"

namespace msynth {

using GeneratorFn = bool (*)(const JsonValue* params, int frameCount, WaveTable& out,
                             std::string* err);

// name -> generator; nullptr if unknown.
GeneratorFn wavetable_generator(const std::string& name);

}  // namespace msynth
