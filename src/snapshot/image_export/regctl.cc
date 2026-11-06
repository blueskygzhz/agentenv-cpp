// SPDX-License-Identifier: MIT
// Rust: src/snapshot/image_export/regctl.rs
#include "agentenv/snapshot/image_export/regctl.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <sstream>

#include "agentenv/core/logging.h"
#include "agentenv/core/process.h"
#include "agentenv/image/oci_image.h"

namespace agentenv {
namespace snapshot {
namespace image_export {

using repository::RepositoryError;
using repository::RepositoryResult;

const char* const kRegctlOciManifestContentType =
    "application/vnd.oci.image.manifest.v1+json";

namespace {

std::string Trim(const std::string& value) {
    std::string::size_type begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    std::string::size_type end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::string Join(const std::vector<std::string>& parts) {
    std::ostringstream out;
    for (std::vector<std::string>::size_type i = 0; i < parts.size(); ++i) {
        if (i != 0) out << " ";
        out << parts[i];
    }
    return out.str();
}

std::string ToDecimal(uint64_t value) {
    std::ostringstream out;
    out << value;
    return out.str();
}

void SleepMs(int64_t ms) {
    if (ms <= 0) return;
    struct timespec requested;
    requested.tv_sec = static_cast<time_t>(ms / 1000);
    requested.tv_nsec = static_cast<long>((ms % 1000) * 1000000L);
    while (::nanosleep(&requested, &requested) != 0 && errno == EINTR) {
    }
}

/// One `regctl` run whose payload is read straight from `path`.
///
/// The descriptor is handed over rather than the bytes: a manifest is small
/// but a layer blob is routinely gigabytes, and `blob put` shares this code.
/// Reopened per attempt, because a retry has to start from offset zero — a
/// reused descriptor would upload the tail of a half-sent blob and then fail
/// the digest check, which reads as corruption rather than as a retry.
RepositoryResult<core::Unit> PutFile(const std::string& binary,
                                     const std::vector<std::string>& args,
                                     const std::string& path,
                                     const std::string& description) {
    const core::Expected<core::Unit, image::ImageError> usable =
        image::EnsureRegctlBinary(binary);
    if (!usable.ok()) {
        return core::make_unexpected(
            RepositoryError::Backend("run regctl " + description, usable.error().reason));
    }

    int64_t backoff_ms = image::kRegctlRetryBaseDelayMs;
    std::string last_stderr;
    for (uint32_t attempt = 1; attempt <= image::kRegctlRetryAttempts; ++attempt) {
        const int payload = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (payload < 0) {
            return core::make_unexpected(RepositoryError::Backend(
                "open regctl " + description + " payload", std::strerror(errno)));
        }

        const image::RegctlCommand command =
            image::MakeRegctlCommand(binary, core::Optional<std::string>());
        std::vector<std::string> argv = command.argv;
        for (std::vector<std::string>::size_type i = 0; i < args.size(); ++i) {
            argv.push_back(args[i]);
        }
        core::process::RunOptions options;
        options.stdin_fd = payload;
        options.env = command.env;

        const core::Expected<core::process::Output, std::string> output =
            core::process::RunWithOptions(argv, options);
        ::close(payload);

        if (!output.ok()) {
            return core::make_unexpected(
                RepositoryError::Backend("run regctl " + description, output.error()));
        }
        if (output.value().success()) return core::Unit();

        last_stderr = Trim(output.value().stderr_text);
        // A 404 is terminal: the repository or the ref does not exist, and
        // asking again cannot create it. Retrying would spend the whole budget
        // before reporting a mistake the caller could have fixed immediately.
        if (image::RegctlStderrIsNotFound(last_stderr)) {
            return core::make_unexpected(RepositoryError::Backend(
                "regctl " + description + " hit a registry 404", last_stderr));
        }
        if (attempt < image::kRegctlRetryAttempts) {
            AGENTENV_WARN("regctl " << description << " failed; retrying attempt=" << attempt
                                    << " error=" << last_stderr);
            SleepMs(backoff_ms);
            backoff_ms *= 2;
        }
    }
    return core::make_unexpected(RepositoryError::Backend(
        "regctl " + description + " failed after " +
            ToDecimal(image::kRegctlRetryAttempts) + " attempts",
        last_stderr));
}

/// The outcome of one `blob get | blob put` attempt.
struct PipeAttempt {
    /// Set when the attempt is worth repeating; the transfer failed for a
    /// reason that may not recur.
    core::Optional<std::string> retryable_summary;
    /// Set when it is not: a 404 at either end, or a spawn/wait failure.
    core::Optional<RepositoryError> fatal;
};

/// Spawns both children joined by an OS pipe and waits for them.
///
/// The pipe connects the two **directly**: `get`'s stdout is `put`'s stdin, so
/// the kernel moves the bytes and they never enter this process at all. Rust
/// pumps them with `tokio::io::copy`, which streams in chunks rather than
/// buffering — the same memory guarantee — but needs a task to do it. There is
/// no async runtime here, and a read/write loop in this process would be
/// strictly worse than letting the kernel do it.
///
/// A consequence worth stating: with no pump there is no separate pump error.
/// A truncated transfer instead surfaces as `put` failing its digest check,
/// which is the same conclusion by a different route.
PipeAttempt SpawnBlobPipe(const std::string& binary, const std::string& source,
                          const std::string& target, const std::string& digest) {
    PipeAttempt attempt;

    // `O_CLOEXEC` is essential, not hygiene. These are the two ends of the
    // transfer pipe, and `put` is spawned *before* `get`: without CLOEXEC the
    // `put` child inherits the write end, so after `get` exits and this
    // process closes its own copies, the write end is still open inside `put`
    // itself. `put` then waits for an EOF that can never arrive and the
    // transfer hangs forever rather than failing.
    //
    // `dup2` in the child clears CLOEXEC on the descriptor it creates, so the
    // intended redirect survives `execvp` while these originals do not.
    int transfer[2];
    if (::pipe2(transfer, O_CLOEXEC) != 0) {
        attempt.fatal = core::Optional<RepositoryError>(
            RepositoryError::Backend("create regctl blob pipe", std::strerror(errno)));
        return attempt;
    }

    const image::RegctlCommand base =
        image::MakeRegctlCommand(binary, core::Optional<std::string>());

    std::vector<std::string> get_argv = base.argv;
    get_argv.push_back("blob");
    get_argv.push_back("get");
    get_argv.push_back(source);
    get_argv.push_back(digest);

    std::vector<std::string> put_argv = base.argv;
    put_argv.push_back("blob");
    put_argv.push_back("put");
    put_argv.push_back(target);
    put_argv.push_back("--digest");
    put_argv.push_back(digest);

    // `put` first, so the reader is already draining when the writer starts.
    // Started the other way round, a blob larger than the pipe buffer would
    // block `get` until `put` happened to be scheduled.
    core::process::RunOptions put_options;
    put_options.stdin_fd = transfer[0];
    put_options.env = base.env;
    const core::Expected<std::shared_ptr<core::process::Child>, std::string> put_child =
        core::process::Child::Spawn(put_argv, put_options);
    if (!put_child.ok()) {
        ::close(transfer[0]);
        ::close(transfer[1]);
        attempt.fatal = core::Optional<RepositoryError>(RepositoryError::Backend(
            "spawn regctl blob put " + target + " --digest " + digest, put_child.error()));
        return attempt;
    }

    core::process::RunOptions get_options;
    get_options.stdout_fd = transfer[1];
    get_options.env = base.env;
    const core::Expected<std::shared_ptr<core::process::Child>, std::string> get_child =
        core::process::Child::Spawn(get_argv, get_options);

    // Both ends closed in this process as soon as the children hold them.
    // Leaving the write end open here would keep `put` waiting for an EOF that
    // never arrives after `get` exits — a hang rather than an error.
    ::close(transfer[0]);
    ::close(transfer[1]);

    if (!get_child.ok()) {
        put_child.value()->StartKill();
        put_child.value()->Wait();
        attempt.fatal = core::Optional<RepositoryError>(RepositoryError::Backend(
            "spawn regctl blob get " + source + " " + digest, get_child.error()));
        return attempt;
    }

    const core::Expected<int, std::string> get_status = get_child.value()->Wait();
    const core::Expected<int, std::string> put_status = put_child.value()->Wait();
    if (!get_status.ok()) {
        attempt.fatal = core::Optional<RepositoryError>(
            RepositoryError::Backend("wait for regctl blob get", get_status.error()));
        return attempt;
    }
    if (!put_status.ok()) {
        attempt.fatal = core::Optional<RepositoryError>(
            RepositoryError::Backend("wait for regctl blob put", put_status.error()));
        return attempt;
    }

    const std::string get_stderr = Trim(get_child.value()->StderrBytes());
    const std::string put_stderr = Trim(put_child.value()->StderrBytes());
    const bool get_ok = get_status.value() == 0;
    const bool put_ok = put_status.value() == 0;
    if (get_ok && put_ok) return attempt;

    // The source blob being absent is reported as a missing artifact, not as a
    // backend fault: the caller asked to copy something that is not there, and
    // the `blob head` probe that preceded this proved the *target* lacked it.
    if (!get_ok && image::RegctlStderrIsNotFound(get_stderr)) {
        attempt.fatal = core::Optional<RepositoryError>(
            RepositoryError::ArtifactNotFound("external rootfs layer blob " + digest +
                                              " in source repository " + source + ": " +
                                              get_stderr));
        return attempt;
    }
    if (!put_ok && image::RegctlStderrIsNotFound(put_stderr)) {
        attempt.fatal = core::Optional<RepositoryError>(RepositoryError::Backend(
            "regctl blob put " + target + " --digest " + digest + " hit a registry 404",
            put_stderr));
        return attempt;
    }

    // Both summaries, because either child can be the one that failed and the
    // two failures read very differently.
    attempt.retryable_summary = core::Optional<std::string>(
        "blob get: " + get_stderr + "; blob put: " + put_stderr);
    return attempt;
}

}  // namespace

core::Expected<core::Optional<std::string>, RepositoryError> Regctl::ManifestHead(
    const std::string& image_ref) const {
    std::vector<std::string> args;
    args.push_back("manifest");
    args.push_back("head");
    args.push_back(image_ref);

    const core::Expected<core::process::Output, image::ImageError> output =
        image::RunRegctl(binary_, args, core::Optional<std::string>());
    if (output.ok()) {
        return core::Optional<std::string>(Trim(output.value().stdout_text));
    }
    // Absent, not failed: a first export has nothing at the ref, and that is
    // what tells the caller to upload instead of to compare.
    if (output.error().kind == image::ImageError::kNotFound) {
        return core::Optional<std::string>();
    }
    return core::make_unexpected(RepositoryError::Backend("regctl manifest head " + image_ref,
                                                          output.error().reason));
}

core::Expected<bool, RepositoryError> Regctl::BlobHead(const std::string& repository,
                                                       const std::string& digest) const {
    std::vector<std::string> args;
    args.push_back("blob");
    args.push_back("head");
    args.push_back(repository);
    args.push_back(digest);

    const core::Expected<core::process::Output, image::ImageError> output =
        image::RunRegctl(binary_, args, core::Optional<std::string>());
    if (output.ok()) return true;
    if (output.error().kind == image::ImageError::kNotFound) return false;
    return core::make_unexpected(RepositoryError::Backend(
        "regctl blob head " + repository + " " + digest, output.error().reason));
}

core::Expected<core::Unit, RepositoryError> Regctl::BlobPut(const std::string& repository,
                                                            const std::string& digest,
                                                            const std::string& path) const {
    std::vector<std::string> args;
    args.push_back("blob");
    args.push_back("put");
    args.push_back(repository);
    args.push_back("--digest");
    args.push_back(digest);
    return PutFile(binary_, args, path,
                   "blob put " + repository + " --digest " + digest);
}

core::Expected<core::Unit, RepositoryError> Regctl::BlobCopy(
    const std::string& source_repository, const std::string& target_repository,
    const std::string& digest) const {
    std::vector<std::string> args;
    args.push_back("blob");
    args.push_back("copy");
    args.push_back(source_repository);
    args.push_back(target_repository);
    args.push_back(digest);

    const core::Expected<core::process::Output, image::ImageError> output =
        image::RunRegctl(binary_, args, core::Optional<std::string>());
    if (output.ok()) return core::Unit();
    if (output.error().kind == image::ImageError::kNotFound) {
        return core::make_unexpected(
            RepositoryError::ArtifactNotFound("external rootfs layer blob " + digest +
                                              " in source repository " + source_repository +
                                              ": " + output.error().reason));
    }
    return core::make_unexpected(RepositoryError::Backend(
        "regctl blob copy " + source_repository + " " + target_repository + " " + digest,
        output.error().reason));
}

core::Expected<core::Unit, RepositoryError> Regctl::BlobPipe(
    const std::string& source_repository, const std::string& target_repository,
    const std::string& digest) const {
    const core::Expected<core::Unit, image::ImageError> usable =
        image::EnsureRegctlBinary(binary_);
    if (!usable.ok()) {
        return core::make_unexpected(RepositoryError::Backend(
            "run regctl blob pipe", usable.error().reason));
    }

    const std::string description = "blob get " + source_repository + " " + digest +
                                    " | blob put " + target_repository + " --digest " +
                                    digest;
    int64_t backoff_ms = image::kRegctlRetryBaseDelayMs;
    std::string last_failure;
    for (uint32_t attempt = 1; attempt <= image::kRegctlRetryAttempts; ++attempt) {
        const PipeAttempt result =
            SpawnBlobPipe(binary_, source_repository, target_repository, digest);
        if (result.fatal.has_value()) return core::make_unexpected(*result.fatal);
        if (!result.retryable_summary.has_value()) return core::Unit();

        last_failure = *result.retryable_summary;
        if (attempt < image::kRegctlRetryAttempts) {
            AGENTENV_WARN("regctl " << description << " failed; retrying attempt=" << attempt
                                    << " error=" << last_failure);
            SleepMs(backoff_ms);
            backoff_ms *= 2;
        }
    }
    return core::make_unexpected(RepositoryError::Backend(
        "regctl " + description + " failed after " +
            ToDecimal(image::kRegctlRetryAttempts) + " attempts",
        last_failure));
}

core::Expected<core::Unit, RepositoryError> Regctl::ManifestPut(
    const std::string& image_ref, const std::string& path) const {
    std::vector<std::string> args;
    args.push_back("manifest");
    args.push_back("put");
    // Explicit, because regctl otherwise guesses from the body: a guess of the
    // Docker v2 media type would rewrite the bytes and so change the digest
    // the caller just computed and compared against the registry.
    args.push_back("--content-type");
    args.push_back(kRegctlOciManifestContentType);
    args.push_back(image_ref);
    return PutFile(binary_, args, path, "manifest put " + image_ref);
}

}  // namespace image_export
}  // namespace snapshot
}  // namespace agentenv
