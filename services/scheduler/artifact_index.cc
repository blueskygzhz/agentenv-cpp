// SPDX-License-Identifier: MIT
// Go upstream: services/scheduler/internal/store.go :: InMemoryArtifactStore.
#include "services/scheduler/artifact_index.h"

namespace agentenv {
namespace services {
namespace scheduler {

const std::size_t kDefaultArtifactStoreCapacity = 1000000;

namespace {

std::string Trim(const std::string& s) {
    const std::size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return std::string();
    const std::size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

}  // namespace

bool ArtifactIndexKey::operator<(const ArtifactIndexKey& o) const {
    if (cluster_id != o.cluster_id) return cluster_id < o.cluster_id;
    if (backend != o.backend) return backend < o.backend;
    return key < o.key;
}

bool ArtifactIndexKey::operator==(const ArtifactIndexKey& o) const {
    return cluster_id == o.cluster_id && backend == o.backend && key == o.key;
}

bool NormalizeArtifactIndexKey(const std::string& cluster_id, const std::string& backend,
                               const std::string& key, ArtifactIndexKey* out) {
    ArtifactIndexKey index_key;
    index_key.cluster_id = Trim(cluster_id);
    index_key.backend = Trim(backend);
    index_key.key = Trim(key);
    if (out) *out = index_key;
    // All three required. An entry under a blank cluster id would be visible
    // to every cluster, and one under a blank backend would hand out addresses
    // in a format the requester cannot dial.
    return !index_key.cluster_id.empty() && !index_key.backend.empty() &&
           !index_key.key.empty();
}

InMemoryArtifactStore::InMemoryArtifactStore(std::size_t capacity,
                                             std::size_t lookup_node_limit)
    : capacity_(capacity == 0 ? kDefaultArtifactStoreCapacity : capacity),
      lookup_node_limit_(lookup_node_limit) {}

void InMemoryArtifactStore::TouchLocked(const ArtifactIndexKey& index_key) {
    std::map<ArtifactIndexKey, std::list<ArtifactIndexKey>::iterator>::iterator at =
        recency_pos_.find(index_key);
    if (at != recency_pos_.end()) {
        recency_.erase(at->second);
        recency_pos_.erase(at);
    }
    recency_.push_back(index_key);
    std::list<ArtifactIndexKey>::iterator last = recency_.end();
    --last;
    recency_pos_[index_key] = last;
}

void InMemoryArtifactStore::EvictOldestLocked() {
    if (recency_.empty()) return;
    const ArtifactIndexKey oldest = recency_.front();

    // Go's eviction callback clears both directions. Dropping only `entries_`
    // would leave `node_keys_` naming a key that no longer exists, and a later
    // `ForgetNode` would then walk into it.
    std::map<ArtifactIndexKey, std::set<std::string> >::iterator entry =
        entries_.find(oldest);
    if (entry != entries_.end()) {
        for (std::set<std::string>::const_iterator node = entry->second.begin();
             node != entry->second.end(); ++node) {
            std::map<std::string, std::set<ArtifactIndexKey> >::iterator keys =
                node_keys_.find(*node);
            if (keys == node_keys_.end()) continue;
            keys->second.erase(oldest);
            if (keys->second.empty()) node_keys_.erase(keys);
        }
        entries_.erase(entry);
    }

    recency_pos_.erase(oldest);
    recency_.pop_front();
}

void InMemoryArtifactStore::Record(const std::string& cluster_id,
                                   const std::string& backend,
                                   const std::string& key,
                                   const std::string& node_id) {
    ArtifactIndexKey index_key;
    const bool usable = NormalizeArtifactIndexKey(cluster_id, backend, key, &index_key);
    const std::string node = Trim(node_id);
    // Go returns silently. The index is advisory, so a malformed record is a
    // no-op rather than an error the caller has to handle: the worst outcome
    // is a peer that is never advertised, which costs a slower fetch.
    if (!usable || node.empty()) return;

    std::lock_guard<std::mutex> guard(mutex_);
    entries_[index_key].insert(node);
    node_keys_[node].insert(index_key);
    TouchLocked(index_key);

    // After the insert, so a record that lands at capacity evicts something
    // older rather than itself.
    while (entries_.size() > capacity_) {
        EvictOldestLocked();
    }
}

void InMemoryArtifactStore::Forget(const std::string& cluster_id,
                                   const std::string& backend,
                                   const std::string& key,
                                   const std::string& node_id) {
    ArtifactIndexKey index_key;
    const bool usable = NormalizeArtifactIndexKey(cluster_id, backend, key, &index_key);
    const std::string node = Trim(node_id);
    if (!usable || node.empty()) return;

    std::lock_guard<std::mutex> guard(mutex_);
    ForgetLocked(index_key, node);
}

void InMemoryArtifactStore::ForgetLocked(const ArtifactIndexKey& index_key,
                                         const std::string& node_id) {
    std::map<ArtifactIndexKey, std::set<std::string> >::iterator entry =
        entries_.find(index_key);
    if (entry != entries_.end()) {
        entry->second.erase(node_id);
        // The key itself goes once nobody claims it. Keeping an empty set
        // would make `Lookup` report a hit with no providers, which reads to
        // the caller as "somebody has it" and costs a pointless round trip.
        if (entry->second.empty()) RemoveArtifactKeyLocked(index_key);
    }

    std::map<std::string, std::set<ArtifactIndexKey> >::iterator keys =
        node_keys_.find(node_id);
    if (keys != node_keys_.end()) {
        keys->second.erase(index_key);
        if (keys->second.empty()) node_keys_.erase(keys);
    }
}

void InMemoryArtifactStore::RemoveArtifactKeyLocked(const ArtifactIndexKey& index_key) {
    entries_.erase(index_key);
    std::map<ArtifactIndexKey, std::list<ArtifactIndexKey>::iterator>::iterator at =
        recency_pos_.find(index_key);
    if (at != recency_pos_.end()) {
        recency_.erase(at->second);
        recency_pos_.erase(at);
    }
}

std::vector<std::string> InMemoryArtifactStore::Lookup(const std::string& cluster_id,
                                                       const std::string& backend,
                                                       const std::string& key) {
    ArtifactIndexKey index_key;
    if (!NormalizeArtifactIndexKey(cluster_id, backend, key, &index_key)) {
        return std::vector<std::string>();
    }

    std::lock_guard<std::mutex> guard(mutex_);
    std::map<ArtifactIndexKey, std::set<std::string> >::const_iterator entry =
        entries_.find(index_key);
    if (entry == entries_.end() || entry->second.empty()) {
        // A miss does not count as a use: touching here would let a flood of
        // lookups for keys nobody has evict the ones that exist.
        return std::vector<std::string>();
    }
    TouchLocked(index_key);

    std::vector<std::string> out;
    out.reserve(entry->second.size());
    for (std::set<std::string>::const_iterator node = entry->second.begin();
         node != entry->second.end(); ++node) {
        out.push_back(*node);
        if (lookup_node_limit_ > 0 && out.size() >= lookup_node_limit_) break;
    }
    return out;
}

void InMemoryArtifactStore::ForgetNode(const std::string& node_id) {
    const std::string node = Trim(node_id);
    if (node.empty()) return;

    std::lock_guard<std::mutex> guard(mutex_);
    std::map<std::string, std::set<ArtifactIndexKey> >::iterator keys =
        node_keys_.find(node);
    if (keys == node_keys_.end()) return;

    // Iterated from the reverse index rather than by scanning `entries_`,
    // which is the reason that index exists: a node departure would otherwise
    // walk every key in the cluster.
    for (std::set<ArtifactIndexKey>::const_iterator index_key = keys->second.begin();
         index_key != keys->second.end(); ++index_key) {
        std::map<ArtifactIndexKey, std::set<std::string> >::iterator entry =
            entries_.find(*index_key);
        if (entry == entries_.end()) continue;
        entry->second.erase(node);
        if (entry->second.empty()) RemoveArtifactKeyLocked(*index_key);
    }
    node_keys_.erase(keys);
}

std::size_t InMemoryArtifactStore::size() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return entries_.size();
}

}  // namespace scheduler
}  // namespace services
}  // namespace agentenv
