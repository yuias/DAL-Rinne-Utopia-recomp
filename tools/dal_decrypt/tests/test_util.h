#pragma once

#include <cstdio>
#include <vector>

namespace dal::decrypt::test {

struct TestCase {
    const char* name;
    void (*run)();
};

struct TestCounters {
    int passed = 0;
    int failed = 0;
};

// Function-local statics: the registry is used from registration objects in other translation units
// during static initialization, so it must exist on first use.
inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

inline TestCounters& counters() {
    static TestCounters value;
    return value;
}

struct Registrar {
    Registrar(const char* name, void (*run)()) { registry().push_back({name, run}); }
};

}  // namespace dal::decrypt::test

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (cond) {                                                                \
            ::dal::decrypt::test::counters().passed++;                             \
        } else {                                                                   \
            ::dal::decrypt::test::counters().failed++;                             \
            std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                          \
    } while (0)

// Defines `void name()` and registers it, so a new test file needs no edit of test_main.cpp.
#define DAL_TEST(name)                                                    \
    void name();                                                          \
    namespace {                                                           \
    const ::dal::decrypt::test::Registrar name##_registrar(#name, &name); \
    }                                                                     \
    void name()
