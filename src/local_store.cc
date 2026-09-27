// SPDX-License-Identifier: MIT
// Rust: src/local_store.rs
#include "agentenv/local_store.h"

#include <cerrno>
#include <cstring>
#include <sstream>

#include <fcntl.h>
#include <unistd.h>

#include "agentenv/core/fs.h"

namespace agentenv {
namespace local_store {
namespace {

using core::Optional;
using core::Unit;
namespace fs = core::fs;

/// Record framing for the append-only log:
///
///     <op:1> <key_len:8 hex> <value_len:8 hex> <key bytes> <value bytes>
///
/// Lengths are fixed-width hex so a record can be parsed without scanning, and
/// keys/values stay fully binary-safe (no delimiter to escape).
const char kOpPut = 'P';
const char kOpDelete = 'D';
const std::size_t kLenWidth = 8;
const std::size_t kHeaderLen = 1 + kLenWidth * 2;

std::string EncodeLength(std::size_t value) {
    static const char* const kDigits = "0123456789abcdef";
    std::string out(kLenWidth, '0');
    for (std::size_t i = 0; i < kLenWidth; ++i) {
        out[kLenWidth - 1 - i] = kDigits[(value >> (i * 4)) & 0xF];
    }
    return out;
}

bool DecodeLength(const std::string& text, std::size_t offset, std::size_t* out) {
    std::size_t value = 0;
    for (std::size_t i = 0; i < kLenWidth; ++i) {
        const char c = text[offset + i];
        int digit;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else {
            return false;
        }
        value = (value << 4) | static_cast<std::size_t>(digit);
    }
    *out = value;
    return true;
}

std::string EncodeRecord(const BatchOp& op) {
    std::string out;
    out.push_back(op.kind == BatchOp::Kind::Put ? kOpPut : kOpDelete);
    out += EncodeLength(op.key.size());
    out += EncodeLength(op.kind == BatchOp::Kind::Put ? op.value.size() : 0);
    out += op.key;
    if (op.kind == BatchOp::Kind::Put) out += op.value;
    return out;
}

}  // namespace

const char* DurabilityToString(Durability durability) {
    switch (durability) {
        case Durability::Memory:
            return "Memory";
        case Durability::Wal:
            return "Wal";
        case Durability::Sync:
            return "Sync";
    }
    return "Wal";
}

std::ostream& operator<<(std::ostream& os, Durability durability) {
    return os << DurabilityToString(durability);
}

BatchOp BatchOp::Put(const std::string& key, const std::string& value) {
    BatchOp op;
    op.kind = Kind::Put;
    op.key = key;
    op.value = value;
    return op;
}

BatchOp BatchOp::Delete(const std::string& key) {
    BatchOp op;
    op.kind = Kind::Delete;
    op.key = key;
    return op;
}

bool BatchOp::operator==(const BatchOp& o) const {
    if (kind != o.kind || key != o.key) return false;
    // A Delete carries no value, so comparing it would be meaningless.
    return kind == Kind::Delete || value == o.value;
}

core::Expected<std::shared_ptr<KvStore>, std::string> KvStore::Open(const std::string& path,
                                                                    Durability durability) {
    const Optional<std::string> parent = fs::Parent(path);
    if (parent.has_value()) {
        const core::Expected<Unit, std::string> made = fs::CreateDirAll(*parent);
        if (!made.ok()) {
            return core::make_unexpected(std::string("create store parent dir: ") + made.error());
        }
    }

    std::shared_ptr<KvStore> store(new KvStore());
    store->log_path_ = path;
    store->durability_ = durability;

    // `Memory` skips the log entirely, so there is nothing to replay and a
    // stale file from a previous run must not resurrect itself.
    if (durability != Durability::Memory) {
        const core::Expected<Unit, std::string> replayed = store->Replay();
        if (!replayed.ok()) return core::make_unexpected(replayed.error());
    }
    return store;
}

core::Expected<Unit, std::string> KvStore::Replay() {
    if (!fs::Exists(log_path_)) return Unit();

    const core::Expected<std::string, std::string> raw = fs::ReadToString(log_path_);
    if (!raw.ok()) {
        return core::make_unexpected(std::string("open store log: ") + raw.error());
    }
    const std::string& log = raw.value();

    std::size_t cursor = 0;
    while (cursor < log.size()) {
        if (log.size() - cursor < kHeaderLen) {
            // A torn tail means the process died mid-append. Everything before
            // it is still valid, so the partial record is simply dropped.
            break;
        }
        const char op = log[cursor];
        std::size_t key_len = 0;
        std::size_t value_len = 0;
        if (!DecodeLength(log, cursor + 1, &key_len) ||
            !DecodeLength(log, cursor + 1 + kLenWidth, &value_len)) {
            return core::make_unexpected(std::string("store log is corrupt: bad record header"));
        }
        const std::size_t body = key_len + value_len;
        if (log.size() - cursor - kHeaderLen < body) break;  // torn tail

        const std::string key = log.substr(cursor + kHeaderLen, key_len);
        if (op == kOpPut) {
            entries_[key] = log.substr(cursor + kHeaderLen + key_len, value_len);
        } else if (op == kOpDelete) {
            entries_.erase(key);
        } else {
            return core::make_unexpected(std::string("store log is corrupt: unknown op"));
        }
        cursor += kHeaderLen + body;
    }
    return Unit();
}

core::Expected<Unit, std::string> KvStore::Commit(const std::vector<BatchOp>& ops) {
    if (ops.empty()) return Unit();

    if (durability_ != Durability::Memory) {
        // The whole batch is one append, so a crash cannot apply it partially
        // on replay — that is what gives the batch its atomicity.
        std::string record;
        for (std::size_t i = 0; i < ops.size(); ++i) record += EncodeRecord(ops[i]);

        const int fd = ::open(log_path_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (fd < 0) {
            std::ostringstream oss;
            oss << "open store log '" << log_path_ << "': " << std::strerror(errno);
            return core::make_unexpected(oss.str());
        }
        fs::FileDescriptor owned(fd);

        const core::Expected<Unit, std::string> written = fs::WriteFd(owned.get(), record);
        if (!written.ok()) {
            return core::make_unexpected(std::string("append store log: ") + written.error());
        }
        if (durability_ == Durability::Sync) {
            const core::Expected<Unit, std::string> synced = fs::SyncFd(owned.get());
            if (!synced.ok()) {
                return core::make_unexpected(std::string("sync store log: ") + synced.error());
            }
        }
    }

    // Applied only after the record is durable, so memory never claims more
    // than the log can recover.
    for (std::size_t i = 0; i < ops.size(); ++i) {
        if (ops[i].kind == BatchOp::Kind::Put) {
            entries_[ops[i].key] = ops[i].value;
        } else {
            entries_.erase(ops[i].key);
        }
    }
    return Unit();
}

core::Expected<Optional<std::string>, std::string> KvStore::Get(const std::string& key) const {
    std::lock_guard<std::mutex> guard(mutex_);
    const std::map<std::string, std::string>::const_iterator found = entries_.find(key);
    if (found == entries_.end()) return Optional<std::string>();
    return Optional<std::string>(found->second);
}

core::Expected<Unit, std::string> KvStore::Put(const std::string& key,
                                               const std::string& value) {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<BatchOp> ops;
    ops.push_back(BatchOp::Put(key, value));
    return Commit(ops);
}

core::Expected<Unit, std::string> KvStore::Delete(const std::string& key) {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<BatchOp> ops;
    ops.push_back(BatchOp::Delete(key));
    return Commit(ops);
}

core::Expected<Unit, std::string> KvStore::WriteBatch(const std::vector<BatchOp>& ops) {
    if (ops.empty()) return Unit();  // Rust returns early without touching the db
    std::lock_guard<std::mutex> guard(mutex_);
    return Commit(ops);
}

core::Expected<std::vector<Entry>, std::string> KvStore::Entries() const {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<Entry> out;
    out.reserve(entries_.size());
    for (std::map<std::string, std::string>::const_iterator it = entries_.begin();
         it != entries_.end(); ++it) {
        out.push_back(Entry(it->first, it->second));
    }
    return out;
}

core::Expected<Unit, std::string> KvStore::Fold(
    const std::function<std::string(const std::string&, const std::string&)>& visit) const {
    std::lock_guard<std::mutex> guard(mutex_);
    for (std::map<std::string, std::string>::const_iterator it = entries_.begin();
         it != entries_.end(); ++it) {
        const std::string error = visit(it->first, it->second);
        if (!error.empty()) return core::make_unexpected(error);
    }
    return Unit();
}

core::Expected<std::vector<Entry>, std::string> KvStore::ScanPrefix(
    const std::string& prefix) const {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<Entry> out;
    // lower_bound seeks straight to the prefix; the loop then stops at the
    // first non-matching key instead of walking the rest of the store.
    for (std::map<std::string, std::string>::const_iterator it = entries_.lower_bound(prefix);
         it != entries_.end(); ++it) {
        if (it->first.compare(0, prefix.size(), prefix) != 0) break;
        out.push_back(Entry(it->first, it->second));
    }
    return out;
}

std::string KvStore::ToDebugString() const {
    std::ostringstream oss;
    oss << "KvStore { durability: " << DurabilityToString(durability_) << ", .. }";
    return oss.str();
}

}  // namespace local_store
}  // namespace agentenv
