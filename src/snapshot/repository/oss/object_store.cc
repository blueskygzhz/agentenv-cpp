// SPDX-License-Identifier: MIT
// Rust: the `list_keys_recursive` half of src/repository/backends/oss/client.rs
#include "agentenv/snapshot/repository/oss/object_store.h"

namespace agentenv {
namespace snapshot {
namespace repository {
namespace oss {

namespace {

/// The protocol's own page ceiling, so each round trip carries as much as the
/// server will give.
const std::size_t kListPageSize = 1000;

}  // namespace

RepositoryResult<std::vector<std::string> > ObjectStore::ListKeysRecursive(
    const std::string& prefix) const {
    std::vector<std::string> all;
    core::Optional<std::string> cursor;

    while (true) {
        const RepositoryResult<std::vector<std::string> > page =
            ListKeysPage(prefix, cursor, kListPageSize);
        if (!page.ok()) return core::make_unexpected(page.error());
        if (page.value().empty()) return all;

        // A page that does not advance past its own cursor would loop
        // forever. Reported rather than broken out of: silently stopping
        // would return a listing that is short for no visible reason.
        if (cursor.has_value() && page.value().back() <= *cursor) {
            return core::make_unexpected(RepositoryError::Backend(
                "list '" + prefix + "' did not advance past '" + *cursor + "'"));
        }

        cursor = core::Optional<std::string>(page.value().back());
        for (std::size_t i = 0; i < page.value().size(); ++i) {
            all.push_back(page.value()[i]);
        }

        // A short page is the last one: the server had nothing more to give
        // under this prefix.
        if (page.value().size() < kListPageSize) return all;
    }
}

}  // namespace oss
}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
