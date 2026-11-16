#pragma once

// Minimal dependency-free test harness (keeps CI builds fast and offline-friendly).

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace b4ndtest
{

struct TestCase
{
    const char* name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry()
{
    static std::vector<TestCase> r;
    return r;
}

inline int& failures()
{
    static int f = 0;
    return f;
}

struct Registrar
{
    Registrar (const char* name, std::function<void()> fn) { registry().push_back ({ name, std::move (fn) }); }
};

inline void report (bool ok, const char* expr, const char* file, int line)
{
    if (! ok)
    {
        ++failures();
        std::printf ("    FAIL %s:%d  %s\n", file, line, expr);
    }
}

} // namespace b4ndtest

#define B4ND_CAT2(a, b) a##b
#define B4ND_CAT(a, b) B4ND_CAT2(a, b)

#define TEST_CASE(name)                                                         \
    static void B4ND_CAT(test_fn_, __LINE__)();                                 \
    static b4ndtest::Registrar B4ND_CAT(test_reg_, __LINE__) (name, &B4ND_CAT(test_fn_, __LINE__)); \
    static void B4ND_CAT(test_fn_, __LINE__)()

#define CHECK(expr) b4ndtest::report (static_cast<bool> (expr), #expr, __FILE__, __LINE__)
#define CHECK_NEAR(a, b, eps) b4ndtest::report (std::fabs ((a) - (b)) <= (eps), #a " ~= " #b, __FILE__, __LINE__)
