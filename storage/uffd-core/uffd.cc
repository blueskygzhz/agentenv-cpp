// SPDX-License-Identifier: MIT
// Rust: storage/uffd-core/
#include "agentenv/storage/uffd-core/uffd.h"

namespace agentenv { namespace storage { namespace uffd {
core::Expected<Uffd*, std::string> Open() {
    return core::make_unexpected(std::string("userfaultfd not compiled in"));
}
}}}
