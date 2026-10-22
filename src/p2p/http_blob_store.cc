// SPDX-License-Identifier: MIT
// Rust: the parts of `iroh_blobs::store::fs::FsStore` the HTTP transport needs.
#include "agentenv/p2p/http_blob_store.h"

#include <cstdio>
#include <cstring>

#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "agentenv/core/json.h"

namespace agentenv {
namespace p2p {

namespace {

const char* const kRecordSuffix = ".json";

/// Keys come from callers and routinely contain `/` and `:` (they are artifact
/// keys, not file names). Hex-encoding them is not cosmetic: a key of `../x`
/// would otherwise name a path outside the store.
std::string HexEncode(const std::string& s) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(s.size() * 2);
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0x0f]);
    }
    return out;
}

P2pError FsError(const std::string& operation, const std::string& detail) {
    return P2pError::InternalMessage(operation, detail);
}

}  // namespace

std::string BlobFileNameForDigest(const std::string& digest) {
    std::string name = digest;
    for (std::size_t i = 0; i < name.size(); ++i) {
        if (name[i] == ':' || name[i] == '/') name[i] = '-';
    }
    return name;
}

// ---- BlobLease --------------------------------------------------------------

BlobLease::~BlobLease() {
    if (owner_) owner_->ReleaseLease(entry_.digest);
}

// ---- open -------------------------------------------------------------------

core::Expected<std::shared_ptr<HttpBlobStore>, P2pError>
HttpBlobStore::Open(const std::string& root_dir) {
    std::shared_ptr<HttpBlobStore> store(new HttpBlobStore(root_dir));

    const core::Expected<core::Unit, std::string> blobs =
        core::fs::CreateDirAll(store->BlobsDir());
    if (!blobs.ok()) {
        return core::make_unexpected(FsError("create P2P blob dir", blobs.error()));
    }
    const core::Expected<core::Unit, std::string> refs =
        core::fs::CreateDirAll(store->RefsDir());
    if (!refs.ok()) {
        return core::make_unexpected(FsError("create P2P publish record dir", refs.error()));
    }

    const core::Expected<core::Unit, P2pError> loaded = store->LoadRecords();
    if (!loaded.ok()) return core::make_unexpected(loaded.error());

    return store;
}

std::string HttpBlobStore::BlobsDir() const { return core::fs::Join(root_dir_, "blobs"); }
std::string HttpBlobStore::RefsDir() const { return core::fs::Join(root_dir_, "refs"); }

std::string HttpBlobStore::BlobPathFor(const std::string& digest) const {
    return core::fs::Join(BlobsDir(), BlobFileNameForDigest(digest));
}

std::string HttpBlobStore::RecordPathFor(const P2pArtifactKey& key) const {
    return core::fs::Join(RefsDir(), HexEncode(key) + kRecordSuffix);
}

// ---- records ----------------------------------------------------------------

core::Expected<core::Unit, P2pError>
HttpBlobStore::WriteRecord(const P2pArtifactKey& key, const BlobEntry& entry) {
    core::JsonObject o;
    o["key"] = core::Json(key);
    o["digest"] = core::Json(entry.digest);
    o["size"] = core::Json(static_cast<int64_t>(entry.size));
    o["path"] = core::Json(entry.path);
    o["referenced"] = core::Json(entry.referenced);

    const core::Expected<core::Unit, std::string> written =
        core::fs::Write(RecordPathFor(key), core::Json(o).ToString());
    if (!written.ok()) {
        return core::make_unexpected(FsError("write P2P publish record", written.error()));
    }
    return core::Unit{};
}

core::Expected<core::Unit, P2pError> HttpBlobStore::LoadRecords() {
    const core::Expected<std::vector<std::string>, std::string> names =
        core::fs::ReadDir(RefsDir());
    if (!names.ok()) {
        return core::make_unexpected(FsError("list P2P publish records", names.error()));
    }

    for (std::size_t i = 0; i < names.value().size(); ++i) {
        const std::string& name = names.value()[i];
        // The directory can hold a half-written `.tmp` from a crash; only
        // finished records are replayed.
        if (name.size() <= std::strlen(kRecordSuffix) ||
            name.compare(name.size() - std::strlen(kRecordSuffix),
                         std::strlen(kRecordSuffix), kRecordSuffix) != 0) {
            continue;
        }

        const std::string path = core::fs::Join(RefsDir(), name);
        const core::Expected<std::string, std::string> text = core::fs::ReadToString(path);
        if (!text.ok()) {
            return core::make_unexpected(
                P2pError::MakeInvalidCatalog(path, text.error()));
        }
        const core::Expected<core::Json, core::AnyError> parsed =
            core::Json::Parse(text.value());
        if (!parsed.ok()) {
            return core::make_unexpected(
                P2pError::MakeInvalidCatalog(path, "record is not valid JSON"));
        }
        const core::Json& json = parsed.value();
        if (json.kind() != core::Json::Kind::Object) {
            return core::make_unexpected(
                P2pError::MakeInvalidCatalog(path, "record is not a JSON object"));
        }

        const core::JsonObject& o = json.as_object();
        core::JsonObject::const_iterator k = o.find("key");
        core::JsonObject::const_iterator d = o.find("digest");
        core::JsonObject::const_iterator s = o.find("size");
        core::JsonObject::const_iterator p = o.find("path");
        core::JsonObject::const_iterator r = o.find("referenced");
        if (k == o.end() || d == o.end() || s == o.end() || p == o.end() ||
            k->second.kind() != core::Json::Kind::String ||
            d->second.kind() != core::Json::Kind::String ||
            s->second.kind() != core::Json::Kind::Int ||
            p->second.kind() != core::Json::Kind::String) {
            return core::make_unexpected(
                P2pError::MakeInvalidCatalog(path, "record is missing a required field"));
        }

        BlobEntry entry;
        entry.digest = d->second.as_string();
        entry.size = static_cast<uint64_t>(s->second.as_int());
        entry.path = p->second.as_string();
        entry.referenced =
            r != o.end() && r->second.kind() == core::Json::Kind::Bool && r->second.as_bool();

        // A publication whose bytes are gone is dropped rather than replayed.
        // Keeping it would have the catalog answer with a locator that 404s on
        // the very next request, which reads to a peer as a node that lies
        // about what it holds; forgetting it merely makes this node a
        // non-provider, which is both true and recoverable by republishing.
        if (!core::fs::Exists(entry.path)) {
            core::fs::RemoveFile(path);
            continue;
        }

        entries_[k->second.as_string()] = entry;
    }
    return core::Unit{};
}

// ---- import -----------------------------------------------------------------

core::Expected<BlobEntry, P2pError>
HttpBlobStore::Import(const P2pArtifactKey& key, const P2pPublishSource& source,
                      P2pPublishMode mode) {
    BlobEntry entry;

    if (source.kind == P2pPublishSource::Bytes) {
        const std::string bytes(source.bytes.begin(), source.bytes.end());
        entry.digest = core::Sha256Digest(bytes);
        entry.size = static_cast<uint64_t>(bytes.size());
        entry.path = BlobPathFor(entry.digest);
        entry.referenced = false;

        // Reference mode is meaningless for an in-memory source: there is no
        // foreign file to point at. Rust's `ImportMode::TryReference` has the
        // same property and falls back to a copy, so this does too rather
        // than rejecting a request that has an obvious correct reading.
        if (!core::fs::Exists(entry.path)) {
            const std::string tmp = entry.path + ".tmp";
            const core::Expected<core::Unit, std::string> written =
                core::fs::Write(tmp, bytes);
            if (!written.ok()) {
                return core::make_unexpected(FsError("write P2P blob", written.error()));
            }
            // The name is the digest, so a reader that sees the final name
            // sees complete, correct bytes. Writing in place would let a
            // concurrent reader open a prefix of them.
            if (std::rename(tmp.c_str(), entry.path.c_str()) != 0) {
                core::fs::RemoveFile(tmp);
                return core::make_unexpected(FsError("publish P2P blob", "rename failed"));
            }
        }
    } else {
        // Hashing and sizing come from one stream, so a file that changes
        // under us cannot yield a size and digest that never coexisted.
        const core::Expected<core::FileDigest, std::string> described =
            core::DescribeFile(source.path);
        if (!described.ok()) {
            return core::make_unexpected(
                P2pError::Invalid("describe P2P publish source: " + described.error()));
        }
        entry.digest = described.value().sha256;
        entry.size = described.value().size;

        if (mode == P2pPublishMode::Reference) {
            entry.path = source.path;
            entry.referenced = true;
        } else {
            entry.path = BlobPathFor(entry.digest);
            entry.referenced = false;
            if (!core::fs::Exists(entry.path)) {
                // Hardlink first: a content-addressed file is immutable, so
                // sharing the inode is free and safe. Copy is the fallback for
                // a source on another filesystem.
                const core::Expected<core::Unit, std::string> linked =
                    core::fs::HardLink(source.path, entry.path);
                if (!linked.ok()) {
                    const core::Expected<core::Unit, std::string> copied =
                        core::fs::Copy(source.path, entry.path);
                    if (!copied.ok()) {
                        return core::make_unexpected(
                            FsError("import P2P blob", copied.error()));
                    }
                }
            }
        }
    }

    const core::Expected<core::Unit, P2pError> recorded = WriteRecord(key, entry);
    if (!recorded.ok()) return core::make_unexpected(recorded.error());

    {
        std::lock_guard<std::mutex> g(mutex_);
        entries_[key] = entry;
    }
    return entry;
}

core::Expected<BlobEntry, P2pError>
HttpBlobStore::ImportFetched(const P2pArtifactKey& key, const std::string& path) {
    return Import(key, P2pPublishSource::FromPath(path), P2pPublishMode::Copy);
}

// ---- leases -----------------------------------------------------------------

std::shared_ptr<BlobLease> HttpBlobStore::MakeLease(const BlobEntry& entry) {
    ++leases_[entry.digest];
    return std::shared_ptr<BlobLease>(new BlobLease(shared_from_this(), entry));
}

std::shared_ptr<BlobLease> HttpBlobStore::Acquire(const std::string& locator) {
    std::lock_guard<std::mutex> g(mutex_);
    for (std::map<P2pArtifactKey, BlobEntry>::const_iterator it = entries_.begin();
         it != entries_.end(); ++it) {
        if (it->second.digest == locator) return MakeLease(it->second);
    }
    return std::shared_ptr<BlobLease>();
}

void HttpBlobStore::ReleaseLease(const std::string& digest) {
    std::lock_guard<std::mutex> g(mutex_);
    std::map<std::string, int>::iterator it = leases_.find(digest);
    if (it == leases_.end()) return;
    if (--it->second > 0) return;
    leases_.erase(it);

    // A removal that arrived mid-transfer deferred the unlink to whoever let
    // go last; that is now us.
    std::map<std::string, std::string>::iterator pending = pending_unlink_.find(digest);
    if (pending != pending_unlink_.end()) {
        core::fs::RemoveFile(pending->second);
        pending_unlink_.erase(pending);
    }
}

// ---- remove -----------------------------------------------------------------

bool HttpBlobStore::DigestStillReferencedLocked(const std::string& digest,
                                                const P2pArtifactKey& except) const {
    for (std::map<P2pArtifactKey, BlobEntry>::const_iterator it = entries_.begin();
         it != entries_.end(); ++it) {
        if (it->first != except && it->second.digest == digest) return true;
    }
    return false;
}

core::Expected<bool, P2pError> HttpBlobStore::Remove(const P2pArtifactKey& key) {
    BlobEntry entry;
    bool unlink_bytes = false;
    bool still_leased = false;

    {
        std::lock_guard<std::mutex> g(mutex_);
        std::map<P2pArtifactKey, BlobEntry>::iterator it = entries_.find(key);
        if (it == entries_.end()) return false;
        entry = it->second;

        // `Reference` bytes are the caller's file; this store never deletes
        // it. Deleting a path we were only lent would destroy data that is
        // very likely still in use for its original purpose.
        unlink_bytes = !entry.referenced && !DigestStillReferencedLocked(entry.digest, key);
        still_leased = leases_.find(entry.digest) != leases_.end();

        // Dropped before the bytes either way: the key must stop resolving
        // immediately, whether or not a transfer is still draining.
        entries_.erase(it);
        if (unlink_bytes && still_leased) pending_unlink_[entry.digest] = entry.path;
    }

    core::fs::RemoveFile(RecordPathFor(key));
    if (unlink_bytes && !still_leased) core::fs::RemoveFile(entry.path);
    return true;
}

core::Optional<std::string>
HttpBlobStore::LocatorFor(const P2pArtifactKey& key) const {
    std::lock_guard<std::mutex> g(mutex_);
    std::map<P2pArtifactKey, BlobEntry>::const_iterator it = entries_.find(key);
    if (it == entries_.end()) return core::Optional<std::string>();
    return it->second.digest;
}

std::size_t HttpBlobStore::size() const {
    std::lock_guard<std::mutex> g(mutex_);
    return entries_.size();
}

}  // namespace p2p
}  // namespace agentenv
