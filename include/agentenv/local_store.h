// SPDX-License-Identifier: MIT
// Rust: src/local_store.rs
//
// Porting note. Rust wraps RocksDB and offloads every call to Tokio's blocking
// pool. This port keeps the *interface* — durability policy, batch atomicity,
// ordered iteration, prefix scan — but the default backend is a plain on-disk
// log rather than RocksDB, because the C++ tree has no mandatory third-party
// dependency (`AGENTENV_WITH_ROCKSDB` is off by default). A RocksDB-backed
// implementation can be dropped in behind the same interface.
//
// The API is synchronous: Rust is async only to keep RocksDB's blocking calls
// off runtime workers, which is not a concern without a reactor.
#ifndef AGENTENV_LOCAL_STORE_H_
#define AGENTENV_LOCAL_STORE_H_

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"

namespace agentenv {
namespace local_store {

/// Rust enum `LocalStoreDurability`. The levels deliberately expose only the
/// knobs AgentENV needs instead of all of RocksDB's `WriteOptions`.
enum class Durability {
    /// Rust `Memory` — skip the write-ahead log. For tests and regenerable
    /// indexes, where a crash may lose the latest writes.
    Memory,
    /// Rust `Wal` — log the write but do not fsync it. Survives a process
    /// restart, not a power loss.
    Wal,
    /// Rust `Sync` — log *and* fsync before acknowledging. For small critical
    /// records where recovery beats throughput.
    Sync,
};

const char* DurabilityToString(Durability durability);
std::ostream& operator<<(std::ostream& os, Durability durability);

/// Rust enum `LocalKvBatchOp`.
struct BatchOp {
    enum class Kind { Put, Delete };

    Kind kind = Kind::Put;
    std::string key;
    std::string value;  // unused for Delete

    /// Rust `LocalKvBatchOp::put`.
    static BatchOp Put(const std::string& key, const std::string& value);
    /// Rust `LocalKvBatchOp::delete`.
    static BatchOp Delete(const std::string& key);

    bool operator==(const BatchOp& o) const;
    bool operator!=(const BatchOp& o) const { return !(*this == o); }
};

/// One key-value pair, in key order when returned from a scan.
using Entry = std::pair<std::string, std::string>;

/// Rust struct `LocalKvStore`.
///
/// Keys and values are opaque byte strings; `std::string` is used as a byte
/// container, so embedded NULs are fine.
class KvStore {
 public:
    /// Rust `LocalKvStore::open` — creates the parent directory as needed.
    static core::Expected<std::shared_ptr<KvStore>, std::string> Open(const std::string& path,
                                                                      Durability durability);

    /// Rust `get` — an absent key is an empty optional, not an error.
    core::Expected<core::Optional<std::string>, std::string> Get(const std::string& key) const;

    /// Rust `put`.
    core::Expected<core::Unit, std::string> Put(const std::string& key,
                                                const std::string& value);

    /// Rust `delete` — removing a missing key succeeds, as in RocksDB.
    core::Expected<core::Unit, std::string> Delete(const std::string& key);

    /// Rust `write_batch` — all mutations apply atomically, or none do. An
    /// empty batch is a no-op that does not touch the log.
    core::Expected<core::Unit, std::string> WriteBatch(const std::vector<BatchOp>& ops);

    /// Rust `entries` — every pair, in key order.
    core::Expected<std::vector<Entry>, std::string> Entries() const;

    /// Rust `fold` — visits every pair in key order, stopping at the first
    /// visitor error. The visitor returns a non-empty string to abort.
    core::Expected<core::Unit, std::string> Fold(
        const std::function<std::string(const std::string&, const std::string&)>& visit) const;

    /// Rust `scan_prefix` — seeks to `prefix` and stops at the first key that
    /// no longer matches, so it does not load the whole store.
    core::Expected<std::vector<Entry>, std::string> ScanPrefix(const std::string& prefix) const;

    Durability durability() const { return durability_; }

    /// Rust `impl Debug for LocalKvStore` — `finish_non_exhaustive`, so only
    /// the durability shows; keys and values are never printed.
    std::string ToDebugString() const;

 private:
    KvStore() : durability_(Durability::Wal) {}

    /// Appends a durable record for `ops` and applies them in memory.
    core::Expected<core::Unit, std::string> Commit(const std::vector<BatchOp>& ops);
    /// Replays the log at `log_path_` into `entries_`.
    core::Expected<core::Unit, std::string> Replay();

    /// Ordered, so iteration and prefix scans are key-ordered for free.
    std::map<std::string, std::string> entries_;
    mutable std::mutex mutex_;
    std::string log_path_;
    Durability durability_;
};

}  // namespace local_store
}  // namespace agentenv
#endif  // AGENTENV_LOCAL_STORE_H_
