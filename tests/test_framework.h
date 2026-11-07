// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre
//
// Minimal in-tree test framework.
//
// Why not GoogleTest / Catch2 / doctest: those are excellent but they
// add a build-system dependency we do not otherwise have. Our test
// surface is small (a handful of building blocks: FrameQueue,
// FrameStorePool, VideoFormat) and a handful of macros is enough to
// catch regressions of the synthetic-path contract. If the surface
// grows past a few hundred assertions, swap this out for doctest
// (header-only, single file, drop-in replacement of EXPECT_/ASSERT_
// style macros).
//
// Usage in a test source:
//
//   #include "test_framework.h"
//   TEST_CASE(MyComponent, BasicHappyPath) {
//       EXPECT_EQ(MyComponent::Compute(), 42);
//   }
//   TEST_CASE(MyComponent, EdgeCase) {
//       ASSERT_TRUE(MyComponent::Validate("input"));
//   }
//
// Each TEST_CASE registers itself with the runner via a global
// constructor; calling RunAllTests() in main() executes them all
// in registration order and exits with the count of failures.
//
// The macros report failure with file/line and a one-line description
// but do not throw. ASSERT_* aborts the current test case (returns
// from the test function early); EXPECT_* records the failure and
// keeps going so a single test can find multiple problems at once.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace ajainput::testing
{

/// Best-effort printable form of a compared value, so EXPECT_EQ / EXPECT_NE
/// failures show what the two sides actually were.
template <typename T>
std::string ToTestString(const T& v)
{
    using U = std::decay_t<T>;
    char buf[64];
    if constexpr (std::is_null_pointer_v<U>)
        return "nullptr";
    else if constexpr (std::is_same_v<U, const char*> ||
                       std::is_same_v<U, char*>)
        return v ? std::string(v) : std::string("nullptr");
    else if constexpr (std::is_same_v<U, bool>)
        return v ? "true" : "false";
    else if constexpr (std::is_enum_v<U>)
    {
        std::snprintf(buf, sizeof(buf), "%lld",
                      static_cast<long long>(
                          static_cast<std::underlying_type_t<U>>(v)));
        return buf;
    }
    else if constexpr (std::is_floating_point_v<U>)
    {
        std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
        return buf;
    }
    else if constexpr (std::is_integral_v<U> && std::is_signed_v<U>)
    {
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
        return buf;
    }
    else if constexpr (std::is_integral_v<U>)
    {
        std::snprintf(buf, sizeof(buf), "%llu",
                      static_cast<unsigned long long>(v));
        return buf;
    }
    else if constexpr (std::is_pointer_v<U>)
    {
        std::snprintf(buf, sizeof(buf), "%p",
                      static_cast<const void*>(v));
        return buf;
    }
    else if constexpr (std::is_convertible_v<U, std::string>)
        return std::string(v);
    else
        return "?";
}

struct TestFailure
{
    std::string suite;
    std::string name;
    std::string file;
    int         line;
    std::string message;
};

class TestRegistry
{
public:
    using TestFn = bool (*)(std::vector<TestFailure>&);

    struct Entry
    {
        const char* suite;
        const char* name;
        TestFn      fn;
    };

    static TestRegistry& Instance()
    {
        static TestRegistry r;
        return r;
    }

    void Add(const char* suite, const char* name, TestFn fn)
    {
        m_entries.push_back({suite, name, fn});
    }

    int RunAll()
    {
        int passed = 0;
        int failed = 0;
        std::vector<TestFailure> failures;

        for (auto& e : m_entries)
        {
            failures.clear();
            const bool ok = e.fn(failures);
            if (ok && failures.empty())
            {
                std::printf("[  PASS  ] %s.%s\n", e.suite, e.name);
                ++passed;
            }
            else
            {
                std::printf("[  FAIL  ] %s.%s\n", e.suite, e.name);
                for (const auto& f : failures)
                    std::printf("           %s:%d: %s\n",
                                f.file.c_str(), f.line, f.message.c_str());
                ++failed;
            }
        }
        std::printf("\n%d passed, %d failed, %d total\n",
                    passed, failed,
                    static_cast<int>(m_entries.size()));
        return failed;
    }

private:
    std::vector<Entry> m_entries;
};

struct AutoRegister
{
    AutoRegister(const char* suite, const char* name,
                 TestRegistry::TestFn fn)
    {
        TestRegistry::Instance().Add(suite, name, fn);
    }
};

} // namespace ajainput::testing

// =========================================================================
// Macros.
// =========================================================================

#define TEST_CASE(suite_name, test_name)                                      \
    static bool suite_name##_##test_name##_impl(                              \
        std::vector<ajainput::testing::TestFailure>& _failures);                 \
    static ::ajainput::testing::AutoRegister                                     \
        suite_name##_##test_name##_register(                                  \
            #suite_name, #test_name,                                          \
            &suite_name##_##test_name##_impl);                                \
    static bool suite_name##_##test_name##_impl(                              \
        std::vector<ajainput::testing::TestFailure>& _failures)

#define EXPECT_TRUE_MSG(cond, msg)                                            \
    do {                                                                      \
        if (!(cond))                                                          \
            _failures.push_back({"", "", __FILE__, __LINE__, (msg)});         \
    } while (0)

#define EXPECT_TRUE(cond)                                                     \
    EXPECT_TRUE_MSG((cond), "EXPECT_TRUE(" #cond ") failed")

#define EXPECT_FALSE(cond)                                                    \
    EXPECT_TRUE_MSG(!(cond), "EXPECT_FALSE(" #cond ") failed")

#define EXPECT_EQ(a, b)                                                       \
    do {                                                                      \
        auto _va = (a);                                                       \
        auto _vb = (b);                                                       \
        if (!(_va == _vb))                                                    \
        {                                                                     \
            _failures.push_back({"", "", __FILE__, __LINE__,                  \
                std::string("EXPECT_EQ(" #a ", " #b ") failed: ") +            \
                ::ajainput::testing::ToTestString(_va) + " vs " +                \
                ::ajainput::testing::ToTestString(_vb)});                        \
        }                                                                     \
    } while (0)

#define EXPECT_NE(a, b)                                                       \
    do {                                                                      \
        auto _va = (a);                                                       \
        auto _vb = (b);                                                       \
        if ((_va == _vb))                                                     \
        {                                                                     \
            _failures.push_back({"", "", __FILE__, __LINE__,                  \
                std::string("EXPECT_NE(" #a ", " #b ") failed: ") +            \
                ::ajainput::testing::ToTestString(_va) + " vs " +                \
                ::ajainput::testing::ToTestString(_vb)});                        \
        }                                                                     \
    } while (0)

#define ASSERT_TRUE(cond)                                                     \
    do {                                                                      \
        if (!(cond))                                                          \
        {                                                                     \
            _failures.push_back({"", "", __FILE__, __LINE__,                  \
                                 "ASSERT_TRUE(" #cond ") failed"});           \
            return false;                                                     \
        }                                                                     \
    } while (0)

#define TEST_RETURN_OK return (_failures.empty())
