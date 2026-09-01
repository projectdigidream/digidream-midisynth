#include "test_framework.h"

#include "synth/json_min.h"

using namespace msynth;

namespace {
JsonPtr parse(const char* s, std::string* err = nullptr) {
    std::string local;
    return json_parse(s, std::strlen(s), err ? err : &local);
}
}  // namespace

TEST(json_numbers_to_q16_without_strtod) {
    JsonPtr v = parse(R"({"a": 1.5, "b": -0.25, "c": 42, "d": 0.0001, "e": -7})");
    CHECK(v && v->is_obj());
    CHECK_EQ(v->get_q16("a", 0), GAIN_ONE + GAIN_ONE / 2);
    CHECK_EQ(v->get_q16("b", 0), -GAIN_ONE / 4);
    CHECK_EQ(v->get_q16("c", 0), 42 * GAIN_ONE);
    CHECK_EQ(v->get_int("c", 0), 42);
    CHECK_NEAR(v->get_q16("d", 0), 7, 2);  // 0.0001 * 65536 = 6.55
    CHECK_EQ(v->get_int("e", 0), -7);
    CHECK_EQ(v->get_q16("e", 0), -7 * GAIN_ONE);
}

TEST(json_long_fraction_truncates_not_crashes) {
    JsonPtr v = parse(R"({"x": 0.123456789012345678901234567890})");
    CHECK(v != nullptr);
    CHECK_NEAR(v->get_q16("x", 0), 8091, 2);  // 0.12345... * 65536
}

TEST(json_exponents_rejected) {
    std::string err;
    CHECK(parse(R"({"x": 1e5})", &err) == nullptr);
    CHECK(!err.empty());
}

TEST(json_strings_and_escapes) {
    JsonPtr v = parse(R"({"s": "a\"b\\c\ndA"})");
    CHECK(v != nullptr);
    CHECK(v->get_str("s", "") == "a\"b\\c\ndA");
}

TEST(json_nested_structures) {
    JsonPtr v = parse(R"({"arr": [1, {"k": [true, false, null]}, "s"]})");
    CHECK(v != nullptr);
    const JsonValue* arr = v->get("arr");
    CHECK(arr && arr->is_arr());
    CHECK_EQ(arr->arr.size(), 3);
    CHECK(arr->arr[1]->get("k")->arr[0]->boolean);
    CHECK(arr->arr[1]->get("k")->arr[2]->type == JsonValue::Type::Null);
}

TEST(json_rejects_garbage) {
    std::string err;
    CHECK(parse("{", &err) == nullptr);
    CHECK(parse("{\"a\":}", &err) == nullptr);
    CHECK(parse("[1,]", &err) == nullptr);
    CHECK(parse("{\"a\":1} trailing", &err) == nullptr);
    CHECK(parse("tru", &err) == nullptr);
    CHECK(parse("\"unterminated", &err) == nullptr);
    CHECK(parse("", &err) == nullptr);
}

TEST(json_rejects_deep_nesting) {
    std::string s;
    for (int i = 0; i < 100; ++i) s += "[";
    std::string err;
    CHECK(parse(s.c_str(), &err) == nullptr);
    CHECK(err == "nesting too deep");
}

TEST(json_missing_keys_yield_defaults) {
    JsonPtr v = parse(R"({"present": 5})");
    CHECK_EQ(v->get_int("absent", 99), 99);
    CHECK_EQ(v->get_q16("absent", -1), -1);
    CHECK(v->get_str("absent", "dflt") == "dflt");
    CHECK_EQ(v->get_bool("absent", true), true);
    // Wrong-typed access falls back to default too.
    CHECK(v->get_str("present", "dflt") == "dflt");
}

TEST(json_empty_containers) {
    JsonPtr v = parse(R"({"o": {}, "a": []})");
    CHECK(v != nullptr);
    CHECK(v->get("o")->is_obj());
    CHECK(v->get("a")->is_arr());
    CHECK_EQ(v->get("a")->arr.size(), 0);
}

TEST(json_serialize_roundtrips_every_q16_exactly) {
    // The track baker re-serializes patch JSON; a single Q16 ulp of drift
    // would silently change the baked audio. Sweep the fraction space.
    XorShift32 rng(99);
    for (int i = 0; i < 5000; ++i) {
        JsonValue v;
        v.type = JsonValue::Type::Number;
        v.num_q16 = static_cast<i32>(rng.next() & 0x7FFFFFF);  // up to ~2048.0
        if (rng.next() & 1) v.num_q16 = -v.num_q16;
        v.num_int = v.num_q16 / GAIN_ONE;
        const std::string s = json_serialize(v);
        std::string err;
        JsonPtr back = json_parse(s.data(), s.size(), &err);
        CHECK(back != nullptr);
        CHECK_EQ(back->num_q16, v.num_q16);
    }
    // Plus the classic decimal landmines.
    for (i32 q : {45875 /*0.7*/, 6554 /*0.1*/, 1, 65535, GAIN_ONE + 1, 3 * GAIN_ONE / 10}) {
        JsonValue v;
        v.type = JsonValue::Type::Number;
        v.num_q16 = q;
        const std::string s = json_serialize(v);
        std::string err;
        JsonPtr back = json_parse(s.data(), s.size(), &err);
        CHECK_EQ(back->num_q16, q);
    }
}
