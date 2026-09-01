#include "wavetable_cache.h"

namespace msynth {

std::string json_canonical(const JsonValue* v) {
    if (!v) return "~";
    switch (v->type) {
        case JsonValue::Type::Null: return "null";
        case JsonValue::Type::Bool: return v->boolean ? "true" : "false";
        case JsonValue::Type::Number: return "q" + std::to_string(v->num_q16);
        case JsonValue::Type::String: return "s\"" + v->str + "\"";
        case JsonValue::Type::Array: {
            std::string s = "[";
            for (const auto& e : v->arr) s += json_canonical(e.get()) + ",";
            return s + "]";
        }
        case JsonValue::Type::Object: {
            std::string s = "{";
            for (const auto& kv : v->obj)  // std::map: already key-sorted
                s += kv.first + ":" + json_canonical(kv.second.get()) + ",";
            return s + "}";
        }
    }
    return "~";
}

WaveTablePtr WaveTableCache::get_or_create(const std::string& generator, int frameCount,
                                           const JsonValue* params, std::string* err) {
    if (frameCount < 1) frameCount = 1;
    if (frameCount > MAX_FRAMES) frameCount = MAX_FRAMES;

    const std::string key =
        generator + "#" + std::to_string(frameCount) + "#" + json_canonical(params);
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;

    GeneratorFn fn = wavetable_generator(generator);
    if (!fn) {
        if (err) *err = "unknown wavetable generator '" + generator + "'";
        return nullptr;
    }
    auto table = std::make_shared<WaveTable>();
    if (!fn(params, frameCount, *table, err)) return nullptr;
    WaveTablePtr ptr = table;
    cache_[key] = ptr;
    return ptr;
}

}  // namespace msynth
