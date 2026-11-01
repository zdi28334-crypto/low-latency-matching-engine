// Minimal dependency-free test harness.
#pragma once
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

struct TestCase { const char* name; std::function<void()> fn; };
inline std::vector<TestCase>& registry() { static std::vector<TestCase> r; return r; }
inline int& failures() { static int f = 0; return f; }

struct TestReg { TestReg(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); } };

#define TEST(name) \
    static void name(); \
    static TestReg reg_##name(#name, name); \
    static void name()

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::printf("    FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            ++failures();                                                           \
        }                                                                           \
    } while (0)

#define CHECK_EQ(a, b)                                                              \
    do {                                                                            \
        auto _a = (a); auto _b = (b);                                               \
        if (!(_a == _b)) {                                                          \
            std::printf("    FAIL %s:%d  %s == %s  (%lld vs %lld)\n", __FILE__,      \
                        __LINE__, #a, #b, (long long)_a, (long long)_b);            \
            ++failures();                                                           \
        }                                                                           \
    } while (0)
