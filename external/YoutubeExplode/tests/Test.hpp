#pragma once

// Minimal self-contained test harness (no external framework dependency).

#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace yte_test {

struct TestCase {
    const char* name;
    std::function<void()> body;
};

std::vector<TestCase>& registry();

struct Registrar {
    Registrar(const char* name, std::function<void()> body) { registry().push_back({name, std::move(body)}); }
};

struct Failure {
    std::string message;
};

/// Marks the current test as skipped (e.g. optional fixtures missing).
struct Skip {
    std::string reason;
};

} // namespace yte_test

#define YTE_CONCAT_(a, b) a##b
#define YTE_CONCAT(a, b) YTE_CONCAT_(a, b)
#define TEST(name)                                                                              \
    static void YTE_CONCAT(test_, name)();                                                      \
    static ::yte_test::Registrar YTE_CONCAT(registrar_, name)(#name, &YTE_CONCAT(test_, name)); \
    static void YTE_CONCAT(test_, name)()

#define CHECK(cond)                                                                                   \
    do {                                                                                              \
        if (!(cond)) {                                                                                \
            std::ostringstream oss_;                                                                  \
            oss_ << __FILE__ << ":" << __LINE__ << ": CHECK(" #cond ") failed";                       \
            throw ::yte_test::Failure{oss_.str()};                                                    \
        }                                                                                             \
    } while (0)

#define CHECK_EQ(a, b)                                                                                \
    do {                                                                                              \
        const auto va_ = (a);                                                                         \
        const auto vb_ = (b);                                                                         \
        if (!(va_ == vb_)) {                                                                          \
            std::ostringstream oss_;                                                                  \
            oss_ << __FILE__ << ":" << __LINE__ << ": CHECK_EQ(" #a ", " #b ") failed\n    left:  "   \
                 << va_ << "\n    right: " << vb_;                                                    \
            throw ::yte_test::Failure{oss_.str()};                                                    \
        }                                                                                             \
    } while (0)

#define CHECK_THROWS_AS(expr, type)                                                                   \
    do {                                                                                              \
        bool thrown_ = false;                                                                         \
        try {                                                                                         \
            (void)(expr);                                                                             \
        } catch (const type&) {                                                                       \
            thrown_ = true;                                                                           \
        }                                                                                             \
        if (!thrown_)                                                                                 \
            throw ::yte_test::Failure{std::string(__FILE__) + ":" + std::to_string(__LINE__) +        \
                                      ": expected " #type " from " #expr};                            \
    } while (0)
