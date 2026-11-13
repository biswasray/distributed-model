// Minimal test harness: TEST_CASE / CHECK / REQUIRE, run by xgi_tests.exe (CTest).
#pragma once

#include <exception>
#include <format>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace testing {

struct Case {
    const char* name;
    std::function<void()> fn;
};
inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}
inline int& failures() {
    static int f = 0;
    return f;
}
struct Registrar {
    Registrar(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); }
};
struct RequireFailed : std::exception {};

inline void report(const char* file, int line, const std::string& expr) {
    ++failures();
    std::cerr << std::format("  {}({}): CHECK failed: {}\n", file, line, expr);
}

}  // namespace testing

#define TT_CAT2(a, b) a##b
#define TT_CAT(a, b) TT_CAT2(a, b)
#define TEST_CASE(name)                                                          \
    static void TT_CAT(test_fn_, __LINE__)();                                    \
    static testing::Registrar TT_CAT(test_reg_, __LINE__)(name, TT_CAT(test_fn_, __LINE__)); \
    static void TT_CAT(test_fn_, __LINE__)()

#define CHECK(expr) \
    do { if (!(expr)) testing::report(__FILE__, __LINE__, #expr); } while (0)
#define CHECK_EQ(a, b)                                                                                    \
    do {                                                                                                  \
        auto&& va_ = (a);                                                                                 \
        auto&& vb_ = (b);                                                                                 \
        if (!(va_ == vb_)) testing::report(__FILE__, __LINE__, std::format("{} == {}  ({} vs {})", #a, #b, va_, vb_)); \
    } while (0)
#define REQUIRE(expr) \
    do { if (!(expr)) { testing::report(__FILE__, __LINE__, #expr); throw testing::RequireFailed{}; } } while (0)
#define CHECK_THROWS(expr)                                                  \
    do {                                                                    \
        bool thrown_ = false;                                               \
        try { (void)(expr); } catch (...) { thrown_ = true; }              \
        if (!thrown_) testing::report(__FILE__, __LINE__, "throws: " #expr); \
    } while (0)
