#pragma once
//
// Minimal JSON parser scoped to the patch/song-config schema (plan §13).
// Control-thread only. Numbers are parsed with integer math into Q16.16
// (plus the exact integer part) -- strtod never touches patch data, so a
// platform's float parsing can't leak into the deterministic path.
//
// Supported: objects, arrays, strings (with \" \\ \/ \b \f \n \r \t \uXXXX ->
// kept as-is for BMP only), numbers (optional sign, int, optional fraction;
// exponents rejected -- the schema never needs them), true/false/null.
//
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "fixmath.h"

namespace msynth {

struct JsonValue;
using JsonPtr = std::shared_ptr<JsonValue>;

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;

    bool boolean = false;
    i64 num_int = 0;      // truncated integer value
    i32 num_q16 = 0;      // Q16.16 value (saturated)
    bool num_negative = false;
    std::string str;
    std::vector<JsonPtr> arr;
    std::map<std::string, JsonPtr> obj;

    bool is_obj() const { return type == Type::Object; }
    bool is_arr() const { return type == Type::Array; }
    bool is_num() const { return type == Type::Number; }
    bool is_str() const { return type == Type::String; }

    const JsonValue* get(const std::string& key) const {
        if (type != Type::Object) return nullptr;
        auto it = obj.find(key);
        return it == obj.end() ? nullptr : it->second.get();
    }
    i64 get_int(const std::string& key, i64 def) const {
        const JsonValue* v = get(key);
        return v && v->is_num() ? v->num_int : def;
    }
    i32 get_q16(const std::string& key, i32 def) const {
        const JsonValue* v = get(key);
        return v && v->is_num() ? v->num_q16 : def;
    }
    std::string get_str(const std::string& key, const std::string& def) const {
        const JsonValue* v = get(key);
        return v && v->is_str() ? v->str : def;
    }
    bool get_bool(const std::string& key, bool def) const {
        const JsonValue* v = get(key);
        return v && v->type == Type::Bool ? v->boolean : def;
    }
};

// Returns null on parse error and fills *error with a message.
JsonPtr json_parse(const char* data, size_t len, std::string* error);

// Serialize back to standard JSON (used by the track baker's tree-shaking).
// Numbers round-trip value-exactly through the Q16.16 representation: an
// integer-valued number prints as an integer, otherwise with up to 5
// fractional digits (Q16.16's full precision).
std::string json_serialize(const JsonValue& v);

}  // namespace msynth
