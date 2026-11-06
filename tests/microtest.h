// SPDX-License-Identifier: MIT
// A ~100-line, header-only style micro test framework so the skeleton compiles
// and `ctest` runs without any 3rd-party dep. Migrate to GoogleTest later.
#ifndef AGENTENV_MICROTEST_H_
#define AGENTENV_MICROTEST_H_

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace microtest {

struct Case {
    const char* name;
    std::function<void()> fn;
};

std::vector<Case>& Registry();
int RunAll();

#define MT_TEST(NAME)                                                                \
    static void mt_##NAME();                                                         \
    namespace {                                                                      \
    struct MtReg_##NAME {                                                            \
        MtReg_##NAME() {                                                             \
            ::microtest::Registry().push_back(::microtest::Case{#NAME, mt_##NAME});  \
        }                                                                            \
    };                                                                               \
    static MtReg_##NAME mt_reg_##NAME;                                               \
    }                                                                                \
    static void mt_##NAME()

/// The stringized condition is passed as an *argument*, never concatenated
/// into the format string.
///
/// Load-bearing: a condition containing a string literal with a `%` -- a
/// percent-encoded URL (`"a%2Db"`), a `stat -c %d` command line -- would
/// otherwise have it interpreted as a conversion specifier with no argument
/// behind it. That is undefined behaviour, and it fires precisely when an
/// assertion fails, so the run can crash inside the code meant to tell you
/// what went wrong and take the real cause with it.
#define MT_EXPECT_TRUE(cond)                                                         \
    do { if (!(cond)) {                                                              \
        std::fprintf(stderr, "  FAIL: %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
        std::exit(1);                                                                \
    } } while (0)

#define MT_EXPECT_EQ(a, b)                                                           \
    do { auto _a = (a); auto _b = (b);                                               \
        if (!(_a == _b)) {                                                           \
            std::ostringstream _os; _os << "expected " << _a << " == " << _b;        \
            std::fprintf(stderr, "  FAIL: %s:%d  %s\n", __FILE__, __LINE__, _os.str().c_str()); \
            std::exit(1);                                                            \
        } } while (0)

#define MT_MAIN                                                                      \
    int main() { return ::microtest::RunAll(); }

}  // namespace microtest
#endif
