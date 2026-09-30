// SPDX-License-Identifier: MIT
// Rust: src/image/cache/graph.rs — `ImageCacheMetadataStore`.
#include "agentenv/image/cache_metadata_store.h"

#include <algorithm>
#include <ctime>
#include <sstream>

#include "agentenv/core/fs.h"
#include "agentenv/core/json.h"
#include "agentenv/storage/overlaybd/config.h"

namespace agentenv {
namespace image {
namespace cache {

const int64_t     kSchemaVersion   = 1;
const char* const kSchemaVersionKey = "schema/version";

uint64_t UnixNowSecs() {
    return static_cast<uint64_t>(std::time(NULL));
}

// ---- record encodings -------------------------------------------------------

core::Json HardCommitObjectRecord::ToJson() const {
    core::JsonObject object;
    object["digest"] = core::Json(digest.AsStr());
    // Rust marks file/size `skip_serializing_if = "Option::is_none"`.
    if (file.has_value()) object["file"] = core::Json(*file);
    if (size.has_value()) object["size"] = core::Json(static_cast<int64_t>(*size));
    if (!p2p_keys.empty()) {
        core::JsonArray keys;
        for (std::set<p2p::P2pArtifactKey>::const_iterator it = p2p_keys.begin();
             it != p2p_keys.end(); ++it) {
            keys.push_back(core::Json(*it));
        }
        object["p2p_keys"] = core::Json(keys);
    }
    return core::Json(object);
}

core::Expected<HardCommitObjectRecord, std::string>
HardCommitObjectRecord::FromJson(const core::Json& json) {
    if (json.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("hard commit object must be an object"));
    }
    const core::JsonObject& fields = json.as_object();

    core::JsonObject::const_iterator it = fields.find("digest");
    if (it == fields.end() || it->second.kind() != core::Json::Kind::String) {
        return core::make_unexpected(std::string("missing field `digest`"));
    }
    core::Expected<HardCommitId, std::string> digest = HardCommitId::New(it->second.as_string());
    if (!digest.ok()) return core::make_unexpected(digest.error());

    HardCommitObjectRecord record(digest.value());

    it = fields.find("file");
    if (it != fields.end() && it->second.kind() == core::Json::Kind::String) {
        record.file = core::Optional<std::string>(it->second.as_string());
    }

    it = fields.find("size");
    if (it != fields.end() && it->second.kind() == core::Json::Kind::Int) {
        record.size = core::Optional<uint64_t>(static_cast<uint64_t>(it->second.as_int()));
    }

    it = fields.find("p2p_keys");
    if (it != fields.end() && it->second.kind() == core::Json::Kind::Array) {
        const core::JsonArray& keys = it->second.as_array();
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (keys[i].kind() != core::Json::Kind::String) {
                return core::make_unexpected(std::string("field `p2p_keys` must hold strings"));
            }
            record.p2p_keys.insert(keys[i].as_string());
        }
    }
    return record;
}

namespace {

/// Rust `ConfigReferenceRecord`.
core::Json ConfigReferenceJson(const ImageCacheConfigId& config_id,
                               const HardCommitId& digest) {
    core::JsonObject object;
    object["config_id"] = core::Json(config_id.AsStr());
    object["digest"]    = core::Json(digest.AsStr());
    return core::Json(object);
}

/// Rust `ConfigLastUsedRecord`.
core::Json ConfigLastUsedJson(const ImageCacheConfigId& config_id, uint64_t last_used) {
    core::JsonObject object;
    object["config_id"] = core::Json(config_id.AsStr());
    object["last_used"] = core::Json(static_cast<int64_t>(last_used));
    return core::Json(object);
}

/// Rust `HoldRecord`.
core::Json HoldRecordJson(const ImageCacheHoldOwner& owner) {
    core::JsonObject object;
    object["namespace"] = core::Json(owner.Namespace());
    object["key"]       = core::Json(owner.Key());
    return core::Json(object);
}

/// Rust `HoldHardCommitReferenceRecord`.
core::Json HoldHardReferenceJson(const ImageCacheHoldOwner& owner,
                                 const HardCommitId& digest) {
    core::JsonObject object;
    object["namespace"] = core::Json(owner.Namespace());
    object["key"]       = core::Json(owner.Key());
    object["digest"]    = core::Json(digest.AsStr());
    return core::Json(object);
}

std::string StringField(const core::JsonObject& fields, const char* name) {
    core::JsonObject::const_iterator it = fields.find(name);
    if (it == fields.end() || it->second.kind() != core::Json::Kind::String) {
        return std::string();
    }
    return it->second.as_string();
}

/// Rust `decode_json` — the key is included so a corrupt entry is traceable.
core::Expected<core::Json, std::string> DecodeJson(const std::string& key,
                                                   const std::string& value,
                                                   const char* record_type) {
    core::Expected<core::Json, core::AnyError> json = core::Json::Parse(value);
    if (!json.ok()) {
        std::ostringstream os;
        os << "decode " << record_type << " at key '" << key << "': "
           << json.error().chain();
        return core::make_unexpected(os.str());
    }
    return json.value();
}

/// Rust `push_mirrored_delete`.
void PushMirroredDelete(std::vector<local_store::BatchOp>* ops,
                        const std::string& forward, const std::string& reverse) {
    ops->push_back(local_store::BatchOp::Delete(forward));
    ops->push_back(local_store::BatchOp::Delete(reverse));
}

/// Rust `push_mirrored_put_json` — both directions carry the same bytes.
void PushMirroredPutJson(std::vector<local_store::BatchOp>* ops,
                         const std::string& forward, const std::string& reverse,
                         const core::Json& record) {
    const std::string value = record.ToString();
    ops->push_back(local_store::BatchOp::Put(forward, value));
    ops->push_back(local_store::BatchOp::Put(reverse, value));
}

/// Rust `config_ref_replacement_ops` — delete what is gone, add what is new,
/// leave untouched edges alone so the batch stays minimal.
std::vector<local_store::BatchOp> ConfigRefReplacementOps(
    const ImageCacheConfigId& config_id,
    const std::set<HardCommitId>& existing,
    const std::set<HardCommitId>& refs) {
    std::vector<local_store::BatchOp> ops;
    for (std::set<HardCommitId>::const_iterator it = existing.begin();
         it != existing.end(); ++it) {
        if (refs.find(*it) == refs.end()) {
            ops.push_back(local_store::BatchOp::Delete(ConfigToHardKey(config_id, *it)));
        }
    }
    for (std::set<HardCommitId>::const_iterator it = refs.begin(); it != refs.end(); ++it) {
        if (existing.find(*it) == existing.end()) {
            ops.push_back(local_store::BatchOp::Put(
                ConfigToHardKey(config_id, *it),
                ConfigReferenceJson(config_id, *it).ToString()));
        }
    }
    return ops;
}

/// Rust `config_ref_removal_ops` — drops the recency entry too.
std::vector<local_store::BatchOp> ConfigRefRemovalOps(
    const ImageCacheConfigId& config_id, const std::set<HardCommitId>& existing) {
    std::vector<local_store::BatchOp> ops;
    for (std::set<HardCommitId>::const_iterator it = existing.begin();
         it != existing.end(); ++it) {
        ops.push_back(local_store::BatchOp::Delete(ConfigToHardKey(config_id, *it)));
    }
    ops.push_back(local_store::BatchOp::Delete(ConfigLastUsedKey(config_id)));
    return ops;
}

/// Rust `hold_ref_replacement_ops`.
std::vector<local_store::BatchOp> HoldRefReplacementOps(
    const ImageCacheHoldOwner& owner,
    const std::set<HardCommitId>& existing,
    const std::set<HardCommitId>& refs) {
    std::vector<local_store::BatchOp> ops;
    for (std::set<HardCommitId>::const_iterator it = existing.begin();
         it != existing.end(); ++it) {
        if (refs.find(*it) == refs.end()) {
            PushMirroredDelete(&ops, HoldToHardKey(owner, *it), HardToHoldKey(*it, owner));
        }
    }
    for (std::set<HardCommitId>::const_iterator it = refs.begin(); it != refs.end(); ++it) {
        if (existing.find(*it) == existing.end()) {
            PushMirroredPutJson(&ops, HoldToHardKey(owner, *it), HardToHoldKey(*it, owner),
                                HoldHardReferenceJson(owner, *it));
        }
    }
    return ops;
}

/// Rust `hold_ref_removal_ops`.
std::vector<local_store::BatchOp> HoldRefRemovalOps(
    const ImageCacheHoldOwner& owner, const std::set<HardCommitId>& existing) {
    std::vector<local_store::BatchOp> ops;
    for (std::set<HardCommitId>::const_iterator it = existing.begin();
         it != existing.end(); ++it) {
        PushMirroredDelete(&ops, HoldToHardKey(owner, *it), HardToHoldKey(*it, owner));
    }
    ops.push_back(local_store::BatchOp::Delete(HoldRecordKey(owner)));
    return ops;
}

/// Rust `hard_commit_object_put_op`.
local_store::BatchOp HardCommitObjectPutOp(const HardCommitId& digest,
                                           const core::Optional<std::string>& file,
                                           const core::Optional<uint64_t>& size,
                                           const std::set<p2p::P2pArtifactKey>& p2p_keys) {
    HardCommitObjectRecord record(digest);
    record.file     = file;
    record.size     = size;
    record.p2p_keys = p2p_keys;
    return local_store::BatchOp::Put(HardCommitObjectKey(digest), record.ToJson().ToString());
}

/// Rust `schema_version_put_op`.
local_store::BatchOp SchemaVersionPutOp() {
    return local_store::BatchOp::Put(kSchemaVersionKey,
                                     core::Json(kSchemaVersion).ToString());
}

}  // namespace

// ---- config parsing ---------------------------------------------------------

bool PathIsInside(const std::string& path, const std::string& ancestor) {
    if (ancestor.empty()) return false;
    // Compare on a trailing-slash-normalised ancestor so "/a/bc" is not
    // treated as living inside "/a/b".
    std::string prefix = ancestor;
    if (prefix[prefix.size() - 1] != '/') prefix += '/';
    return path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0;
}

namespace {

core::Expected<storage::overlaybd::ImageConfig, std::string> LoadImageConfig(
    const std::string& image_config_path) {
    core::Expected<std::string, std::string> text =
        core::fs::ReadToString(image_config_path);
    if (!text.ok()) {
        return core::make_unexpected(std::string("load image config ") + image_config_path +
                                     ": " + text.error());
    }
    core::Expected<storage::overlaybd::ImageConfig, std::string> config =
        storage::overlaybd::ParseImageConfig(text.value());
    if (!config.ok()) {
        return core::make_unexpected(std::string("load image config ") + image_config_path +
                                     ": " + config.error());
    }
    return config.value();
}

}  // namespace

core::Expected<std::vector<ParsedHardCommitRef>, std::string>
LoadCacheOwnedHardCommitRefs(const std::string& image_config_path) {
    core::Expected<storage::overlaybd::ImageConfig, std::string> config =
        LoadImageConfig(image_config_path);
    if (!config.ok()) return core::make_unexpected(config.error());

    std::vector<ParsedHardCommitRef> refs;
    std::set<HardCommitId>           seen;
    const std::vector<storage::overlaybd::LayerConfig>& lowers = config.value().lowers;
    for (std::size_t i = 0; i < lowers.size(); ++i) {
        // `dir=` lowers are remote-recoverable, so they are never pinned.
        if (lowers[i].file.empty()) continue;
        core::Expected<HardCommitId, std::string> digest =
            HardCommitId::New(lowers[i].digest);
        if (!digest.ok()) return core::make_unexpected(digest.error());
        if (seen.insert(digest.value()).second) {
            refs.push_back(
                ParsedHardCommitRef(digest.value(), lowers[i].file, lowers[i].size));
        }
    }
    return refs;
}

core::Expected<std::vector<ParsedHardCommitRef>, std::string>
LoadCommitStoreOwnedHardCommitRefs(const std::string& image_config_path,
                                   const std::string& commit_store) {
    core::Expected<storage::overlaybd::ImageConfig, std::string> config =
        LoadImageConfig(image_config_path);
    if (!config.ok()) return core::make_unexpected(config.error());

    std::vector<ParsedHardCommitRef> refs;
    std::set<HardCommitId>           seen;
    const std::vector<storage::overlaybd::LayerConfig>& lowers = config.value().lowers;
    for (std::size_t i = 0; i < lowers.size(); ++i) {
        if (lowers[i].file.empty()) continue;
        if (!PathIsInside(lowers[i].file, commit_store)) continue;

        // Rust fails closed here: a commit-store file without identity would
        // silently escape reference tracking and then be GC'd out from under
        // a live image.
        std::ostringstream where;
        where << "image config " << image_config_path << " lower " << i;
        if (lowers[i].digest.empty()) {
            return core::make_unexpected(
                where.str() + " has image-cache commit-store file but no digest");
        }
        if (lowers[i].size == 0) {
            return core::make_unexpected(
                where.str() + " has image-cache commit-store file but no size");
        }

        core::Expected<HardCommitId, std::string> digest =
            HardCommitId::New(lowers[i].digest);
        if (!digest.ok()) return core::make_unexpected(digest.error());
        if (seen.insert(digest.value()).second) {
            refs.push_back(
                ParsedHardCommitRef(digest.value(), lowers[i].file, lowers[i].size));
        }
    }
    return refs;
}

// ---- open / schema ----------------------------------------------------------

core::Expected<std::shared_ptr<ImageCacheMetadataStore>, std::string>
ImageCacheMetadataStore::Open(const std::string& path,
                              local_store::Durability durability) {
    core::Expected<std::shared_ptr<local_store::KvStore>, std::string> store =
        local_store::KvStore::Open(path, durability);
    if (!store.ok()) return core::make_unexpected(store.error());

    std::shared_ptr<ImageCacheMetadataStore> metadata(
        new ImageCacheMetadataStore(store.value()));
    core::Expected<core::Unit, std::string> schema = metadata->EnsureSchemaVersion();
    if (!schema.ok()) return core::make_unexpected(schema.error());
    return metadata;
}

core::Expected<core::Unit, std::string> ImageCacheMetadataStore::EnsureSchemaVersion() {
    core::Expected<core::Optional<std::string>, std::string> stored =
        store_->Get(kSchemaVersionKey);
    if (!stored.ok()) return core::make_unexpected(stored.error());

    if (!stored.value().has_value()) {
        core::Expected<core::Unit, std::string> written =
            store_->Put(kSchemaVersionKey, core::Json(kSchemaVersion).ToString());
        if (!written.ok()) {
            return core::make_unexpected(std::string("write image cache metadata schema "
                                                     "version: ") + written.error());
        }
        return core::Unit{};
    }

    core::Expected<core::Json, core::AnyError> json = core::Json::Parse(*stored.value());
    if (!json.ok() || json.value().kind() != core::Json::Kind::Int) {
        return ResetSchemaVersion("invalid image cache metadata schema version");
    }
    if (json.value().as_int() != kSchemaVersion) {
        std::ostringstream os;
        os << "unsupported image cache metadata schema version " << json.value().as_int()
           << "; expected " << kSchemaVersion;
        return ResetSchemaVersion(os.str());
    }
    return core::Unit{};
}

core::Expected<core::Unit, std::string>
ImageCacheMetadataStore::ResetSchemaVersion(const std::string& reason) {
    core::Expected<std::vector<local_store::Entry>, std::string> entries =
        store_->Entries();
    if (!entries.ok()) {
        return core::make_unexpected(
            std::string("scan image cache metadata before reset: ") + reason);
    }
    std::vector<local_store::BatchOp> ops;
    const std::vector<local_store::Entry>& all = entries.value();
    for (std::size_t i = 0; i < all.size(); ++i) {
        ops.push_back(local_store::BatchOp::Delete(all[i].first));
    }
    ops.push_back(SchemaVersionPutOp());

    core::Expected<core::Unit, std::string> written = store_->WriteBatch(ops);
    if (!written.ok()) {
        return core::make_unexpected(std::string("reset image cache metadata store: ") +
                                     reason + ": " + written.error());
    }
    return core::Unit{};
}

// ---- hard commit objects ----------------------------------------------------

core::Expected<core::Optional<HardCommitObjectRecord>, std::string>
ImageCacheMetadataStore::GetHardCommitObject(const HardCommitId& digest) const {
    const std::string key = HardCommitObjectKey(digest);
    core::Expected<core::Optional<std::string>, std::string> value = store_->Get(key);
    if (!value.ok()) return core::make_unexpected(value.error());
    if (!value.value().has_value()) {
        return core::Optional<HardCommitObjectRecord>();
    }
    core::Expected<core::Json, std::string> json =
        DecodeJson(key, *value.value(), "hard commit object record");
    if (!json.ok()) return core::make_unexpected(json.error());
    core::Expected<HardCommitObjectRecord, std::string> record =
        HardCommitObjectRecord::FromJson(json.value());
    if (!record.ok()) return core::make_unexpected(record.error());
    return core::Optional<HardCommitObjectRecord>(record.value());
}

core::Expected<core::Unit, std::string>
ImageCacheMetadataStore::RecordHardCommitObject(
    const HardCommitId& digest, const core::Optional<std::string>& file,
    const core::Optional<uint64_t>& size,
    const std::set<p2p::P2pArtifactKey>& p2p_keys) {
    std::lock_guard<std::mutex> guard(object_update_lock_);

    // Rust extends rather than replaces: a re-record must not lose the P2P
    // keys an earlier publish attached.
    std::set<p2p::P2pArtifactKey> merged = p2p_keys;
    core::Expected<core::Optional<HardCommitObjectRecord>, std::string> existing =
        GetHardCommitObject(digest);
    if (!existing.ok()) return core::make_unexpected(existing.error());
    if (existing.value().has_value()) {
        const std::set<p2p::P2pArtifactKey>& previous = existing.value()->p2p_keys;
        merged.insert(previous.begin(), previous.end());
    }

    HardCommitObjectRecord record(digest);
    record.file     = file;
    record.size     = size;
    record.p2p_keys = merged;

    core::Expected<core::Unit, std::string> written =
        store_->Put(HardCommitObjectKey(digest), record.ToJson().ToString());
    if (!written.ok()) {
        return core::make_unexpected(std::string("record hard commit object ") +
                                     digest.AsStr() + ": " + written.error());
    }
    return core::Unit{};
}

core::Expected<std::vector<HardCommitObjectRecord>, std::string>
ImageCacheMetadataStore::ListHardCommitObjects() const {
    core::Expected<std::vector<local_store::Entry>, std::string> entries =
        store_->ScanPrefix(kHardCommitObjectPrefix);
    if (!entries.ok()) return core::make_unexpected(entries.error());

    std::vector<HardCommitObjectRecord> records;
    const std::vector<local_store::Entry>& all = entries.value();
    for (std::size_t i = 0; i < all.size(); ++i) {
        core::Expected<core::Json, std::string> json =
            DecodeJson(all[i].first, all[i].second, "hard commit object record");
        if (!json.ok()) return core::make_unexpected(json.error());
        core::Expected<HardCommitObjectRecord, std::string> record =
            HardCommitObjectRecord::FromJson(json.value());
        if (!record.ok()) return core::make_unexpected(record.error());
        records.push_back(record.value());
    }
    // The keys are hex-encoded, so scan order is not digest order.
    std::sort(records.begin(), records.end(),
              [](const HardCommitObjectRecord& l, const HardCommitObjectRecord& r) {
                  return l.digest < r.digest;
              });
    return records;
}

core::Expected<core::Unit, std::string>
ImageCacheMetadataStore::RemoveHardCommitObject(const HardCommitId& digest) {
    core::Expected<core::Unit, std::string> removed =
        store_->Delete(HardCommitObjectKey(digest));
    if (!removed.ok()) {
        return core::make_unexpected(std::string("remove hard commit object ") +
                                     digest.AsStr() + ": " + removed.error());
    }
    return core::Unit{};
}

core::Expected<core::Unit, std::string>
ImageCacheMetadataStore::AddHardCommitP2pKey(const HardCommitId& digest,
                                             const p2p::P2pArtifactKey& key) {
    std::lock_guard<std::mutex> guard(object_update_lock_);

    core::Expected<core::Optional<HardCommitObjectRecord>, std::string> existing =
        GetHardCommitObject(digest);
    if (!existing.ok()) return core::make_unexpected(existing.error());
    if (!existing.value().has_value()) {
        return core::make_unexpected(std::string("hard commit object ") + digest.AsStr() +
                                     " does not exist while recording P2P key '" + key + "'");
    }

    HardCommitObjectRecord record = *existing.value();
    if (!record.p2p_keys.insert(key).second) {
        // Rust returns early when the key is already present.
        return core::Unit{};
    }
    core::Expected<core::Unit, std::string> written =
        store_->Put(HardCommitObjectKey(digest), record.ToJson().ToString());
    if (!written.ok()) {
        return core::make_unexpected(std::string("record P2P key for hard commit ") +
                                     digest.AsStr() + ": " + written.error());
    }
    return core::Unit{};
}

// ---- config references ------------------------------------------------------

core::Expected<std::set<HardCommitId>, std::string>
ImageCacheMetadataStore::ConfigRefsSet(const ImageCacheConfigId& config_id) const {
    core::Expected<std::vector<local_store::Entry>, std::string> entries =
        store_->ScanPrefix(ConfigToHardPrefixForConfig(config_id));
    if (!entries.ok()) return core::make_unexpected(entries.error());

    std::set<HardCommitId> refs;
    const std::vector<local_store::Entry>& all = entries.value();
    for (std::size_t i = 0; i < all.size(); ++i) {
        core::Expected<core::Json, std::string> json =
            DecodeJson(all[i].first, all[i].second, "config-to-hard reference");
        if (!json.ok()) return core::make_unexpected(json.error());
        if (json.value().kind() != core::Json::Kind::Object) continue;
        core::Expected<HardCommitId, std::string> digest =
            HardCommitId::New(StringField(json.value().as_object(), "digest"));
        if (!digest.ok()) return core::make_unexpected(digest.error());
        refs.insert(digest.value());
    }
    return refs;
}

core::Expected<std::map<ImageCacheConfigId, std::set<HardCommitId> >, std::string>
ImageCacheMetadataStore::ConfigRefMap() const {
    core::Expected<std::vector<local_store::Entry>, std::string> entries =
        store_->ScanPrefix(kConfigToHardPrefix);
    if (!entries.ok()) return core::make_unexpected(entries.error());

    std::map<ImageCacheConfigId, std::set<HardCommitId> > refs;
    const std::vector<local_store::Entry>& all = entries.value();
    for (std::size_t i = 0; i < all.size(); ++i) {
        core::Expected<core::Json, std::string> json =
            DecodeJson(all[i].first, all[i].second, "config-to-hard reference");
        if (!json.ok()) return core::make_unexpected(json.error());
        if (json.value().kind() != core::Json::Kind::Object) continue;
        const core::JsonObject& fields = json.value().as_object();

        core::Expected<ImageCacheConfigId, std::string> config_id =
            ImageCacheConfigId::FromFilename(StringField(fields, "config_id"));
        if (!config_id.ok()) return core::make_unexpected(config_id.error());
        core::Expected<HardCommitId, std::string> digest =
            HardCommitId::New(StringField(fields, "digest"));
        if (!digest.ok()) return core::make_unexpected(digest.error());

        refs[config_id.value()].insert(digest.value());
    }
    return refs;
}

core::Expected<std::map<ImageCacheConfigId, uint64_t>, std::string>
ImageCacheMetadataStore::ConfigLastUsedMap() const {
    core::Expected<std::vector<local_store::Entry>, std::string> entries =
        store_->ScanPrefix(kConfigLastUsedPrefix);
    if (!entries.ok()) return core::make_unexpected(entries.error());

    std::map<ImageCacheConfigId, uint64_t> last_used;
    const std::vector<local_store::Entry>& all = entries.value();
    for (std::size_t i = 0; i < all.size(); ++i) {
        core::Expected<core::Json, std::string> json =
            DecodeJson(all[i].first, all[i].second, "config last-used record");
        if (!json.ok()) return core::make_unexpected(json.error());
        if (json.value().kind() != core::Json::Kind::Object) continue;
        const core::JsonObject& fields = json.value().as_object();

        core::Expected<ImageCacheConfigId, std::string> config_id =
            ImageCacheConfigId::FromFilename(StringField(fields, "config_id"));
        if (!config_id.ok()) return core::make_unexpected(config_id.error());

        core::JsonObject::const_iterator used = fields.find("last_used");
        if (used == fields.end() || used->second.kind() != core::Json::Kind::Int) continue;
        last_used[config_id.value()] = static_cast<uint64_t>(used->second.as_int());
    }
    return last_used;
}

core::Expected<core::Optional<uint64_t>, std::string>
ImageCacheMetadataStore::ConfigLastUsed(const ImageCacheConfigId& config_id) const {
    const std::string key = ConfigLastUsedKey(config_id);
    core::Expected<core::Optional<std::string>, std::string> value = store_->Get(key);
    if (!value.ok()) return core::make_unexpected(value.error());
    if (!value.value().has_value()) return core::Optional<uint64_t>();

    core::Expected<core::Json, std::string> json =
        DecodeJson(key, *value.value(), "config last-used record");
    if (!json.ok()) return core::make_unexpected(json.error());
    if (json.value().kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("config last-used record must be an object"));
    }
    const core::JsonObject& fields = json.value().as_object();

    // Rust treats a key/record mismatch as corruption rather than a miss.
    const std::string recorded = StringField(fields, "config_id");
    if (recorded != config_id.AsStr()) {
        return core::make_unexpected(std::string("config last-used key mismatch: requested ") +
                                     config_id.AsStr() + ", record has " + recorded);
    }
    core::JsonObject::const_iterator used = fields.find("last_used");
    if (used == fields.end() || used->second.kind() != core::Json::Kind::Int) {
        return core::make_unexpected(std::string("missing field `last_used`"));
    }
    return core::Optional<uint64_t>(static_cast<uint64_t>(used->second.as_int()));
}

core::Expected<core::Unit, std::string>
ImageCacheMetadataStore::RecordConfigRefsFromConfigPath(const std::string& config_path) {
    core::Expected<ImageCacheConfigId, std::string> config_id =
        ImageCacheConfigId::FromConfigPath(config_path);
    if (!config_id.ok()) return core::make_unexpected(config_id.error());

    core::Expected<std::vector<ParsedHardCommitRef>, std::string> hard_refs =
        LoadCacheOwnedHardCommitRefs(config_path);
    if (!hard_refs.ok()) return core::make_unexpected(hard_refs.error());

    std::set<HardCommitId> new_refs;
    for (std::size_t i = 0; i < hard_refs.value().size(); ++i) {
        new_refs.insert(hard_refs.value()[i].digest);
    }

    std::lock_guard<std::mutex> guard(object_update_lock_);

    core::Expected<std::set<HardCommitId>, std::string> existing =
        ConfigRefsSet(config_id.value());
    if (!existing.ok()) return core::make_unexpected(existing.error());

    std::vector<local_store::BatchOp> ops =
        ConfigRefReplacementOps(config_id.value(), existing.value(), new_refs);

    for (std::size_t i = 0; i < hard_refs.value().size(); ++i) {
        const ParsedHardCommitRef& reference = hard_refs.value()[i];
        core::Expected<core::Optional<HardCommitObjectRecord>, std::string> object =
            GetHardCommitObject(reference.digest);
        if (!object.ok()) return core::make_unexpected(object.error());
        std::set<p2p::P2pArtifactKey> p2p_keys;
        if (object.value().has_value()) p2p_keys = object.value()->p2p_keys;

        ops.push_back(HardCommitObjectPutOp(reference.digest,
                                            core::Optional<std::string>(reference.file),
                                            core::Optional<uint64_t>(reference.size),
                                            p2p_keys));
    }

    // This path runs on every resolve, so it doubles as the LRU touch.
    // `RebuildFromConfigs` deliberately does not call it, so reconciles never
    // reset recency.
    if (new_refs.empty()) {
        ops.push_back(local_store::BatchOp::Delete(ConfigLastUsedKey(config_id.value())));
    } else {
        ops.push_back(local_store::BatchOp::Put(
            ConfigLastUsedKey(config_id.value()),
            ConfigLastUsedJson(config_id.value(), UnixNowSecs()).ToString()));
    }

    core::Expected<core::Unit, std::string> written = store_->WriteBatch(ops);
    if (!written.ok()) {
        return core::make_unexpected(std::string("record image cache refs for config ") +
                                     config_id.value().AsStr() + ": " + written.error());
    }
    return core::Unit{};
}

core::Expected<std::vector<HardCommitId>, std::string>
ImageCacheMetadataStore::CommitStoreHardCommitDigestsFromConfigPath(
    const std::string& config_path, const std::string& commit_store) {
    core::Expected<std::vector<ParsedHardCommitRef>, std::string> refs =
        LoadCommitStoreOwnedHardCommitRefs(config_path, commit_store);
    if (!refs.ok()) return core::make_unexpected(refs.error());

    std::vector<HardCommitId> digests;
    for (std::size_t i = 0; i < refs.value().size(); ++i) {
        digests.push_back(refs.value()[i].digest);
    }
    return digests;
}

core::Expected<core::Unit, std::string>
ImageCacheMetadataStore::RemoveConfigRefs(const ImageCacheConfigId& config_id) {
    core::Expected<std::set<HardCommitId>, std::string> existing = ConfigRefsSet(config_id);
    if (!existing.ok()) return core::make_unexpected(existing.error());

    core::Expected<core::Unit, std::string> written =
        store_->WriteBatch(ConfigRefRemovalOps(config_id, existing.value()));
    if (!written.ok()) {
        return core::make_unexpected(std::string("remove image cache refs for config ") +
                                     config_id.AsStr() + ": " + written.error());
    }
    return core::Unit{};
}

core::Expected<std::vector<ImageCacheConfigId>, std::string>
ImageCacheMetadataStore::HardCommitConfigReferrers(const HardCommitId& digest) const {
    core::Expected<std::map<ImageCacheConfigId, std::set<HardCommitId> >, std::string> refs =
        ConfigRefMap();
    if (!refs.ok()) return core::make_unexpected(refs.error());

    std::vector<ImageCacheConfigId> referrers;
    for (std::map<ImageCacheConfigId, std::set<HardCommitId> >::const_iterator it =
             refs.value().begin();
         it != refs.value().end(); ++it) {
        if (it->second.find(digest) != it->second.end()) referrers.push_back(it->first);
    }
    return referrers;
}

core::Expected<std::map<HardCommitId, std::vector<ImageCacheConfigId> >, std::string>
ImageCacheMetadataStore::HardCommitConfigReferrerMap() const {
    core::Expected<std::map<ImageCacheConfigId, std::set<HardCommitId> >, std::string> refs =
        ConfigRefMap();
    if (!refs.ok()) return core::make_unexpected(refs.error());

    std::map<HardCommitId, std::vector<ImageCacheConfigId> > referrers;
    for (std::map<ImageCacheConfigId, std::set<HardCommitId> >::const_iterator it =
             refs.value().begin();
         it != refs.value().end(); ++it) {
        for (std::set<HardCommitId>::const_iterator digest = it->second.begin();
             digest != it->second.end(); ++digest) {
            referrers[*digest].push_back(it->first);
        }
    }
    // The outer map is already config-ordered, so each vector is sorted and
    // duplicate-free by construction.
    return referrers;
}

core::Expected<core::Unit, std::string>
ImageCacheMetadataStore::RebuildFromConfigs(const std::string& configs_dir) {
    // Rust returns an empty map when the directory is absent.
    std::map<ImageCacheConfigId, std::vector<ParsedHardCommitRef> > parsed;
    if (core::fs::Exists(configs_dir)) {
        core::Expected<std::vector<std::string>, std::string> names =
            core::fs::ReadDir(configs_dir);
        if (!names.ok()) {
            return core::make_unexpected(std::string("rebuild image cache metadata from ") +
                                         configs_dir + ": " + names.error());
        }
        for (std::size_t i = 0; i < names.value().size(); ++i) {
            const std::string& name = names.value()[i];
            if (!IsRegularConfigFilename(name)) continue;

            core::Expected<ImageCacheConfigId, std::string> config_id =
                ImageCacheConfigId::FromFilename(name);
            if (!config_id.ok()) return core::make_unexpected(config_id.error());

            core::Expected<std::vector<ParsedHardCommitRef>, std::string> refs =
                LoadCacheOwnedHardCommitRefs(configs_dir + "/" + name);
            // Rust fails closed on a malformed config so a partial graph never
            // becomes the basis for GC decisions.
            if (!refs.ok()) {
                return core::make_unexpected(
                    std::string("rebuild image cache metadata from ") + configs_dir + ": " +
                    refs.error());
            }
            parsed[config_id.value()] = refs.value();
        }
    }

    std::lock_guard<std::mutex> guard(object_update_lock_);

    core::Expected<std::map<ImageCacheConfigId, std::set<HardCommitId> >, std::string>
        existing = ConfigRefMap();
    if (!existing.ok()) return core::make_unexpected(existing.error());
    core::Expected<std::map<ImageCacheConfigId, uint64_t>, std::string> existing_last_used =
        ConfigLastUsedMap();
    if (!existing_last_used.ok()) return core::make_unexpected(existing_last_used.error());

    const uint64_t now = UnixNowSecs();
    std::vector<local_store::BatchOp> ops;

    // Configs that disappeared from disk lose their edges.
    for (std::map<ImageCacheConfigId, std::set<HardCommitId> >::const_iterator it =
             existing.value().begin();
         it != existing.value().end(); ++it) {
        if (parsed.find(it->first) == parsed.end()) {
            std::vector<local_store::BatchOp> removal =
                ConfigRefRemovalOps(it->first, it->second);
            ops.insert(ops.end(), removal.begin(), removal.end());
        }
    }

    for (std::map<ImageCacheConfigId, std::vector<ParsedHardCommitRef> >::const_iterator it =
             parsed.begin();
         it != parsed.end(); ++it) {
        std::set<HardCommitId> new_refs;
        for (std::size_t i = 0; i < it->second.size(); ++i) {
            new_refs.insert(it->second[i].digest);
        }
        std::set<HardCommitId> old_refs;
        std::map<ImageCacheConfigId, std::set<HardCommitId> >::const_iterator previous =
            existing.value().find(it->first);
        if (previous != existing.value().end()) old_refs = previous->second;

        std::vector<local_store::BatchOp> replacement =
            ConfigRefReplacementOps(it->first, old_refs, new_refs);
        ops.insert(ops.end(), replacement.begin(), replacement.end());

        for (std::size_t i = 0; i < it->second.size(); ++i) {
            const ParsedHardCommitRef& reference = it->second[i];
            core::Expected<core::Optional<HardCommitObjectRecord>, std::string> object =
                GetHardCommitObject(reference.digest);
            if (!object.ok()) return core::make_unexpected(object.error());
            std::set<p2p::P2pArtifactKey> p2p_keys;
            if (object.value().has_value()) p2p_keys = object.value()->p2p_keys;

            ops.push_back(HardCommitObjectPutOp(reference.digest,
                                                core::Optional<std::string>(reference.file),
                                                core::Optional<uint64_t>(reference.size),
                                                p2p_keys));
        }

        // Rebuilds preserve recency; only new configs get a baseline touch.
        if (new_refs.empty()) {
            ops.push_back(local_store::BatchOp::Delete(ConfigLastUsedKey(it->first)));
        } else if (existing_last_used.value().find(it->first) ==
                   existing_last_used.value().end()) {
            ops.push_back(local_store::BatchOp::Put(
                ConfigLastUsedKey(it->first),
                ConfigLastUsedJson(it->first, now).ToString()));
        }
    }

    core::Expected<core::Unit, std::string> written = store_->WriteBatch(ops);
    if (!written.ok()) {
        return core::make_unexpected(std::string("write image cache metadata rebuild from ") +
                                     configs_dir + ": " + written.error());
    }
    return core::Unit{};
}

// ---- holds ------------------------------------------------------------------

core::Expected<std::set<HardCommitId>, std::string>
ImageCacheMetadataStore::HoldRefsSet(const ImageCacheHoldOwner& owner) const {
    core::Expected<std::vector<local_store::Entry>, std::string> entries =
        store_->ScanPrefix(HoldToHardPrefixForOwner(owner));
    if (!entries.ok()) return core::make_unexpected(entries.error());

    std::set<HardCommitId> refs;
    const std::vector<local_store::Entry>& all = entries.value();
    for (std::size_t i = 0; i < all.size(); ++i) {
        core::Expected<core::Json, std::string> json =
            DecodeJson(all[i].first, all[i].second, "hold-to-hard reference");
        if (!json.ok()) return core::make_unexpected(json.error());
        if (json.value().kind() != core::Json::Kind::Object) continue;
        core::Expected<HardCommitId, std::string> digest =
            HardCommitId::New(StringField(json.value().as_object(), "digest"));
        if (!digest.ok()) return core::make_unexpected(digest.error());
        refs.insert(digest.value());
    }
    return refs;
}

core::Expected<core::Unit, std::string>
ImageCacheMetadataStore::CreateOrReplaceHold(const ImageCacheHoldOwner& owner,
                                             const std::set<HardCommitId>& refs) {
    core::Expected<std::set<HardCommitId>, std::string> existing = HoldRefsSet(owner);
    if (!existing.ok()) return core::make_unexpected(existing.error());

    std::vector<local_store::BatchOp> ops =
        HoldRefReplacementOps(owner, existing.value(), refs);
    ops.push_back(
        local_store::BatchOp::Put(HoldRecordKey(owner), HoldRecordJson(owner).ToString()));

    core::Expected<core::Unit, std::string> written = store_->WriteBatch(ops);
    if (!written.ok()) {
        return core::make_unexpected(std::string("replace image cache hold ") +
                                     owner.ToString() + ": " + written.error());
    }
    return core::Unit{};
}

core::Expected<core::Unit, std::string>
ImageCacheMetadataStore::ReleaseHold(const ImageCacheHoldOwner& owner) {
    core::Expected<std::set<HardCommitId>, std::string> existing = HoldRefsSet(owner);
    if (!existing.ok()) return core::make_unexpected(existing.error());

    core::Expected<core::Unit, std::string> written =
        store_->WriteBatch(HoldRefRemovalOps(owner, existing.value()));
    if (!written.ok()) {
        return core::make_unexpected(std::string("release image cache hold ") +
                                     owner.ToString() + ": " + written.error());
    }
    return core::Unit{};
}

core::Expected<std::vector<ImageCacheHoldOwner>, std::string>
ImageCacheMetadataStore::ListHoldOwnersInNamespaces(
    const std::vector<std::string>& namespaces) const {
    core::Expected<std::vector<local_store::Entry>, std::string> entries =
        store_->ScanPrefix(kHoldRecordPrefix);
    if (!entries.ok()) return core::make_unexpected(entries.error());

    std::vector<ImageCacheHoldOwner> owners;
    const std::vector<local_store::Entry>& all = entries.value();
    for (std::size_t i = 0; i < all.size(); ++i) {
        core::Expected<core::Json, std::string> json =
            DecodeJson(all[i].first, all[i].second, "hold record");
        if (!json.ok()) return core::make_unexpected(json.error());
        if (json.value().kind() != core::Json::Kind::Object) continue;
        const core::JsonObject& fields = json.value().as_object();

        const std::string ns = StringField(fields, "namespace");
        if (std::find(namespaces.begin(), namespaces.end(), ns) == namespaces.end()) {
            continue;
        }
        core::Expected<ImageCacheHoldOwner, std::string> owner =
            ImageCacheHoldOwner::New(ns, StringField(fields, "key"));
        if (!owner.ok()) return core::make_unexpected(owner.error());
        owners.push_back(owner.value());
    }
    return owners;
}

core::Expected<std::vector<ImageCacheHoldOwner>, std::string>
ImageCacheMetadataStore::ReleaseHoldsInNamespaces(
    const std::vector<std::string>& namespaces) {
    core::Expected<std::vector<ImageCacheHoldOwner>, std::string> owners =
        ListHoldOwnersInNamespaces(namespaces);
    if (!owners.ok()) return core::make_unexpected(owners.error());

    std::vector<local_store::BatchOp> ops;
    for (std::size_t i = 0; i < owners.value().size(); ++i) {
        core::Expected<std::set<HardCommitId>, std::string> refs =
            HoldRefsSet(owners.value()[i]);
        if (!refs.ok()) return core::make_unexpected(refs.error());
        std::vector<local_store::BatchOp> removal =
            HoldRefRemovalOps(owners.value()[i], refs.value());
        ops.insert(ops.end(), removal.begin(), removal.end());
    }
    if (!ops.empty()) {
        core::Expected<core::Unit, std::string> written = store_->WriteBatch(ops);
        if (!written.ok()) {
            return core::make_unexpected(
                std::string("release stale image cache holds at startup: ") + written.error());
        }
    }
    return owners.value();
}

core::Expected<std::vector<ImageCacheHoldOwner>, std::string>
ImageCacheMetadataStore::HardCommitHoldReferrers(const HardCommitId& digest) const {
    core::Expected<std::vector<local_store::Entry>, std::string> entries =
        store_->ScanPrefix(HardToHoldPrefixForDigest(digest));
    if (!entries.ok()) return core::make_unexpected(entries.error());

    // A set, so the result is owner-ordered and duplicate-free like Rust's.
    std::set<ImageCacheHoldOwner> owners;
    const std::vector<local_store::Entry>& all = entries.value();
    for (std::size_t i = 0; i < all.size(); ++i) {
        core::Expected<core::Json, std::string> json =
            DecodeJson(all[i].first, all[i].second, "hard-to-hold reference");
        if (!json.ok()) return core::make_unexpected(json.error());
        if (json.value().kind() != core::Json::Kind::Object) continue;
        const core::JsonObject& fields = json.value().as_object();

        core::Expected<ImageCacheHoldOwner, std::string> owner = ImageCacheHoldOwner::New(
            StringField(fields, "namespace"), StringField(fields, "key"));
        if (!owner.ok()) return core::make_unexpected(owner.error());
        owners.insert(owner.value());
    }
    return std::vector<ImageCacheHoldOwner>(owners.begin(), owners.end());
}

// ---- capacity ---------------------------------------------------------------

core::Expected<CapacityEvictionPlan, std::string>
ImageCacheMetadataStore::PlanCapacityEvictionFromStore(uint64_t high_watermark_bytes,
                                                        uint64_t low_watermark_bytes,
                                                        uint64_t evictable_before) const {
    core::Expected<std::map<ImageCacheConfigId, std::set<HardCommitId> >, std::string>
        config_refs = ConfigRefMap();
    if (!config_refs.ok()) return core::make_unexpected(config_refs.error());

    core::Expected<std::vector<HardCommitObjectRecord>, std::string> objects =
        ListHardCommitObjects();
    if (!objects.ok()) return core::make_unexpected(objects.error());

    std::map<HardCommitId, uint64_t> sizes;
    for (std::size_t i = 0; i < objects.value().size(); ++i) {
        const HardCommitObjectRecord& record = objects.value()[i];
        // Rust uses `size.unwrap_or(0)`: a digest-only record frees nothing.
        sizes[record.digest] = record.size.has_value() ? *record.size : 0;
    }

    core::Expected<std::map<ImageCacheConfigId, uint64_t>, std::string> last_used =
        ConfigLastUsedMap();
    if (!last_used.ok()) return core::make_unexpected(last_used.error());

    return PlanCapacityEviction(config_refs.value(), sizes, last_used.value(),
                                high_watermark_bytes, low_watermark_bytes,
                                evictable_before);
}

}  // namespace cache
}  // namespace image
}  // namespace agentenv
