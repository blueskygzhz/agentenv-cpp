// SPDX-License-Identifier: MIT
#include "microtest.h"

#include <cstdlib>
#include <cstring>

namespace microtest {

std::vector<Case>& Registry() {
    static std::vector<Case> r;
    return r;
}

int RunAll() {
    // `MT_FILTER` runs only the cases whose name contains it. Several suites
    // here take upwards of ten minutes because each case drives a real
    // sandbox launch, which makes the whole-suite loop the wrong unit for
    // debugging a single assertion -- and makes a 26-mutation harness over
    // one of them a five-hour run. Unset, every case runs, so a harness or
    // CI invocation that passes nothing behaves exactly as before.
    const char* filter = std::getenv("MT_FILTER");
    if (filter != NULL && filter[0] == '\0') filter = NULL;

    int failed = 0;
    std::size_t ran = 0;
    for (auto& c : Registry()) {
        if (filter != NULL && std::strstr(c.name, filter) == NULL) continue;
        ++ran;
        std::printf("[TEST] %s ...\n", c.name);
        try { c.fn(); std::printf("       ok\n"); }
        catch (...) { std::printf("       FAIL (exception)\n"); ++failed; }
    }
    if (filter != NULL) {
        std::printf("--- %zu of %zu tests (MT_FILTER=%s), %d failed ---\n",
                    ran, Registry().size(), filter, failed);
        // A filter that matches nothing would otherwise report success, which
        // is the one outcome a caller must not read as "the tests passed".
        if (ran == 0) {
            std::printf("no test name contains '%s'\n", filter);
            return 1;
        }
    } else {
        std::printf("--- %zu tests, %d failed ---\n", Registry().size(), failed);
    }
    return failed == 0 ? 0 : 1;
}

}  // namespace microtest
