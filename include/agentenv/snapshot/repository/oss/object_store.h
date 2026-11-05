// SPDX-License-Identifier: MIT
// Rust: the object-store operations `src/repository/backends/oss/client.rs`
// exposes to `documents.rs` (`get_bytes_with_etag`, `put_bytes_conditionally`,
// `put_bytes`, `delete`, `list_keys_page`, `can_append`).
//
// Rust reaches object storage through `opendal`. There is no C++ equivalent,
// and the existing `storage/overlaybd/oss.h` backend cannot serve here: it is
// shaped for reading overlaybd image layers (ranged GET, HEAD, multipart
// upload) and has no conditional write, no delete and no listing — which are
// exactly the three operations a shared catalog needs.
//
// So this is the narrow interface the document layer actually requires,
// separated from any transport so that the compare-and-swap logic above it is
// testable without a server. That separation is not incidental: the value of
// a CAS is entirely in what happens when two writers race, and a test that
// cannot produce a race cannot show the CAS works.
#ifndef AGENTENV_SNAPSHOT_REPOSITORY_OSS_OBJECT_STORE_H_
#define AGENTENV_SNAPSHOT_REPOSITORY_OSS_OBJECT_STORE_H_

#include <cstddef>
#include <string>
#include <vector>

#include "agentenv/core/optional.h"
#include "agentenv/snapshot/repository/errors.h"

namespace agentenv {
namespace snapshot {
namespace repository {
namespace oss {

/// Rust `get_bytes_with_etag` -> `Result<Option<(Bytes, Option<String>)>>`,
/// flattened into one three-state answer.
///
/// The three states must stay distinct, and conflating any two of them breaks
/// the update loop in a way that only appears under concurrency:
///
///   * `Found`    — the object was read together with a version token that
///                  describes *these* bytes.
///   * `Changed`  — the object was replaced between reading its version and
///                  reading its bytes (opendal `ConditionNotMatch`). Nothing
///                  consistent was obtained, so there is nothing to decide
///                  from; the caller backs off and reads again.
///   * `NotFound` — the object does not exist, so an update creates it.
///
/// Folding `Changed` into `NotFound` is the dangerous one: a document being
/// actively rewritten would be treated as absent, the update would then write
/// it create-only, and that write can never succeed while the object exists.
/// A hot document would stop being updatable at all.
struct ObjectRead {
    enum class Kind { Found, Changed, NotFound };

    Kind kind;
    /// Set for `Found`.
    std::string bytes;
    /// Set for `Found`, and only when the backend supplies one. An absent
    /// token means this store cannot support a conditional update, which the
    /// caller reports rather than silently writing unconditionally.
    core::Optional<std::string> etag;

    static ObjectRead Found(const std::string& bytes,
                            const core::Optional<std::string>& etag) {
        ObjectRead r;
        r.kind = Kind::Found;
        r.bytes = bytes;
        r.etag = etag;
        return r;
    }
    static ObjectRead Changed() {
        ObjectRead r;
        r.kind = Kind::Changed;
        return r;
    }
    static ObjectRead NotFound() {
        ObjectRead r;
        r.kind = Kind::NotFound;
        return r;
    }

 private:
    ObjectRead() : kind(Kind::NotFound) {}
};

/// The object-store operations the shared document layer needs.
class ObjectStore {
 public:
    virtual ~ObjectStore() {}

    /// Rust `can_append` — true only for Alibaba's native OSS, which cannot
    /// write conditionally and therefore serialises document writes with a
    /// lease instead of a compare-and-swap.
    ///
    /// The document layer dispatches on this. It is reported rather than
    /// assumed because picking the wrong strategy loses updates silently: a
    /// CAS against a store that ignores the precondition overwrites whatever
    /// landed in between.
    virtual bool CanAppend() const = 0;

    /// Rust `get_bytes_with_etag`.
    virtual RepositoryResult<ObjectRead> GetWithEtag(const std::string& key) const = 0;

    /// Rust `put_bytes_conditionally` — `false` when the precondition failed
    /// (HTTP 412), which is a lost race rather than an error.
    ///
    /// An absent `etag` means "only if the object does not exist", the
    /// create-only form an update uses when it read `NotFound`.
    virtual RepositoryResult<bool> PutConditionally(const std::string& key,
                                                    const std::string& bytes,
                                                    const core::Optional<std::string>& etag) = 0;

    /// Rust `put_bytes` — unconditional replace.
    virtual RepositoryResult<core::Unit> Put(const std::string& key,
                                             const std::string& bytes) = 0;

    /// Rust `delete` — a missing object is success, as it is on every backend.
    virtual RepositoryResult<core::Unit> Delete(const std::string& key) = 0;

    /// Rust `list_keys_page` — up to `limit` keys under `prefix`, in object
    /// key order, strictly after `start_after`.
    virtual RepositoryResult<std::vector<std::string> > ListKeysPage(
        const std::string& prefix, const core::Optional<std::string>& start_after,
        std::size_t limit) const = 0;
};

}  // namespace oss
}  // namespace repository
}  // namespace snapshot
}  // namespace agentenv
#endif  // AGENTENV_SNAPSHOT_REPOSITORY_OSS_OBJECT_STORE_H_
