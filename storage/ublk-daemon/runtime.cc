// SPDX-License-Identifier: MIT
// Rust: storage/ublk-daemon/src/runtime.rs
#include "agentenv/storage/ublk-daemon/runtime.h"

#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <sstream>
#include <utility>

#include "agentenv/core/fs.h"
#include "agentenv/core/logging.h"
#include "agentenv/storage/overlaybd/image_file.h"
#include "agentenv/storage/overlaybd/image_helper.h"
#include "agentenv/storage/overlaybd/layer_metadata.h"

namespace agentenv {
namespace storage {
namespace ublk_daemon {

const uint64_t kGib = 1024ull * 1024ull * 1024ull;
const std::size_t kResizeOutputLimit = 64u * 1024u;
const char* const kResizeOutputTruncatedMarker = " ... (truncated)";
const char* const kRuntimeImageConfigFile = "image.json";
const char* const kRuntimeUpperDataFile = "upper.data";
const char* const kRuntimeUpperIndexFile = "upper.index";
const char* const kRuntimeResultFile = "result.txt";

namespace {

/// Rust `std::fs::canonicalize(p).unwrap_or_else(|_| p.to_path_buf())`.
///
/// The fallback is load-bearing, not laziness: the path may not exist yet, and
/// the caller only wants a stable cache key / device key. Failing here would
/// reject configs that resolve perfectly well.
std::string CanonicalizeOrSelf(const std::string& path) {
    char buf[PATH_MAX];
    if (::realpath(path.c_str(), buf) != 0) return path;
    return std::string(buf);
}

/// Rust `String::from_utf8_lossy(..).trim()`.
std::string Trimmed(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && ::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && ::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(begin, end - begin);
}

std::string ToDecimal(uint64_t value) {
    std::ostringstream o;
    o << value;
    return o.str();
}

/// Rust `ExitStatus`'s `Display`, which is what lands in the error message.
std::string DescribeWaitStatus(int status) {
    std::ostringstream o;
    if (WIFEXITED(status)) {
        o << "exit status: " << WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        o << "signal: " << WTERMSIG(status);
    } else {
        o << "unknown status " << status;
    }
    return o.str();
}

/// Rust `ClaimedRuntimeDir::claim`.
///
/// `mkdir`, not `create_dir_all`: the directory must *not* already exist. An
/// existing one may be backing a live device, and adopting it would give two
/// devices the same upper. `CreateDirAll` succeeds on an existing directory,
/// so it cannot be used here.
core::Expected<core::Unit, std::string> ClaimRuntimeDir(const std::string& path) {
    if (::mkdir(path.c_str(), 0755) != 0) {
        return core::make_unexpected(std::string("claim overlaybd runtime dir ") + path +
                                     " (it must not already exist): " + std::strerror(errno));
    }
    return core::Unit();
}

/// Rust `ClaimedRuntimeDir::cleanup`.
void CleanupRuntimeDir(const std::string& path) {
    const core::Expected<core::Unit, std::string> removed = core::fs::RemoveDirAll(path);
    if (!removed.ok()) {
        AGENTENV_WARN("failed to remove daemon-owned overlaybd runtime dir during rollback: "
                      << path << ": " << removed.error());
    }
}

/// Rust `relative_path(runtime_dir, path)?.to_string_lossy()` for an optional
/// source path, with the absent case becoming `""`.
///
/// Empty means "unset" in an overlaybd upper, so an absent target and an
/// absent gzip index both have to serialize as the empty string rather than
/// being omitted: the loader reads a missing key as unset too, but the tools
/// round-trip the config and an omitted key has been seen to come back wrong.
core::Expected<std::string, std::string> RelativeOrEmpty(
    const std::string& runtime_dir, const core::Optional<std::string>& path) {
    if (!path.has_value()) return std::string();
    return overlaybd::RelativePath(runtime_dir, *path);
}

/// Rust `serde_json::from_value::<UpperConfig>(value["upper"])`, including the
/// `unwrap_or_else(|| json!({}))` for an absent key.
core::Expected<overlaybd::UpperConfig, std::string> UpperFromJson(const core::Json& value) {
    if (value.kind() != core::Json::Kind::Object) {
        return overlaybd::ParseUpperConfig(core::Json());
    }
    const core::JsonObject& object = value.as_object();
    const core::JsonObject::const_iterator found = object.find("upper");
    if (found == object.end()) return overlaybd::ParseUpperConfig(core::Json());
    return overlaybd::ParseUpperConfig(found->second);
}

/// Rust `tokio::process::Command` + `tokio::time::timeout` + `kill_on_drop`,
/// collapsed into one blocking spawn.
///
/// Hand-rolled rather than reusing `sandbox::Executor`: that lives above this
/// layer and would invert the dependency, it has no truncation marker, and its
/// timeout rounds up to whole seconds. `storage/overlaybd/tools_oci.cc` spawns
/// its tools the same way for the same reason.
struct ResizeChildResult {
    std::string stdout_text;
    std::string stderr_text;
    int wait_status;
    bool timed_out;

    ResizeChildResult() : wait_status(0), timed_out(false) {}
};

void CloseIfOpen(int* fd) {
    if (*fd >= 0) {
        ::close(*fd);
        *fd = -1;
    }
}

int64_t NowMs() {
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

/// Reads both pipes to EOF under a deadline, bounding each the way Rust's
/// `read_bounded_output` does.
///
/// Both pipes are drained concurrently through `poll`. Reading one to EOF
/// first would deadlock whenever the tool fills the other pipe's buffer, which
/// `overlaybd-resize` does on a verbose failure — exactly the case the output
/// is being captured for.
core::Expected<ResizeChildResult, std::string> RunResizeChild(
    const std::vector<std::string>& argv, const std::string& cwd,
    const core::Optional<std::string>& ld_library_path, uint64_t timeout_secs) {
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (::pipe(out_pipe) != 0) {
        return core::make_unexpected(std::string("pipe for overlaybd-resize stdout: ") +
                                     std::strerror(errno));
    }
    if (::pipe(err_pipe) != 0) {
        const int saved = errno;
        CloseIfOpen(&out_pipe[0]);
        CloseIfOpen(&out_pipe[1]);
        return core::make_unexpected(std::string("pipe for overlaybd-resize stderr: ") +
                                     std::strerror(saved));
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        const int saved = errno;
        CloseIfOpen(&out_pipe[0]);
        CloseIfOpen(&out_pipe[1]);
        CloseIfOpen(&err_pipe[0]);
        CloseIfOpen(&err_pipe[1]);
        return core::make_unexpected(std::string("execute overlaybd-resize ") + argv[0] + ": " +
                                     std::strerror(saved));
    }
    if (pid == 0) {
        ::close(out_pipe[0]);
        ::close(err_pipe[0]);
        if (::dup2(out_pipe[1], STDOUT_FILENO) < 0) ::_exit(127);
        if (::dup2(err_pipe[1], STDERR_FILENO) < 0) ::_exit(127);
        ::close(out_pipe[1]);
        ::close(err_pipe[1]);
        if (::chdir(cwd.c_str()) != 0) ::_exit(127);
        if (ld_library_path.has_value()) {
            ::setenv("LD_LIBRARY_PATH", ld_library_path->c_str(), 1);
        }
        std::vector<char*> raw;
        raw.reserve(argv.size() + 1);
        for (std::size_t i = 0; i < argv.size(); ++i) {
            raw.push_back(const_cast<char*>(argv[i].c_str()));
        }
        raw.push_back(0);
        ::execvp(raw[0], &raw[0]);
        ::_exit(127);
    }

    CloseIfOpen(&out_pipe[1]);
    CloseIfOpen(&err_pipe[1]);

    const std::size_t retained_limit =
        kResizeOutputLimit - std::strlen(kResizeOutputTruncatedMarker);
    std::string out_text;
    std::string err_text;
    bool out_truncated = false;
    bool err_truncated = false;
    const int64_t deadline = NowMs() + static_cast<int64_t>(timeout_secs) * 1000;
    ResizeChildResult result;

    while (out_pipe[0] >= 0 || err_pipe[0] >= 0) {
        const int64_t remaining_ms = deadline - NowMs();
        if (remaining_ms <= 0) {
            result.timed_out = true;
            break;
        }
        struct pollfd fds[2];
        int nfds = 0;
        const int out_slot = out_pipe[0] >= 0 ? nfds++ : -1;
        if (out_slot >= 0) {
            fds[out_slot].fd = out_pipe[0];
            fds[out_slot].events = POLLIN;
            fds[out_slot].revents = 0;
        }
        const int err_slot = err_pipe[0] >= 0 ? nfds++ : -1;
        if (err_slot >= 0) {
            fds[err_slot].fd = err_pipe[0];
            fds[err_slot].events = POLLIN;
            fds[err_slot].revents = 0;
        }

        const int ready = ::poll(fds, nfds, static_cast<int>(std::min<int64_t>(remaining_ms, 200)));
        if (ready < 0) {
            if (errno == EINTR) continue;
            const int saved = errno;
            CloseIfOpen(&out_pipe[0]);
            CloseIfOpen(&err_pipe[0]);
            ::kill(pid, SIGKILL);
            int discarded = 0;
            ::waitpid(pid, &discarded, 0);
            return core::make_unexpected(std::string("poll overlaybd-resize output: ") +
                                         std::strerror(saved));
        }

        for (int slot = 0; slot < nfds; ++slot) {
            if (fds[slot].revents == 0) continue;
            const bool is_out = slot == out_slot;
            int* fd = is_out ? &out_pipe[0] : &err_pipe[0];
            std::string* sink = is_out ? &out_text : &err_text;
            bool* truncated = is_out ? &out_truncated : &err_truncated;

            char chunk[8192];
            const ssize_t read_bytes = ::read(*fd, chunk, sizeof(chunk));
            if (read_bytes > 0) {
                const std::size_t remaining =
                    sink->size() >= retained_limit ? 0 : retained_limit - sink->size();
                if (remaining > 0) {
                    sink->append(chunk, std::min<std::size_t>(
                                            static_cast<std::size_t>(read_bytes), remaining));
                }
                // `>` and not `>=`: a read that exactly fills the remaining
                // room lost nothing.
                if (static_cast<std::size_t>(read_bytes) > remaining) *truncated = true;
            } else if (read_bytes == 0) {
                CloseIfOpen(fd);
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                CloseIfOpen(fd);
            }
        }
    }

    CloseIfOpen(&out_pipe[0]);
    CloseIfOpen(&err_pipe[0]);
    if (out_truncated) out_text += kResizeOutputTruncatedMarker;
    if (err_truncated) err_text += kResizeOutputTruncatedMarker;

    if (result.timed_out) {
        // Killed *and* reaped before reporting: returning early would leave a
        // zombie holding the resize cacheDir lock the next caller waits on.
        ::kill(pid, SIGKILL);
        int discarded = 0;
        while (::waitpid(pid, &discarded, 0) < 0 && errno == EINTR) {
        }
        result.stdout_text = out_text;
        result.stderr_text = err_text;
        return result;
    }

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return core::make_unexpected(std::string("wait for overlaybd-resize: ") +
                                         std::strerror(errno));
        }
    }
    result.stdout_text = out_text;
    result.stderr_text = err_text;
    result.wait_status = status;
    return result;
}

}  // namespace

core::Expected<core::Unit, std::string> RunResizeTool(const ResizeToolSpec& tool,
                                                      const std::string& image_config,
                                                      const std::string& resize_global_config,
                                                      uint64_t target_size) {
    const core::Optional<std::string> runtime_dir = core::fs::Parent(image_config);
    if (!runtime_dir.has_value()) {
        return core::make_unexpected(
            std::string("overlaybd runtime image config has no parent directory"));
    }

    std::vector<std::string> argv;
    argv.push_back(tool.binary);
    argv.push_back("--config");
    argv.push_back(image_config);
    argv.push_back("--size");
    // GiB, not bytes: that is the unit `overlaybd-resize` takes. The caller
    // already rejected anything not GiB-aligned, so this division is exact.
    argv.push_back(ToDecimal(target_size / kGib));
    argv.push_back("--service_config_path");
    argv.push_back(resize_global_config);

    core::Optional<std::string> ld_library_path;
    if (tool.lib_dir.has_value()) {
        const char* inherited_raw = ::getenv("LD_LIBRARY_PATH");
        core::Optional<std::string> inherited;
        if (inherited_raw != 0) inherited = std::string(inherited_raw);
        const core::Expected<std::string, std::string> joined =
            OverlaybdLibraryPath(*tool.lib_dir, inherited);
        if (!joined.ok()) return core::make_unexpected(joined.error());
        ld_library_path = joined.value();
    }

    const core::Expected<ResizeChildResult, std::string> ran =
        RunResizeChild(argv, *runtime_dir, ld_library_path, tool.timeout_secs);
    if (!ran.ok()) return core::make_unexpected(ran.error());

    if (ran.value().timed_out) {
        std::ostringstream o;
        o << "overlaybd-resize timed out after " << tool.timeout_secs << " seconds";
        return core::make_unexpected(o.str());
    }
    if (!(WIFEXITED(ran.value().wait_status) && WEXITSTATUS(ran.value().wait_status) == 0)) {
        std::ostringstream o;
        o << "overlaybd-resize failed (status=" << DescribeWaitStatus(ran.value().wait_status)
          << "): stdout=" << Trimmed(ran.value().stdout_text)
          << " stderr=" << Trimmed(ran.value().stderr_text);
        return core::make_unexpected(o.str());
    }
    return core::Unit();
}

namespace {

/// Rust `reopen_overlaybd_virtual_size`.
///
/// Distinct from `ResolveOverlaybdVirtualSize` and must stay so: that one
/// short-circuits on a lower's trailer, which after a resize still reports the
/// *pre*-resize size. Only a full reopen sees the new upper.
core::Expected<uint64_t, std::string> ReopenOverlaybdVirtualSize(
    ImageServiceCache* image_service_cache, const std::string& image_config,
    const std::string& global_config) {
    const core::Expected<overlaybd::ImageConfig, std::string> image_cfg =
        overlaybd::LoadImageConfig(image_config);
    if (!image_cfg.ok()) {
        return core::make_unexpected("load overlaybd image config '" + image_config + "': " +
                                     image_cfg.error());
    }
    const core::Expected<overlaybd::ImageService, std::string> image_service =
        image_service_cache->GetOrCreate(global_config);
    if (!image_service.ok()) {
        return core::make_unexpected("load overlaybd global config '" + global_config + "': " +
                                     image_service.error());
    }
    const core::Expected<std::shared_ptr<overlaybd::ImageFile>, std::string> image =
        overlaybd::ImageFile::Open(image_cfg.value(), image_service.value(),
                                   core::Optional<std::string>(CanonicalizeOrSelf(image_config)));
    if (!image.ok()) {
        return core::make_unexpected("open overlaybd image config '" + image_config + "': " +
                                     image.error());
    }
    const core::Expected<uint64_t, std::string> size = image.value()->Size();
    if (!size.ok()) {
        return core::make_unexpected("read overlaybd virtual size from '" + image_config + "': " +
                                     size.error());
    }
    return size.value();
}

/// Rust `verify_resized_runtime`.
core::Expected<core::Unit, std::string> VerifyResizedRuntime(
    ImageServiceCache* image_service_cache, const std::string& image_config,
    const std::string& global_config, const std::string& runtime_dir,
    const ResolvedUpperMode& upper_mode, uint64_t expected_size) {
    if (upper_mode.kind == ResolvedUpperMode::kCreate) {
        // Checked before the reopen: the header pair is what the tool was
        // supposed to rewrite, and a mismatch here names the resize as the
        // culprit, whereas a reopen failure would only say the image is
        // unusable.
        const core::Expected<core::Unit, std::string> validated =
            overlaybd::ValidateRwHeaderPairPaths(
                core::fs::Join(runtime_dir, kRuntimeUpperDataFile),
                RuntimeUpperIndexPath(runtime_dir, upper_mode.create_mode), expected_size,
                RwLayoutFromUpperMode(upper_mode.create_mode));
        if (!validated.ok()) {
            return core::make_unexpected("validate resized OverlayBD RW header pair: " +
                                         validated.error());
        }
    }

    const core::Expected<uint64_t, std::string> reopened =
        ReopenOverlaybdVirtualSize(image_service_cache, image_config, global_config);
    if (!reopened.ok()) {
        return core::make_unexpected("reopen resized overlaybd runtime: " + reopened.error());
    }
    if (reopened.value() != expected_size) {
        std::ostringstream o;
        o << "resized overlaybd runtime size mismatch: expected " << expected_size << ", got "
          << reopened.value();
        return core::make_unexpected(o.str());
    }
    return core::Unit();
}

/// Rust `materialize_runtime_contents` — everything inside the claimed dir.
///
/// Split from `MaterializeOverlaybdRuntime` so the claim/cleanup pairing has
/// exactly one place to live: every failure below returns, and the caller
/// removes the directory unconditionally.
core::Expected<std::pair<std::string, uint64_t>, RuntimeError> MaterializeRuntimeContents(
    const MaterializeOverlaybdRuntimeRequest& request) {
    uint64_t base_virtual_size = 0;
    if (request.known_source_virtual_size.has_value()) {
        base_virtual_size = *request.known_source_virtual_size;
    } else {
        const core::Expected<uint64_t, std::string> resolved = ResolveOverlaybdVirtualSize(
            request.image_service_cache, request.source_image_config, request.global_config);
        if (!resolved.ok()) {
            return core::make_unexpected(
                InternalError("resolve overlaybd base virtual size from " +
                              request.source_image_config + ": " + resolved.error()));
        }
        base_virtual_size = resolved.value();
    }
    if (base_virtual_size == 0) {
        return core::make_unexpected(
            InternalError("overlaybd base virtual size must be non-zero"));
    }

    const core::Expected<uint64_t, std::string> validated = ValidateRequestedVirtualSize(
        request.requested_virtual_size, base_virtual_size, request.allow_shrink);
    if (!validated.ok()) return core::make_unexpected(InvalidRequest(validated.error()));
    const uint64_t actual_virtual_size = validated.value();

    const core::Expected<ResolvedUpperMode, std::string> upper_mode = ResolveUpperMode(
        request.source_image_config, request.read_only, request.runtime_upper_mode);
    if (!upper_mode.ok()) return core::make_unexpected(InternalError(upper_mode.error()));

    // Resizing is confined to an upper the daemon just created. `Existing`
    // points at the source's own upper and `Absent` at none at all; growing
    // either would rewrite storage shared with other devices.
    if (actual_virtual_size != base_virtual_size &&
        upper_mode.value().kind != ResolvedUpperMode::kCreate) {
        return core::make_unexpected(
            InvalidRequest("resizing requires a fresh writable OverlayBD upper"));
    }

    if (upper_mode.value().kind == ResolvedUpperMode::kCreate) {
        const overlaybd::UpperMode mode = upper_mode.value().create_mode;
        // Laid down at the *base* size. The resize tool grows it afterwards;
        // creating it at the target size would leave the tool with nothing to
        // do and the lowers describing a smaller device.
        const core::Expected<core::Unit, std::string> prepared = overlaybd::PrepareRuntimeUpper(
            core::fs::Join(request.runtime_dir, kRuntimeUpperDataFile),
            RuntimeUpperIndexPath(request.runtime_dir, mode), base_virtual_size, mode);
        if (!prepared.ok()) {
            return core::make_unexpected(InternalError("prepare overlaybd runtime upper in " +
                                                       request.runtime_dir + ": " +
                                                       prepared.error()));
        }
    }

    const core::Expected<core::Json, std::string> image = MaterializeOverlaybdImageConfig(
        request.source_image_config, request.runtime_dir, upper_mode.value());
    if (!image.ok()) return core::make_unexpected(InternalError(image.error()));

    const std::string runtime_image_config_path =
        core::fs::Join(request.runtime_dir, kRuntimeImageConfigFile);
    // Rust writes `to_vec_pretty`; this is compact. The consumers are the
    // overlaybd loader and `overlaybd-resize`, both real JSON parsers, so the
    // whitespace is not part of the contract -- only the keys are.
    const core::Expected<core::Unit, std::string> written =
        core::fs::Write(runtime_image_config_path, image.value().ToString());
    if (!written.ok()) {
        return core::make_unexpected(InternalError("write overlaybd runtime image config " +
                                                   runtime_image_config_path + ": " +
                                                   written.error()));
    }

    if (actual_virtual_size != base_virtual_size) {
        if (request.resize_tool == 0) {
            return core::make_unexpected(
                InvalidRequest("overlaybd resize tool is required when virtual size changes"));
        }
        {
            // Serialized daemon-wide: every `overlaybd-resize` child shares the
            // one isolated resize cacheDir. Scoped so the permit is released on
            // every exit path, and released *before* verification, which
            // reopens through this process's own ImageService and never touches
            // the resize cacheDir -- holding it across the reopen would
            // serialize work that does not contend.
            std::unique_lock<std::mutex> permit;
            if (request.resize_permit != 0) {
                permit = std::unique_lock<std::mutex>(*request.resize_permit);
            }
            const core::Expected<core::Unit, std::string> resized =
                RunResizeTool(*request.resize_tool, runtime_image_config_path,
                              request.resize_global_config, actual_virtual_size);
            if (!resized.ok()) return core::make_unexpected(InternalError(resized.error()));
        }
        const core::Expected<core::Unit, std::string> verified = VerifyResizedRuntime(
            request.image_service_cache, runtime_image_config_path, request.global_config,
            request.runtime_dir, upper_mode.value(), actual_virtual_size);
        if (!verified.ok()) return core::make_unexpected(InternalError(verified.error()));
    }

    return std::make_pair(runtime_image_config_path, actual_virtual_size);
}

}  // namespace

RuntimeError InvalidRequest(const std::string& message) {
    RuntimeError error;
    error.invalid_request = true;
    error.message = message;
    return error;
}

RuntimeError InternalError(const std::string& message) {
    RuntimeError error;
    error.invalid_request = false;
    error.message = message;
    return error;
}

// ---------------------------------------------------------------------------
// ImageServiceCache
// ---------------------------------------------------------------------------

ImageServiceCache::ImageServiceCache(const core::Optional<std::string>& p2p_publish_url)
    : p2p_publish_url_(p2p_publish_url) {}

core::Expected<overlaybd::ImageService, std::string> ImageServiceCache::GetOrCreate(
    const std::string& global_config) {
    const std::string canonical = CanonicalizeOrSelf(global_config);

    std::lock_guard<std::mutex> guard(lock_);
    const std::map<std::string, overlaybd::ImageService>::const_iterator found =
        services_.find(canonical);
    if (found != services_.end()) return found->second;

    const core::Expected<overlaybd::ImageService, std::string> service =
        overlaybd::ImageService::FromConfigPathWithP2pPublishUrl(canonical, p2p_publish_url_);
    if (!service.ok()) {
        return core::make_unexpected("create ImageService from global config: " + canonical +
                                     ": " + service.error());
    }
    services_.insert(std::make_pair(canonical, service.value()));
    return service.value();
}

// ---------------------------------------------------------------------------
// Upper resolution
// ---------------------------------------------------------------------------

namespace {
/// `core::Optional` has no `operator==`, and adding one would change a widely
/// shared type for the benefit of one comparison. Absent compares equal only
/// to absent, which is the distinction the upper comparison exists to make: an
/// unset index is not an index that happens to be "".
bool SameOptional(const core::Optional<std::string>& a, const core::Optional<std::string>& b) {
    if (a.has_value() != b.has_value()) return false;
    return !a.has_value() || *a == *b;
}
}  // namespace

bool ResolvedSourceUpper::operator==(const ResolvedSourceUpper& o) const {
    return data_path == o.data_path && SameOptional(index_path, o.index_path) &&
           SameOptional(target_path, o.target_path) &&
           SameOptional(gzip_index_path, o.gzip_index_path) && writable_mode == o.writable_mode;
}

ResolvedUpperMode ResolvedUpperMode::Absent() { return ResolvedUpperMode(); }

ResolvedUpperMode ResolvedUpperMode::Create(overlaybd::UpperMode mode) {
    ResolvedUpperMode resolved;
    resolved.kind = kCreate;
    resolved.create_mode = mode;
    return resolved;
}

ResolvedUpperMode ResolvedUpperMode::Existing(const ResolvedSourceUpper& upper) {
    ResolvedUpperMode resolved;
    resolved.kind = kExisting;
    resolved.existing = upper;
    return resolved;
}

core::Expected<ResolvedUpperMode, std::string> ResolveUpperMode(
    const std::string& image_config_path, bool read_only,
    overlaybd::UpperMode runtime_upper_mode) {
    // Checked first: a read-only device gets no upper at all, so whether the
    // source declares one is irrelevant and reading it would only be a way to
    // fail.
    if (read_only) return ResolvedUpperMode::Absent();

    const core::Expected<core::Optional<ResolvedSourceUpper>, std::string> existing =
        ResolveSourceUpper(image_config_path);
    if (!existing.ok()) return core::make_unexpected(existing.error());
    if (existing.value().has_value()) {
        return ResolvedUpperMode::Existing(*existing.value());
    }
    return ResolvedUpperMode::Create(runtime_upper_mode);
}

core::Expected<core::Optional<ResolvedSourceUpper>, std::string> ResolveSourceUpper(
    const std::string& image_config_path) {
    const core::Expected<std::string, std::string> raw =
        core::fs::ReadToString(image_config_path);
    if (!raw.ok()) {
        return core::make_unexpected("read overlaybd image config '" + image_config_path + "': " +
                                     raw.error());
    }
    const core::Expected<core::Json, core::AnyError> value = core::Json::Parse(raw.value());
    if (!value.ok()) {
        return core::make_unexpected("parse overlaybd image config '" + image_config_path +
                                     "': " + value.error().chain());
    }
    const core::Optional<std::string> base = core::fs::Parent(image_config_path);
    const std::string base_dir = base.has_value() ? *base : std::string(".");

    const core::Expected<overlaybd::UpperConfig, std::string> upper = UpperFromJson(value.value());
    if (!upper.ok()) {
        return core::make_unexpected("parse overlaybd upper '" + image_config_path + "': " +
                                     upper.error());
    }
    const core::Expected<bool, std::string> writable =
        overlaybd::ValidateUpperConfig(upper.value());
    if (!writable.ok()) return core::make_unexpected(writable.error());
    // Not writable means "no usable upper here", not an error: a read-only
    // source image is the normal case for a rootfs template.
    if (!writable.value()) return core::Optional<ResolvedSourceUpper>();

    ResolvedSourceUpper resolved;
    const core::Expected<std::string, std::string> data =
        overlaybd::ResolveConfigPath(base_dir, upper.value().data);
    if (!data.ok()) return core::make_unexpected(data.error());
    resolved.data_path = data.value();

    // Each optional field is resolved only when non-empty, because empty
    // means "unset" and resolving it would yield the config's own directory --
    // a plausible-looking path to a file that is not supposed to exist.
    const std::string* const raw_optionals[3] = {&upper.value().index, &upper.value().target,
                                                 &upper.value().gzip_index};
    core::Optional<std::string>* const slots[3] = {&resolved.index_path, &resolved.target_path,
                                                   &resolved.gzip_index_path};
    for (int i = 0; i < 3; ++i) {
        if (raw_optionals[i]->empty()) continue;
        const core::Expected<std::string, std::string> path =
            overlaybd::ResolveConfigPath(base_dir, *raw_optionals[i]);
        if (!path.ok()) return core::make_unexpected(path.error());
        *slots[i] = path.value();
    }
    resolved.writable_mode = upper.value().WritableMode();
    return core::Optional<ResolvedSourceUpper>(resolved);
}

// ---------------------------------------------------------------------------
// Config materialization
// ---------------------------------------------------------------------------

core::Expected<core::Json, std::string> MaterializeOverlaybdImageConfig(
    const std::string& image_config_path, const std::string& runtime_dir,
    const ResolvedUpperMode& upper_mode) {
    const core::Expected<std::string, std::string> raw =
        core::fs::ReadToString(image_config_path);
    if (!raw.ok()) {
        return core::make_unexpected("read overlaybd image config failed: " + image_config_path +
                                     ": " + raw.error());
    }
    core::Expected<core::Json, core::AnyError> parsed = core::Json::Parse(raw.value());
    if (!parsed.ok()) {
        return core::make_unexpected("parse overlaybd image config failed: " + image_config_path +
                                     ": " + parsed.error().chain());
    }
    core::Json value = parsed.value();
    if (value.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("overlaybd image config should be an object"));
    }
    const core::Optional<std::string> base = core::fs::Parent(image_config_path);
    const std::string base_dir = base.has_value() ? *base : std::string(".");

    const core::Expected<core::Unit, std::string> rewritten =
        overlaybd::RewriteOverlaybdLowerPaths(&value, base_dir, runtime_dir);
    if (!rewritten.ok()) return core::make_unexpected(rewritten.error());

    // Named, not `core::Json upper(core::JsonObject())`: that spelling parses
    // as a function declaration.
    const core::JsonObject empty_upper;
    core::Json upper(empty_upper);
    if (upper_mode.kind == ResolvedUpperMode::kCreate) {
        const overlaybd::UpperMode mode = upper_mode.create_mode;
        upper["mode"] = core::Json(std::string(overlaybd::UpperModeToString(mode)));
        // A sparse upper has no index file, so the key must be empty rather
        // than naming one: the loader would try to open it.
        upper["index"] = core::Json(mode == overlaybd::UpperMode::Sparse
                                        ? std::string()
                                        : std::string("./") + kRuntimeUpperIndexFile);
        upper["data"] = core::Json(std::string("./") + kRuntimeUpperDataFile);
        upper["target"] = core::Json(std::string());
        upper["gzipIndex"] = core::Json(std::string());
    } else if (upper_mode.kind == ResolvedUpperMode::kExisting) {
        const ResolvedSourceUpper& source = upper_mode.existing;
        upper["mode"] =
            core::Json(std::string(overlaybd::UpperModeToString(source.writable_mode)));
        // Rewritten relative to `runtime_dir`, because the generated config
        // lives there and overlaybd resolves relative paths against the config.
        const core::Expected<std::string, std::string> index =
            RelativeOrEmpty(runtime_dir, source.index_path);
        if (!index.ok()) return core::make_unexpected(index.error());
        upper["index"] = core::Json(index.value());

        const core::Expected<std::string, std::string> data =
            overlaybd::RelativePath(runtime_dir, source.data_path);
        if (!data.ok()) return core::make_unexpected(data.error());
        upper["data"] = core::Json(data.value());

        const core::Expected<std::string, std::string> target =
            RelativeOrEmpty(runtime_dir, source.target_path);
        if (!target.ok()) return core::make_unexpected(target.error());
        upper["target"] = core::Json(target.value());

        const core::Expected<std::string, std::string> gzip_index =
            RelativeOrEmpty(runtime_dir, source.gzip_index_path);
        if (!gzip_index.ok()) return core::make_unexpected(gzip_index.error());
        upper["gzipIndex"] = core::Json(gzip_index.value());
    }

    // Assigned, not merged: whatever the source declared is replaced outright.
    // Keeping any of it would leave the runtime config pointing at the
    // source's upper alongside the daemon's own.
    value["upper"] = upper;
    value["resultFile"] = core::Json(std::string("./") + kRuntimeResultFile);
    return value;
}

// ---------------------------------------------------------------------------
// Size resolution and validation
// ---------------------------------------------------------------------------

core::Expected<uint64_t, std::string> ResolveOverlaybdVirtualSize(
    ImageServiceCache* image_service_cache, const std::string& image_config,
    const std::string& global_config) {
    const core::Expected<overlaybd::ImageConfig, std::string> image_cfg =
        overlaybd::LoadImageConfig(image_config);
    if (!image_cfg.ok()) {
        return core::make_unexpected("load overlaybd image config '" + image_config + "': " +
                                     image_cfg.error());
    }

    // Topmost local lower first (reverse order): it is the one whose trailer
    // describes the whole stack's logical size. A remote lower is skipped
    // rather than ending the search, but a local lower whose trailer will not
    // read *does* end it -- the stack is then suspect and the authoritative
    // answer has to come from opening the image.
    for (std::size_t i = image_cfg.value().lowers.size(); i > 0; --i) {
        const core::Optional<std::string> local_path =
            overlaybd::ResolveLocalLayerPath(image_cfg.value().lowers[i - 1]);
        if (!local_path.has_value()) continue;
        const core::Expected<uint64_t, std::string> size =
            overlaybd::ReadOverlaybdLayerVirtualSize(*local_path);
        if (size.ok()) return size.value();
        AGENTENV_DEBUG("fall back to opening overlaybd image for virtual_size: " << *local_path
                                                                                << ": "
                                                                                << size.error());
        break;
    }

    const core::Expected<overlaybd::ImageService, std::string> image_service =
        image_service_cache->GetOrCreate(global_config);
    if (!image_service.ok()) {
        return core::make_unexpected("load overlaybd global config '" + global_config + "': " +
                                     image_service.error());
    }
    const core::Expected<std::shared_ptr<overlaybd::ImageFile>, std::string> image =
        overlaybd::ImageFile::Open(image_cfg.value(), image_service.value(),
                                   core::Optional<std::string>(CanonicalizeOrSelf(image_config)));
    if (!image.ok()) {
        return core::make_unexpected("open overlaybd image config '" + image_config + "': " +
                                     image.error());
    }
    const core::Expected<uint64_t, std::string> size = image.value()->Size();
    if (!size.ok()) {
        return core::make_unexpected("read overlaybd virtual size from '" + image_config + "': " +
                                     size.error());
    }
    return size.value();
}

core::Expected<uint64_t, std::string> ValidateRequestedVirtualSize(
    const core::Optional<uint64_t>& requested_virtual_size, uint64_t base_virtual_size,
    bool allow_shrink) {
    if (!requested_virtual_size.has_value()) return base_virtual_size;
    const uint64_t requested = *requested_virtual_size;
    // Equal to the base short-circuits *before* the alignment check: a base
    // image that is itself not GiB-aligned must still be usable at its own
    // size, and only a real resize has to satisfy the tool's GiB granularity.
    if (requested == base_virtual_size) return base_virtual_size;

    if (requested == 0) {
        return core::make_unexpected(
            std::string("requested overlaybd runtime virtual size must be non-zero"));
    }
    if (requested % kGib != 0) {
        std::ostringstream o;
        o << "requested overlaybd runtime virtual size " << requested << " must be GiB-aligned";
        return core::make_unexpected(o.str());
    }
    if (requested < base_virtual_size && !allow_shrink) {
        std::ostringstream o;
        o << "requested overlaybd runtime virtual size " << requested
          << " is smaller than base virtual size " << base_virtual_size
          << "; shrinking is disabled";
        return core::make_unexpected(o.str());
    }
    return requested;
}

core::Optional<std::string> RuntimeUpperIndexPath(const std::string& runtime_dir,
                                                  overlaybd::UpperMode mode) {
    if (mode == overlaybd::UpperMode::LogStructured ||
        mode == overlaybd::UpperMode::HybridLogStructured) {
        return core::Optional<std::string>(core::fs::Join(runtime_dir, kRuntimeUpperIndexFile));
    }
    return core::Optional<std::string>();
}

overlaybd::RwLayout RwLayoutFromUpperMode(overlaybd::UpperMode mode) {
    switch (mode) {
        case overlaybd::UpperMode::Sparse:
            return overlaybd::kRwSparse;
        case overlaybd::UpperMode::HybridLogStructured:
            return overlaybd::kRwHybridLogStructured;
        case overlaybd::UpperMode::LogStructured:
        default:
            return overlaybd::kRwLogStructured;
    }
}

core::Expected<std::string, std::string> OverlaybdLibraryPath(
    const std::string& lib_dir, const core::Optional<std::string>& inherited) {
    std::vector<std::string> paths;
    paths.push_back(lib_dir);
    if (inherited.has_value() && !inherited->empty()) {
        // `split_paths` then re-join rather than concatenating: it drops the
        // platform quoting rules Rust applies and, more importantly, lets the
        // ':' check below see every component individually.
        std::size_t start = 0;
        const std::string& value = *inherited;
        for (;;) {
            const std::size_t sep = value.find(':', start);
            if (sep == std::string::npos) {
                paths.push_back(value.substr(start));
                break;
            }
            paths.push_back(value.substr(start, sep - start));
            start = sep + 1;
        }
    }

    std::string joined;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        // Rust's `join_paths` returns an error for a component containing the
        // separator, because the result would silently become two entries.
        if (paths[i].find(':') != std::string::npos) {
            return core::make_unexpected(
                std::string("construct overlaybd-resize LD_LIBRARY_PATH: path contains ':': ") +
                paths[i]);
        }
        if (i != 0) joined += ":";
        joined += paths[i];
    }
    return joined;
}

core::Expected<std::string, std::string> ReadBoundedOutput(int fd) {
    const std::size_t retained_limit =
        kResizeOutputLimit - std::strlen(kResizeOutputTruncatedMarker);
    std::string retained;
    bool truncated = false;
    char chunk[8192];
    for (;;) {
        const ssize_t read_bytes = ::read(fd, chunk, sizeof(chunk));
        if (read_bytes == 0) break;
        if (read_bytes < 0) {
            if (errno == EINTR) continue;
            return core::make_unexpected(std::string("read bounded output: ") +
                                         std::strerror(errno));
        }
        const std::size_t remaining =
            retained.size() >= retained_limit ? 0 : retained_limit - retained.size();
        if (remaining > 0) {
            retained.append(chunk,
                            std::min<std::size_t>(static_cast<std::size_t>(read_bytes), remaining));
        }
        if (static_cast<std::size_t>(read_bytes) > remaining) truncated = true;
    }
    if (truncated) retained += kResizeOutputTruncatedMarker;
    return retained;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

void MaterializedOverlaybdRuntime::Rollback() {
    if (claimed_runtime_dir_.empty()) return;
    const std::string path = claimed_runtime_dir_;
    claimed_runtime_dir_.clear();
    CleanupRuntimeDir(path);
}

core::Expected<MaterializedOverlaybdRuntime, RuntimeError> MaterializeOverlaybdRuntime(
    const MaterializeOverlaybdRuntimeRequest& request) {
    if (request.image_service_cache == 0) {
        return core::make_unexpected(
            InternalError("materialize overlaybd runtime requires an ImageServiceCache"));
    }
    const core::Expected<core::Unit, std::string> claimed = ClaimRuntimeDir(request.runtime_dir);
    if (!claimed.ok()) return core::make_unexpected(InternalError(claimed.error()));

    const core::Expected<std::pair<std::string, uint64_t>, RuntimeError> contents =
        MaterializeRuntimeContents(request);
    if (!contents.ok()) {
        // Removed here and not by the caller: the caller never saw the claim,
        // and leaving the directory behind would make every retry at this path
        // fail with "already exists".
        CleanupRuntimeDir(request.runtime_dir);
        return core::make_unexpected(contents.error());
    }

    MaterializedOverlaybdRuntime runtime;
    runtime.runtime_image_config_path = contents.value().first;
    runtime.actual_virtual_size = contents.value().second;
    runtime.SetClaimedRuntimeDir(request.runtime_dir);
    return runtime;
}

}  // namespace ublk_daemon
}  // namespace storage
}  // namespace agentenv
