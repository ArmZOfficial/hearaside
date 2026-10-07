// Minimal self-contained test harness (no external dependency).
#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace sstest {

struct Case { const char* name; std::function<void()> fn; };
inline std::vector<Case>& registry() { static std::vector<Case> r; return r; }
inline int& failures() { static int f = 0; return f; }
inline const char*& current() { static const char* c = ""; return c; }

struct Registrar { Registrar(const char* n, std::function<void()> f) { registry().push_back({ n, std::move(f) }); } };

inline void fail(const char* file, int line, const std::string& msg) {
    ++failures();
    std::printf("  FAIL [%s] %s:%d: %s\n", current(), file, line, msg.c_str());
}

} // namespace sstest

#define SS_CAT2(a, b) a##b
#define SS_CAT(a, b) SS_CAT2(a, b)
#define TEST_CASE(name) \
    static void SS_CAT(ss_test_, __LINE__)(); \
    static ::sstest::Registrar SS_CAT(ss_reg_, __LINE__)(name, &SS_CAT(ss_test_, __LINE__)); \
    static void SS_CAT(ss_test_, __LINE__)()

#define CHECK(cond) do { if (!(cond)) ::sstest::fail(__FILE__, __LINE__, #cond); } while (0)
#define REQUIRE(cond) do { if (!(cond)) { ::sstest::fail(__FILE__, __LINE__, #cond); return; } } while (0)
#define CHECK_NEAR(a, b, tol) do { const double _a = double(a), _b = double(b); \
    if (!(std::abs(_a - _b) <= double(tol))) { char _m[256]; std::snprintf(_m, sizeof _m, "%s = %g, expected %g (tol %g)", #a, _a, _b, double(tol)); ::sstest::fail(__FILE__, __LINE__, _m); } } while (0)
#define CHECK_LT(a, b) do { const double _a = double(a), _b = double(b); \
    if (!(_a < _b)) { char _m[256]; std::snprintf(_m, sizeof _m, "%s = %g, expected < %g", #a, _a, _b); ::sstest::fail(__FILE__, __LINE__, _m); } } while (0)
