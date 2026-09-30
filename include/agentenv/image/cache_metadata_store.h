// SPDX-License-Identifier: MIT
// Rust: src/image/cache/graph.rs — `ImageCacheMetadataStore`.
//
// The durable half of the image-cache reference graph. `cache.h` holds the
// pure logic (identity newtypes, key encoding, the eviction planner); this
// header adds the KV-backed store that persists the graph and answers the
// reference questions GC asks before deleting a commit.
//
// Reference model. Two kinds of edge root a hard commit:
//   * config → hard  : a source image config on disk references the commit.
//   * hold   → hard  : a named owner (namespace/key) pins it explicitly.
// Hold edges are stored twice — `hold-to-hard` and `hard-to-hold` — so both
// "what does this owner pin" and "who pins this commit" are prefix scans
// rather than full-store walks. Every mutation that touches a hold edge must
// therefore write or delete both directions; the helpers below do that in one
// batch so a crash cannot leave the mirror half-written.
//
// Porting note. Rust is async purely to keep RocksDB off the reactor; without
// one the same operations are plain synchronous calls. The `object_update_lock`
// that serialises read-modify-write on object records becomes a plain mutex.
#ifndef AGENTENV_IMAGE_CACHE_METADATA_STORE_H_
#define AGENTENV_IMAGE_CACHE_METADATA_STORE_H_

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/json.h"
#include "agentenv/core/optional.h"
#include "agentenv/image/cache.h"
#include "agentenv/local_store.h"
#include "agentenv/p2p/types.h"

namespace agentenv {
namespace image {
namespace cache {

/// Rust `SCHEMA_VERSION`.
extern const int64_t kSchemaVersion;
/// Rust `SCHEMA_VERSION_KEY`.
extern const char* const kSchemaVersionKey;

/// Rust `unix_now_secs`.
uint64_t UnixNowSecs();

/// Rust `struct HardCommitObjectRecord`.
struct HardCommitObjectRecord {
    HardCommitId                     digest;
    /// Rust `Option<PathBuf>` — absent when only the digest is known.
    core::Optional<std::string>      file;
    core::Optional<uint64_t>         size;
    /// Rust `BTreeSet<P2pArtifactKey>` — ordered, so the encoding is stable.
    std::set<p2p::P2pArtifactKey>    p2p_keys;

    explicit HardCommitObjectRecord(HardCommitId d) : digest(std::move(d)) {}

    core::Json ToJson() const;
    static core::Expected<HardCommitObjectRecord, std::string>
        FromJson(const core::Json& json);
};

/// Rust `struct ParsedHardCommitRef` — one `file=` lower of a cache-owned
/// image config, already validated to carry a digest and a size.
struct ParsedHardCommitRef {
    HardCommitId digest;
    std::string  file;
    uint64_t     size = 0;

    ParsedHardCommitRef(HardCommitId d, std::string f, uint64_t s)
        : digest(std::move(d)), file(std::move(f)), size(s) {}
};

/// Rust `load_cache_owned_hard_commit_refs`.
///
/// In a cache-owned config, `file=` lowers are hard commits. `dir=` lowers are
/// remote-recoverable and deliberately not pinned, so they are skipped.
core::Expected<std::vector<ParsedHardCommitRef>, std::string>
    LoadCacheOwnedHardCommitRefs(const std::string& image_config_path);

/// Rust `load_commit_store_owned_hard_commit_refs` — the subset of `file=`
/// lowers that live inside `commit_store`. Fails closed when such a lower is
/// missing its digest or size.
core::Expected<std::vector<ParsedHardCommitRef>, std::string>
    LoadCommitStoreOwnedHardCommitRefs(const std::string& image_config_path,
                                       const std::string& commit_store);

/// Rust `path_is_inside`.
bool PathIsInside(const std::string& path, const std::string& ancestor);

/// Rust `struct ImageCacheMetadataStore`.
class ImageCacheMetadataStore {
 public:
    /// Rust `open` — opens the KV store and reconciles the schema version,
    /// wiping the store when it is absent, unreadable or from another version.
    static core::Expected<std::shared_ptr<ImageCacheMetadataStore>, std::string>
        Open(const std::string& path, local_store::Durability durability);

    // ---- hard commit objects ------------------------------------------------

    /// Rust `record_hard_commit_object` — merges into the existing record's
    /// P2P key set rather than replacing it.
    core::Expected<core::Unit, std::string>
        RecordHardCommitObject(const HardCommitId& digest,
                               const core::Optional<std::string>& file,
                               const core::Optional<uint64_t>& size,
                               const std::set<p2p::P2pArtifactKey>& p2p_keys);

    /// Rust `get_hard_commit_object`.
    core::Expected<core::Optional<HardCommitObjectRecord>, std::string>
        GetHardCommitObject(const HardCommitId& digest) const;

    /// Rust `list_hard_commit_objects` — digest-ordered.
    core::Expected<std::vector<HardCommitObjectRecord>, std::string>
        ListHardCommitObjects() const;

    /// Rust `remove_hard_commit_object`.
    core::Expected<core::Unit, std::string>
        RemoveHardCommitObject(const HardCommitId& digest);

    /// Rust `add_hard_commit_p2p_key` — refuses to record a key for a commit
    /// that has no object record, and is a no-op when the key is already set.
    core::Expected<core::Unit, std::string>
        AddHardCommitP2pKey(const HardCommitId& digest, const p2p::P2pArtifactKey& key);

    // ---- config references --------------------------------------------------

    /// Rust `record_config_refs_from_config_path`.
    ///
    /// Replaces the config's edge set from the config file on disk and, since
    /// this runs on every resolve, doubles as the LRU touch.
    core::Expected<core::Unit, std::string>
        RecordConfigRefsFromConfigPath(const std::string& config_path);

    /// Rust `commit_store_hard_commit_digests_from_config_path`.
    static core::Expected<std::vector<HardCommitId>, std::string>
        CommitStoreHardCommitDigestsFromConfigPath(const std::string& config_path,
                                                   const std::string& commit_store);

    /// Rust `remove_config_refs`.
    core::Expected<core::Unit, std::string>
        RemoveConfigRefs(const ImageCacheConfigId& config_id);

    /// Rust `hard_commit_config_referrers`.
    core::Expected<std::vector<ImageCacheConfigId>, std::string>
        HardCommitConfigReferrers(const HardCommitId& digest) const;

    /// Rust `hard_commit_config_referrer_map`.
    core::Expected<std::map<HardCommitId, std::vector<ImageCacheConfigId> >, std::string>
        HardCommitConfigReferrerMap() const;

    /// Rust `rebuild_from_configs` — reconciles the whole graph against a
    /// config directory, preserving recency for configs that already had it.
    core::Expected<core::Unit, std::string>
        RebuildFromConfigs(const std::string& configs_dir);

    // ---- holds --------------------------------------------------------------

    /// Rust `create_or_replace_hold`.
    core::Expected<core::Unit, std::string>
        CreateOrReplaceHold(const ImageCacheHoldOwner& owner,
                            const std::set<HardCommitId>& refs);

    /// Rust `release_hold`.
    core::Expected<core::Unit, std::string> ReleaseHold(const ImageCacheHoldOwner& owner);

    /// Rust `list_hold_owners_in_namespaces`.
    core::Expected<std::vector<ImageCacheHoldOwner>, std::string>
        ListHoldOwnersInNamespaces(const std::vector<std::string>& namespaces) const;

    /// Rust `release_holds_in_namespaces` — startup cleanup for transient
    /// namespaces. Never pass a durable namespace.
    core::Expected<std::vector<ImageCacheHoldOwner>, std::string>
        ReleaseHoldsInNamespaces(const std::vector<std::string>& namespaces);

    /// Rust `hard_commit_hold_referrers`.
    core::Expected<std::vector<ImageCacheHoldOwner>, std::string>
        HardCommitHoldReferrers(const HardCommitId& digest) const;

    // ---- recency / capacity -------------------------------------------------

    /// Rust `config_last_used`.
    core::Expected<core::Optional<uint64_t>, std::string>
        ConfigLastUsed(const ImageCacheConfigId& config_id) const;

    /// Rust `plan_capacity_eviction` — scans the store, then delegates to the
    /// store-free planner in `cache.h`.
    core::Expected<CapacityEvictionPlan, std::string>
        PlanCapacityEvictionFromStore(uint64_t high_watermark_bytes,
                                      uint64_t low_watermark_bytes,
                                      uint64_t evictable_before) const;

 private:
    explicit ImageCacheMetadataStore(std::shared_ptr<local_store::KvStore> store)
        : store_(std::move(store)) {}

    /// Rust `ensure_schema_version` / `write_schema_version` /
    /// `reset_schema_version`.
    core::Expected<core::Unit, std::string> EnsureSchemaVersion();
    core::Expected<core::Unit, std::string> ResetSchemaVersion(const std::string& reason);

    /// Rust `config_refs_set`.
    core::Expected<std::set<HardCommitId>, std::string>
        ConfigRefsSet(const ImageCacheConfigId& config_id) const;
    /// Rust `config_ref_map`.
    core::Expected<std::map<ImageCacheConfigId, std::set<HardCommitId> >, std::string>
        ConfigRefMap() const;
    /// Rust `config_last_used_map`.
    core::Expected<std::map<ImageCacheConfigId, uint64_t>, std::string>
        ConfigLastUsedMap() const;
    /// Rust `hold_refs_set`.
    core::Expected<std::set<HardCommitId>, std::string>
        HoldRefsSet(const ImageCacheHoldOwner& owner) const;

    std::shared_ptr<local_store::KvStore> store_;
    /// Rust `object_update_lock` — serialises read-modify-write sequences on
    /// object records and config edge sets.
    mutable std::mutex                   object_update_lock_;
};

}  // namespace cache
}  // namespace image
}  // namespace agentenv
#endif  // AGENTENV_IMAGE_CACHE_METADATA_STORE_H_
