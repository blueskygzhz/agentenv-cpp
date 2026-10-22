// SPDX-License-Identifier: MIT
// Go upstream: services/scheduler/internal/store.go ::
// InMemoryArtifactStore — the `key -> nodes holding it` index behind the four
// P2P scheduler calls.
//
// This is the server side of what `p2p/scheduler_http.h` dials. The client
// half went in first; without this there was nothing in the tree for it to
// talk to, so an operator had to supply a scheduler of their own to use P2P at
// all.
//
// Two maps, not one. `entries` answers "who has this key"; `node_keys`
// answers "what does this node have". The reverse direction exists for
// `ForgetNode`: when a node drops out, every key it claimed has to stop
// naming it, and finding those by scanning `entries` would be a walk over the
// whole index on every node departure.
//
// Bounded on purpose. The index is advisory — a miss costs a slower fetch, not
// a failure — so a cluster publishing more keys than the cap must shed old
// ones rather than grow without limit. Eviction is LRU by last lookup, which
// keeps whatever is actually being fetched.
#ifndef AGENTENV_SERVICES_SCHEDULER_ARTIFACT_INDEX_H_
#define AGENTENV_SERVICES_SCHEDULER_ARTIFACT_INDEX_H_

#include <cstddef>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace agentenv {
namespace services {
namespace scheduler {

/// Go `defaultArtifactStoreCapacity`.
extern const std::size_t kDefaultArtifactStoreCapacity;

/// Go `artifactIndexKey` — the index is per cluster *and* per backend.
///
/// Both are part of the key rather than filters applied afterwards. A node
/// serving `iroh` cannot serve an `http` peer's request, so an index that
/// merged the two would hand out addresses in the wrong format and the
/// requester would have to discover that by failing to dial them.
struct ArtifactIndexKey {
    std::string cluster_id;
    std::string backend;
    std::string key;

    bool operator<(const ArtifactIndexKey& o) const;
    bool operator==(const ArtifactIndexKey& o) const;
};

/// Go `normalizeArtifactIndexKey` — trims, and reports whether all three
/// components survived.
///
/// A blank component makes the key unusable rather than defaulting: an entry
/// under an empty cluster id would be visible to every cluster.
bool NormalizeArtifactIndexKey(const std::string& cluster_id, const std::string& backend,
                               const std::string& key, ArtifactIndexKey* out);

/// Go `InMemoryArtifactStore`.
class InMemoryArtifactStore {
 public:
    /// `capacity <= 0` takes the default. `lookup_node_limit == 0` means no
    /// limit, matching Go's `> 0` guards.
    explicit InMemoryArtifactStore(std::size_t capacity = 0,
                                   std::size_t lookup_node_limit = 0);

    /// Go `Record` — note `nodeID` is the node that *holds* the artifact.
    ///
    /// Idempotent: the node set is a set. A node republishing the same key is
    /// the normal case, since every fetch adopts what it pulled.
    void Record(const std::string& cluster_id, const std::string& backend,
                const std::string& key, const std::string& node_id);

    /// Go `Forget` — drops one node's claim on one key.
    void Forget(const std::string& cluster_id, const std::string& backend,
                const std::string& key, const std::string& node_id);

    /// Go `Lookup` — the node ids claiming `key`, capped at the lookup limit.
    ///
    /// Counts as a use for eviction purposes, which is the whole point of the
    /// LRU being keyed on lookups rather than on writes: a key written once
    /// and fetched constantly is the one worth keeping.
    std::vector<std::string> Lookup(const std::string& cluster_id,
                                    const std::string& backend,
                                    const std::string& key);

    /// Go `ForgetNode` — drops every claim a node made.
    ///
    /// Called when a node leaves the registry. Without it the index would keep
    /// directing peers at a node that is gone, and they would each pay a
    /// failed dial to find out.
    void ForgetNode(const std::string& node_id);

    /// Number of keys currently indexed.
    std::size_t size() const;

 private:
    void ForgetLocked(const ArtifactIndexKey& index_key, const std::string& node_id);
    void RemoveArtifactKeyLocked(const ArtifactIndexKey& index_key);
    /// Drops the least recently looked-up key, with its reverse edges.
    void EvictOldestLocked();
    /// Moves `index_key` to the most-recent end.
    void TouchLocked(const ArtifactIndexKey& index_key);

    mutable std::mutex mutex_;
    std::size_t        capacity_;
    std::size_t        lookup_node_limit_;

    /// key -> node ids holding it.
    std::map<ArtifactIndexKey, std::set<std::string> >    entries_;
    /// node id -> keys it holds. The reverse of `entries_`, kept in step.
    std::map<std::string, std::set<ArtifactIndexKey> >    node_keys_;

    /// Recency order, least recent first. A `std::list` plus an iterator map
    /// stands in for Go's `lru.Cache`: both give O(1) touch and eviction,
    /// which matters because `Lookup` touches on every call.
    std::list<ArtifactIndexKey>                                      recency_;
    std::map<ArtifactIndexKey, std::list<ArtifactIndexKey>::iterator> recency_pos_;
};

}  // namespace scheduler
}  // namespace services
}  // namespace agentenv
#endif  // AGENTENV_SERVICES_SCHEDULER_ARTIFACT_INDEX_H_
