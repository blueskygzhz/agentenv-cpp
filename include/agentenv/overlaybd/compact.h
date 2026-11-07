// SPDX-License-Identifier: MIT
// Rust: src/overlaybd/compact.rs — `OverlaybdCompactOutput` only.
//
// Which physical form a layer is written in when it is compacted for upload.
// Capture never consults this: local layers always stay raw so a local resume
// pays no decompression cost. It is the *upload* paths that recontainerize.
#ifndef AGENTENV_OVERLAYBD_COMPACT_H_
#define AGENTENV_OVERLAYBD_COMPACT_H_

#include <cstddef>
#include <string>
#include <vector>

#include "agentenv/cfg.h"
#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/storage/overlaybd/config.h"

namespace agentenv {
namespace overlaybd {

/// Rust `const MAX_COMPRESSION_WORKERS`.
///
/// A ceiling on *OS threads*, not a tuning hint: `workers` comes from operator
/// config, and an absurd value would spawn that many threads per layer during
/// an upload.
extern const std::size_t kMaxCompressionWorkers;

/// Rust `enum OverlaybdCompactOutput`.
struct OverlaybdCompactOutput {
    enum class Kind { Raw, ZFile };

    Kind kind = Kind::Raw;
    /// `ZFile` only.
    cfg::OverlaybdCompressionAlgorithm algorithm = cfg::OverlaybdCompressionAlgorithm::Lz4;
    /// `ZFile` only. Already clamped to `[1, kMaxCompressionWorkers]`.
    std::size_t workers = 1;

    OverlaybdCompactOutput() {}

    static OverlaybdCompactOutput Raw() {
        OverlaybdCompactOutput out;
        out.kind = Kind::Raw;
        return out;
    }
    static OverlaybdCompactOutput ZFile(cfg::OverlaybdCompressionAlgorithm algorithm,
                                        std::size_t workers) {
        OverlaybdCompactOutput out;
        out.kind = Kind::ZFile;
        out.algorithm = algorithm;
        out.workers = workers;
        return out;
    }

    bool is_raw() const { return kind == Kind::Raw; }
    bool is_zfile() const { return kind == Kind::ZFile; }

    /// Rust `from_publish_compression_config`.
    ///
    /// `workers` is clamped here rather than where the threads are spawned, so
    /// every consumer of this value sees an already-sane count.
    static OverlaybdCompactOutput FromPublishCompressionConfig(
        const cfg::SnapshotPublishCompressionConfig& config);

    /// Rust `#[derive(Eq, PartialEq)]`: the `Raw` variant carries nothing, so
    /// a stale algorithm/worker count must not take part.
    bool operator==(const OverlaybdCompactOutput& o) const {
        if (kind != o.kind) return false;
        if (kind == Kind::Raw) return true;
        return algorithm == o.algorithm && workers == o.workers;
    }
    bool operator!=(const OverlaybdCompactOutput& o) const { return !(*this == o); }
};

/// Rust `compact_layers` — merge sealed overlaybd layers into one sealed
/// commit at `output_path`, written in the physical form `mode` asks for.
///
/// `layers` is **bottom-to-top**: oldest base first, newest lower last. Only
/// `LayerConfig::file` is read; the struct is the parameter type to match
/// Rust, whose callers already hold one. Each input is opened through the
/// tar-adaptor + switch-file chain, so raw and zfile-compressed layers can be
/// mixed freely in one compaction.
///
/// Returns the published path, or an empty optional when `layers` is empty --
/// "nothing to compact" is not an error, and a caller that expected output is
/// the one positioned to say so.
///
/// **Atomic publish.** The merge goes to a sibling temp file which is renamed
/// into place only on success; a failure removes the temp file and leaves any
/// pre-existing `output_path` untouched. Both halves matter: without the
/// rename a reader could observe a half-written layer, and without the
/// cleanup a failed upload would leave the staging directory growing.
///
/// Deviation from Rust: the compaction itself is single-threaded (Rust sets
/// `concurrency = 32`). The *compression* still fans out across
/// `mode.workers` threads inside the zfile writer, which is where the cost
/// is; what is sequential is the block copying. The output bytes are
/// identical either way -- Rust's concurrent branches exist for throughput
/// and both reorder their results before writing.
///
/// The output carries the nil uuid and no parent, matching Rust's
/// `CommitArgs::from_writer`: a compacted layer is a new object, and
/// inheriting the source's identity would make the layer below claim it as
/// the same child twice.
core::Expected<core::Optional<std::string>, std::string> CompactLayers(
    const std::vector<storage::overlaybd::LayerConfig>& layers, const std::string& output_path,
    const OverlaybdCompactOutput& mode);

}  // namespace overlaybd
}  // namespace agentenv
#endif  // AGENTENV_OVERLAYBD_COMPACT_H_
