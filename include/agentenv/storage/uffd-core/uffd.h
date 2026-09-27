// SPDX-License-Identifier: MIT
// Rust: storage/uffd-core/ — userfaultfd wrapper used by snapshot restore.
#ifndef AGENTENV_STORAGE_UFFD_CORE_H_
#define AGENTENV_STORAGE_UFFD_CORE_H_

#include <cstdint>
#include <string>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace storage {
namespace uffd {

/// Rust struct `Uffd` — a handle around userfaultfd(2).
class Uffd {
 public:
    virtual ~Uffd() {}
    virtual core::Expected<core::Unit, std::string>
        Register(uint64_t start, uint64_t len) = 0;
    virtual core::Expected<core::Unit, std::string>
        Copy(uint64_t dest, const void* src, uint64_t len) = 0;
    virtual core::Expected<core::Unit, std::string> Close() = 0;
};

/// Rust `Uffd::open` factory.
core::Expected<Uffd*, std::string> Open();

}  // namespace uffd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_UFFD_CORE_H_
