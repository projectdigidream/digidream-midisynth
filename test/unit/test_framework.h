#pragma once
//
// Tiny test framework: TEST(name) registers a function; the runner executes
// all of them and reports failures with file:line.
//
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace testfw {

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

inline int failures = 0;
inline int checks = 0;
inline const char* current_test = "";

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, fn}); }
};

inline int run_all() {
    int failed_tests = 0;
    for (auto& t : registry()) {
        current_test = t.name;
        const int before = failures;
        t.fn();
        if (failures != before) {
            ++failed_tests;
            std::printf("FAIL %s\n", t.name);
        }
    }
    std::printf("%zu tests, %d checks, %d failures\n", registry().size(), checks, failures);
    return failed_tests == 0 ? 0 : 1;
}

}  // namespace testfw

#define TEST(name)                                                         \
    static void test_##name();                                             \
    static testfw::Registrar reg_##name(#name, test_##name);               \
    static void test_##name()

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++testfw::checks;                                                  \
        if (!(cond)) {                                                     \
            ++testfw::failures;                                            \
            std::printf("  CHECK failed [%s] %s:%d: %s\n",                 \
                        testfw::current_test, __FILE__, __LINE__, #cond);  \
        }                                                                  \
    } while (0)

#define CHECK_EQ(a, b)                                                     \
    do {                                                                   \
        ++testfw::checks;                                                  \
        const long long chk_va_ = (long long)(a);                          \
        const long long chk_vb_ = (long long)(b);                          \
        if (chk_va_ != chk_vb_) {                                          \
            ++testfw::failures;                                            \
            std::printf("  CHECK_EQ failed [%s] %s:%d: %s=%lld vs %s=%lld\n",       \
                        testfw::current_test, __FILE__, __LINE__, #a,      \
                        chk_va_, #b, chk_vb_);                             \
        }                                                                  \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                              \
    do {                                                                   \
        ++testfw::checks;                                                  \
        long long va = (long long)(a);                                     \
        long long vb = (long long)(b);                                     \
        long long d = va > vb ? va - vb : vb - va;                         \
        if (d > (long long)(tol)) {                                        \
            ++testfw::failures;                                            \
            std::printf("  CHECK_NEAR failed [%s] %s:%d: %s=%lld vs %s=%lld (tol %lld)\n", \
                        testfw::current_test, __FILE__, __LINE__, #a, va,  \
                        #b, vb, (long long)(tol));                         \
        }                                                                  \
    } while (0)
