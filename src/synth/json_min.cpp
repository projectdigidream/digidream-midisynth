#include "json_min.h"

#include <cstdio>

namespace msynth {
namespace {

struct Parser {
    const char* p;
    const char* end;
    std::string* err;
    int depth = 0;

    bool fail(const char* msg) {
        if (err && err->empty()) *err = msg;
        return false;
    }

    void skip_ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }

    bool parse_value(JsonValue& out) {
        if (++depth > 64) return fail("nesting too deep");
        skip_ws();
        if (p >= end) return fail("unexpected end");
        bool ok;
        switch (*p) {
            case '{': ok = parse_object(out); break;
            case '[': ok = parse_array(out); break;
            case '"': ok = parse_string_value(out); break;
            case 't': ok = parse_lit("true", out, true); break;
            case 'f': ok = parse_lit("false", out, false); break;
            case 'n': ok = parse_null(out); break;
            default:  ok = parse_number(out); break;
        }
        --depth;
        return ok;
    }

    bool parse_lit(const char* lit, JsonValue& out, bool val) {
        for (const char* q = lit; *q; ++q, ++p)
            if (p >= end || *p != *q) return fail("bad literal");
        out.type = JsonValue::Type::Bool;
        out.boolean = val;
        return true;
    }

    bool parse_null(JsonValue& out) {
        const char* lit = "null";
        for (const char* q = lit; *q; ++q, ++p)
            if (p >= end || *p != *q) return fail("bad literal");
        out.type = JsonValue::Type::Null;
        return true;
    }

    bool parse_string(std::string& out) {
        ++p;  // opening quote
        while (p < end && *p != '"') {
            char c = *p++;
            if (c == '\\') {
                if (p >= end) return fail("bad escape");
                char e = *p++;
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        if (end - p < 4) return fail("bad \\u escape");
                        u32 cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = *p++;
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= static_cast<u32>(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= static_cast<u32>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= static_cast<u32>(h - 'A' + 10);
                            else return fail("bad \\u escape");
                        }
                        // Encode BMP codepoint as UTF-8 (surrogates unsupported).
                        if (cp < 0x80) out += static_cast<char>(cp);
                        else if (cp < 0x800) {
                            out += static_cast<char>(0xC0 | (cp >> 6));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            out += static_cast<char>(0xE0 | (cp >> 12));
                            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: return fail("bad escape");
                }
            } else {
                out += c;
            }
        }
        if (p >= end) return fail("unterminated string");
        ++p;  // closing quote
        return true;
    }

    bool parse_string_value(JsonValue& out) {
        out.type = JsonValue::Type::String;
        return parse_string(out.str);
    }

    bool parse_number(JsonValue& out) {
        bool neg = false;
        if (p < end && *p == '-') { neg = true; ++p; }
        if (p >= end || *p < '0' || *p > '9') return fail("bad number");
        i64 int_part = 0;
        while (p < end && *p >= '0' && *p <= '9') {
            if (int_part < (1LL << 40)) int_part = int_part * 10 + (*p - '0');
            ++p;
        }
        // Fraction: accumulate up to 9 digits, scale to Q16.16 by integer math.
        i64 frac_num = 0, frac_den = 1;
        if (p < end && *p == '.') {
            ++p;
            if (p >= end || *p < '0' || *p > '9') return fail("bad number");
            while (p < end && *p >= '0' && *p <= '9') {
                if (frac_den < 1000000000LL) {
                    frac_num = frac_num * 10 + (*p - '0');
                    frac_den *= 10;
                }
                ++p;
            }
        }
        if (p < end && (*p == 'e' || *p == 'E')) return fail("exponents unsupported");
        out.type = JsonValue::Type::Number;
        out.num_negative = neg;
        i64 q16 = (int_part << GAIN_SHIFT) + (frac_num << GAIN_SHIFT) / frac_den;
        if (neg) { int_part = -int_part; q16 = -q16; }
        out.num_int = int_part;
        out.num_q16 = clamp_i32(q16, -0x7FFFFFFFLL, 0x7FFFFFFFLL);
        return true;
    }

    bool parse_object(JsonValue& out) {
        out.type = JsonValue::Type::Object;
        ++p;  // '{'
        skip_ws();
        if (p < end && *p == '}') { ++p; return true; }
        for (;;) {
            skip_ws();
            if (p >= end || *p != '"') return fail("expected key");
            std::string key;
            if (!parse_string(key)) return false;
            skip_ws();
            if (p >= end || *p != ':') return fail("expected ':'");
            ++p;
            auto val = std::make_shared<JsonValue>();
            if (!parse_value(*val)) return false;
            out.obj[key] = std::move(val);
            skip_ws();
            if (p < end && *p == ',') { ++p; continue; }
            if (p < end && *p == '}') { ++p; return true; }
            return fail("expected ',' or '}'");
        }
    }

    bool parse_array(JsonValue& out) {
        out.type = JsonValue::Type::Array;
        ++p;  // '['
        skip_ws();
        if (p < end && *p == ']') { ++p; return true; }
        for (;;) {
            auto val = std::make_shared<JsonValue>();
            if (!parse_value(*val)) return false;
            out.arr.push_back(std::move(val));
            skip_ws();
            if (p < end && *p == ',') { ++p; continue; }
            if (p < end && *p == ']') { ++p; return true; }
            return fail("expected ',' or ']'");
        }
    }
};

}  // namespace

namespace {

void serialize_to(const JsonValue& v, std::string& out) {
    switch (v.type) {
        case JsonValue::Type::Null: out += "null"; break;
        case JsonValue::Type::Bool: out += v.boolean ? "true" : "false"; break;
        case JsonValue::Type::Number: {
            i32 q = v.num_q16;
            if (q < 0) { out += '-'; q = -q; }
            out += std::to_string(q >> 16);
            const u32 frac = static_cast<u32>(q) & 0xFFFFu;
            if (frac) {
                // Emit a 9-digit decimal that the parser's truncating
                // fixed-point conversion maps back to EXACTLY this Q16 value
                // (n = ceil(frac * 10^9 / 2^16); any n in the interval works,
                // and trailing-zero trimming preserves the value).
                const u64 n9 = (static_cast<u64>(frac) * 1000000000ull + 65535ull) >> 16;
                char tmp[16];
                std::snprintf(tmp, sizeof(tmp), "%09llu", static_cast<unsigned long long>(n9));
                int len = 9;
                while (len > 1 && tmp[len - 1] == '0') --len;
                out += '.';
                out.append(tmp, len);
            }
            break;
        }
        case JsonValue::Type::String: {
            out += '"';
            for (char ch : v.str) {
                if (ch == '"' || ch == '\\') { out += '\\'; out += ch; }
                else if (ch == '\n') out += "\\n";
                else if (ch == '\t') out += "\\t";
                else if (ch == '\r') out += "\\r";
                else if (static_cast<unsigned char>(ch) < 0x20) out += ' ';
                else out += ch;
            }
            out += '"';
            break;
        }
        case JsonValue::Type::Array: {
            out += '[';
            for (size_t i = 0; i < v.arr.size(); ++i) {
                if (i) out += ',';
                serialize_to(*v.arr[i], out);
            }
            out += ']';
            break;
        }
        case JsonValue::Type::Object: {
            out += '{';
            bool first = true;
            for (const auto& kv : v.obj) {
                if (!first) out += ',';
                first = false;
                out += '"' + kv.first + "\":";
                serialize_to(*kv.second, out);
            }
            out += '}';
            break;
        }
    }
}

}  // namespace

std::string json_serialize(const JsonValue& v) {
    std::string out;
    serialize_to(v, out);
    return out;
}

JsonPtr json_parse(const char* data, size_t len, std::string* error) {
    Parser parser{data, data + len, error};
    auto root = std::make_shared<JsonValue>();
    if (!parser.parse_value(*root)) return nullptr;
    parser.skip_ws();
    if (parser.p != parser.end) {
        if (error && error->empty()) *error = "trailing garbage";
        return nullptr;
    }
    return root;
}

}  // namespace msynth
