#pragma once
//
// Dedupe generated wavetables by generator name + frame count + canonical
// param serialization (plan §4). Patches sharing a recipe share one table.
// Control-thread only.
//
#include <map>
#include <string>
#include <vector>

#include "wavetable_gen.h"

namespace msynth {

class WaveTableCache {
  public:
    // Returns nullptr on failure (unknown generator / generator error).
    WaveTablePtr get_or_create(const std::string& generator, int frameCount,
                               const JsonValue* params, std::string* err);

    size_t size() const { return cache_.size(); }
    void clear() { cache_.clear(); }

  private:
    std::map<std::string, WaveTablePtr> cache_;  // keyed by canonical recipe
};

// Canonical, order-stable serialization of a JSON value (objects by sorted
// key; numbers as raw Q16.16) -- the cache key and nothing else.
std::string json_canonical(const JsonValue* v);

}  // namespace msynth
