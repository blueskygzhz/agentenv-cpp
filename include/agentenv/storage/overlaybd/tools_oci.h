// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/tools/oci.rs — the overlaybd CLI tool wrappers.
#ifndef AGENTENV_STORAGE_OVERLAYBD_TOOLS_OCI_H_
#define AGENTENV_STORAGE_OVERLAYBD_TOOLS_OCI_H_

#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"

namespace agentenv {
namespace storage {
namespace overlaybd {
namespace tools {

/// Rust `ToolExecutionError` — why a tool invocation did not produce a layer.
///
/// A tagged struct rather than a class hierarchy: the caller branches on
/// `kind` (a missing binary is a provisioning problem, a non-zero exit is the
/// tool's own verdict), and the detail fields are what makes a failed
/// conversion diagnosable at all. `command_line` and `stderr` in particular:
/// without them a failure is just "overlaybd-apply exited 1".
struct ToolExecutionError {
    enum Kind {
        kMissingBinary,
        kInvalidRequest,
        kNonZeroExit,
        kOther
    };

    Kind kind;
    /// Rust's `#[error(..)]` rendering, so a caller that only logs has one
    /// field to reach for.
    std::string message;
    /// Set for `kMissingBinary`.
    std::string path;
    /// Set for `kNonZeroExit`.
    std::string command_line;
    std::string stdout_text;
    std::string stderr_text;
    /// Rust's `Option<i32>`: absent when the child was killed by a signal
    /// rather than exiting.
    core::Optional<int> exit_code;

    ToolExecutionError() : kind(kOther) {}
};

/// Rust `StepSummary`.
struct StepSummary {
    std::string stdout_text;
    std::string stderr_text;
};

/// Rust `CreateLayerRequest`.
struct CreateLayerRequest {
    std::string data_file;
    std::string index_file;
    /// Virtual size in GiB -- passed straight to `overlaybd-create`.
    uint64_t virtual_size_gib;

    CreateLayerRequest() : virtual_size_gib(0) {}
};

/// Rust `CommitLayerRequest`.
struct CommitLayerRequest {
    std::string data_file;
    std::string index_file;
    std::string commit_file;
    /// Empty means "do not pass the flag", matching Rust's `Option<Uuid>`.
    std::string uuid;
    std::string parent_uuid;
};

/// Rust `ConvertLayerRequest`.
struct ConvertLayerRequest {
    std::string work_dir;
    std::string input_layer_path;
    std::string global_config_path;
    uint64_t virtual_size_gib;
    bool mkfs;
    std::vector<std::string> lower_layers;
    std::string uuid;
    std::string parent_uuid;

    ConvertLayerRequest() : virtual_size_gib(0), mkfs(false) {}
};

/// Rust `ConvertLayerResult`.
struct ConvertLayerResult {
    /// The sealed overlaybd lower layer the conversion produced.
    std::string layer_commit;
};

/// Rust's work-directory file names. Public because the cleanliness check is
/// part of the contract: a caller has to know which names the conversion
/// claims in `work_dir`.
extern const char* const kUpperData;
extern const char* const kUpperIndex;
extern const char* const kImageJson;
extern const char* const kLayerCommit;
extern const char* const kResultTxt;

/// Rust `is_zstd_magic`.
///
/// Exposed because the zstd branch exists to work around a specific
/// overlaybd-apply version, and the magic test is the only part of it that can
/// be checked without the tool present.
bool IsZstdMagic(const uint8_t magic[4]);

/// Rust `write_image_json`.
///
/// Separate from the conversion so it is testable without any binary: the
/// field names here (`lowers`, `upper.data`, `upper.index`, `resultFile`) are
/// a wire contract with `overlaybd-apply`, and a typo would only show up as
/// the tool failing to find its upper.
core::Expected<core::Unit, ToolExecutionError> WriteImageJson(
    const std::string& path, const std::vector<std::string>& lower_layers,
    const std::string& upper_data, const std::string& upper_index,
    const std::string& result_file);

/// Rust `OverlaybdTools` — a handle onto the three CLI binaries.
class OverlaybdTools {
 public:
    /// Rust `from_bin_dir`.
    static OverlaybdTools FromBinDir(const std::string& bin_dir);
    /// Rust `from_overlaybd_install_root` — `root/bin`.
    static OverlaybdTools FromOverlaybdInstallRoot(const std::string& root);

    const std::string& CreateBin() const { return create_bin_; }
    const std::string& ApplyBin() const { return apply_bin_; }
    const std::string& CommitBin() const { return commit_bin_; }

    /// Rust `create_writable_layer`.
    core::Expected<StepSummary, ToolExecutionError> CreateWritableLayer(
        const CreateLayerRequest& req, const std::string& work_dir) const;

    /// Rust `commit_writable_layer`.
    core::Expected<StepSummary, ToolExecutionError> CommitWritableLayer(
        const CommitLayerRequest& req, const std::string& work_dir) const;

    /// Rust `convert_local_oci_layer_to_overlaybd` — one local OCI tar layer
    /// into a sealed overlaybd lower layer.
    ///
    /// On success the build-time artifacts (`upper.data`, `upper.index`,
    /// `image.json`, `result.txt`) are removed and only `layer.commit`
    /// remains. On failure they are **kept**: a conversion that failed
    /// halfway is only diagnosable from what it left behind.
    core::Expected<ConvertLayerResult, ToolExecutionError>
    ConvertLocalOciLayerToOverlaybd(const ConvertLayerRequest& req) const;

 private:
    OverlaybdTools() {}

    /// Rust `apply_oci_layer`.
    ///
    /// Private for the same reason as in Rust: it is only correct between a
    /// create and a commit, and it expects the `image.json` the conversion
    /// wrote.
    ///
    /// One deliberate simplification against Rust: when the input is zstd and
    /// the decompressor fails, Rust reports the decompressor's captured
    /// stderr. Here `zstd`'s stderr is left inherited so it reaches the log,
    /// and the error carries only its exit code. Capturing it would mean
    /// draining a third pipe concurrently with the apply's two, and the apply
    /// verdict already wins in every case where both fail -- a failed apply
    /// closes the pipe, which kills `zstd` with SIGPIPE, so reporting the
    /// decompressor would name the symptom instead of the cause.
    core::Expected<StepSummary, ToolExecutionError> ApplyOciLayer(
        const std::string& input_layer_path, const std::string& image_config_path,
        const std::string& global_config_path, bool mkfs,
        const std::string& work_dir) const;

    std::string create_bin_;
    std::string apply_bin_;
    std::string commit_bin_;
};

}  // namespace tools
}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_TOOLS_OCI_H_
