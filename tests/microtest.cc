// SPDX-License-Identifier: MIT
#include "microtest.h"

namespace microtest {

std::vector<Case>& Registry() {
    static std::vector<Case> r;
    return r;
}

int RunAll() {
    int failed = 0;
    for (auto& c : Registry()) {
        std::printf("[TEST] %s ...\n", c.name);
        try { c.fn(); std::printf("       ok\n"); }
        catch (...) { std::printf("       FAIL (exception)\n"); ++failed; }
    }
    std::printf("--- %zu tests, %d failed ---\n", Registry().size(), failed);
    return failed == 0 ? 0 : 1;
}

}  // namespace microtest
