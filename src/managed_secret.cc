// SPDX-License-Identifier: MIT
// Rust: src/managed_secret.rs
#include "agentenv/managed_secret.h"

#include <cerrno>
#include <sstream>

namespace agentenv {
namespace managed_secret {
namespace {

using core::Optional;
using core::Unit;
namespace fs = core::fs;

/// Renders a mode the way Rust's `{mode:04o}` does.
std::string FormatMode(uint32_t mode) {
    std::ostringstream oss;
    oss << std::oct;
    oss.width(4);
    oss.fill('0');
    oss << mode;
    return oss.str();
}

/// Rust `managed_parent` — the secret's directory must be a dedicated one
/// named `secrets`, so tightening its mode cannot affect unrelated files.
core::Expected<std::string, std::string> ManagedParent(const std::string& path) {
    const Optional<std::string> parent = fs::Parent(path);
    if (!parent.has_value()) {
        return core::make_unexpected(std::string("managed secret path has no parent"));
    }
    const Optional<std::string> name = fs::FileName(*parent);
    if (!name.has_value() || *name != "secrets") {
        std::ostringstream oss;
        oss << "managed secret parent " << *parent
            << " must be a dedicated directory named secrets";
        return core::make_unexpected(oss.str());
    }
    return *parent;
}

/// Rust `validate_directory_identity` — must be a real directory owned by us,
/// and specifically not a symlink (checked with `lstat`, never `stat`).
core::Expected<fs::FileStat, std::string> ValidateDirectoryIdentity(const std::string& path) {
    const core::Expected<fs::FileStat, std::string> stat = fs::SymlinkStat(path);
    if (!stat.ok()) return core::make_unexpected(stat.error());

    if (stat.value().is_symlink || !stat.value().is_dir) {
        return core::make_unexpected(
            std::string("must be a directory and not a symbolic link"));
    }
    const uint32_t expected_uid = fs::EffectiveUid();
    if (stat.value().uid != expected_uid) {
        std::ostringstream oss;
        oss << "must be owned by uid " << expected_uid << ", found uid " << stat.value().uid;
        return core::make_unexpected(oss.str());
    }
    return stat.value();
}

/// Rust `validate_directory` — identity plus an exact 0700 mode.
core::Expected<Unit, std::string> ValidateDirectory(const std::string& path) {
    const core::Expected<fs::FileStat, std::string> stat = ValidateDirectoryIdentity(path);
    if (!stat.ok()) return core::make_unexpected(stat.error());

    if (stat.value().mode != 0700u) {
        std::ostringstream oss;
        oss << "must have permissions 0700, found " << FormatMode(stat.value().mode);
        return core::make_unexpected(oss.str());
    }
    return Unit();
}

std::string Context(const std::string& action, const std::string& path,
                    const std::string& cause) {
    std::ostringstream oss;
    oss << action << " " << path << ": " << cause;
    return oss.str();
}

}  // namespace

core::Expected<std::string, std::string> ReadFile(const std::string& path, int fd,
                                                  std::size_t max_len) {
    // Metadata comes from the open descriptor, not the path, so nothing can be
    // swapped underneath us between the check and the read.
    const core::Expected<fs::FileStat, std::string> stat = fs::StatFd(fd);
    if (!stat.ok()) {
        return core::make_unexpected(Context("inspect managed secret", path, stat.error()));
    }

    if (!stat.value().is_regular) {
        std::ostringstream oss;
        oss << "managed secret " << path << " must be a regular file";
        return core::make_unexpected(oss.str());
    }

    if (stat.value().mode != 0600u) {
        std::ostringstream oss;
        oss << "managed secret " << path << " must have permissions 0600, found "
            << FormatMode(stat.value().mode);
        return core::make_unexpected(oss.str());
    }

    const uint32_t expected_uid = fs::EffectiveUid();
    if (stat.value().uid != expected_uid) {
        std::ostringstream oss;
        oss << "managed secret " << path << " must be owned by uid " << expected_uid
            << ", found uid " << stat.value().uid;
        return core::make_unexpected(oss.str());
    }

    if (stat.value().size > static_cast<uint64_t>(max_len)) {
        std::ostringstream oss;
        oss << "managed secret " << path << " must be at most " << max_len << " bytes";
        return core::make_unexpected(oss.str());
    }

    // Read one byte past the limit: the size check above can race with a
    // concurrent append, so the content length is re-checked afterwards.
    if (max_len == static_cast<std::size_t>(-1)) {
        return core::make_unexpected(std::string("managed secret size limit is too large"));
    }
    const core::Expected<std::string, std::string> contents = fs::ReadFdToString(fd, max_len + 1);
    if (!contents.ok()) {
        return core::make_unexpected(Context("read managed secret", path, contents.error()));
    }
    if (contents.value().size() > max_len) {
        std::ostringstream oss;
        oss << "managed secret " << path << " must be at most " << max_len << " bytes";
        return core::make_unexpected(oss.str());
    }
    return contents.value();
}

core::Expected<Optional<std::string>, std::string> Read(const std::string& path,
                                                        std::size_t max_len) {
    const core::Expected<std::string, std::string> parent = ManagedParent(path);
    if (!parent.ok()) return core::make_unexpected(parent.error());

    // A missing secret is the normal first-run case, not a failure. The
    // NotFound classification happens inside the fs helper, where errno is
    // still trustworthy.
    core::Expected<Optional<fs::FileDescriptor>, std::string> opened =
        fs::OpenReadNoFollowOptional(path);
    if (!opened.ok()) {
        return core::make_unexpected(Context("open managed secret", path, opened.error()));
    }
    if (!opened.value().has_value()) return Optional<std::string>();

    const core::Expected<Unit, std::string> valid = ValidateDirectory(parent.value());
    if (!valid.ok()) {
        return core::make_unexpected(
            Context("validate managed secret directory", parent.value(), valid.error()));
    }

    const core::Expected<std::string, std::string> contents =
        ReadFile(path, opened.value()->get(), max_len);
    if (!contents.ok()) return core::make_unexpected(contents.error());
    return Optional<std::string>(contents.value());
}

core::Expected<CreateOutcome, std::string> Create(const std::string& path,
                                                  const std::string& contents) {
    const core::Expected<std::string, std::string> parent_result = ManagedParent(path);
    if (!parent_result.ok()) return core::make_unexpected(parent_result.error());
    const std::string parent = parent_result.value();

    const core::Expected<Unit, std::string> made = fs::CreateDirAllWithMode(parent, 0700);
    if (!made.ok()) {
        return core::make_unexpected(
            Context("create managed secret directory", parent, made.error()));
    }

    // Ownership is checked *before* the mode is tightened: chmod'ing a
    // directory we do not own would be both futile and a privilege hazard.
    const core::Expected<fs::FileStat, std::string> identity = ValidateDirectoryIdentity(parent);
    if (!identity.ok()) {
        return core::make_unexpected(Context("validate managed secret directory ownership",
                                             parent, identity.error()));
    }

    const core::Expected<Unit, std::string> tightened = fs::SetPermissions(parent, 0700);
    if (!tightened.ok()) {
        return core::make_unexpected(
            Context("set permissions on", parent, tightened.error()));
    }

    const core::Expected<Unit, std::string> valid = ValidateDirectory(parent);
    if (!valid.ok()) {
        return core::make_unexpected(
            Context("validate managed secret directory", parent, valid.error()));
    }

    core::Expected<fs::TempFile, std::string> temporary = fs::CreateTempFileIn(parent, ".tmp-");
    if (!temporary.ok()) {
        return core::make_unexpected(
            Context("create temporary secret in", parent, temporary.error()));
    }
    const std::string temporary_path = temporary.value().path;

    // From here on every failure must remove the temporary file.
    const core::Expected<Unit, std::string> mode =
        fs::SetPermissions(temporary_path, 0600);
    if (!mode.ok()) {
        fs::RemoveFile(temporary_path);
        return core::make_unexpected(Context("set permissions on", temporary_path, mode.error()));
    }

    // Written through the descriptor we already hold, not by reopening the
    // path: that keeps the bytes and the following fsync on the same file, and
    // leaves no window for the path to be swapped.
    const core::Expected<Unit, std::string> written =
        fs::WriteFd(temporary.value().fd.get(), contents);
    if (!written.ok()) {
        fs::RemoveFile(temporary_path);
        return core::make_unexpected(
            Context("write temporary secret in", parent, written.error()));
    }

    // fsync the data before publishing the name, so a crash can never expose a
    // secret file with truncated contents.
    const core::Expected<Unit, std::string> synced = fs::SyncFd(temporary.value().fd.get());
    if (!synced.ok()) {
        fs::RemoveFile(temporary_path);
        return core::make_unexpected(
            Context("sync temporary secret in", parent, synced.error()));
    }
    temporary.value().fd.reset();  // close before linking

    const core::Expected<bool, std::string> persisted =
        fs::PersistNoClobber(temporary_path, path);
    if (!persisted.ok()) {
        fs::RemoveFile(temporary_path);
        return core::make_unexpected(Context("persist managed secret", path, persisted.error()));
    }

    if (persisted.value()) {
        const core::Expected<Unit, std::string> dir_synced = fs::SyncDirectory(parent);
        if (!dir_synced.ok()) {
            return core::make_unexpected(
                Context("sync managed secret directory", parent, dir_synced.error()));
        }
        CreateOutcome outcome;
        outcome.kind = CreateOutcome::Kind::Created;
        return outcome;
    }

    // Lost the race. Re-validate the directory (the winner may have been a
    // different uid) and hand back their file.
    const core::Expected<Unit, std::string> revalidated = ValidateDirectory(parent);
    if (!revalidated.ok()) {
        return core::make_unexpected(
            Context("validate managed secret directory", parent, revalidated.error()));
    }

    core::Expected<fs::FileDescriptor, std::string> existing = fs::OpenReadNoFollow(path);
    if (!existing.ok()) {
        return core::make_unexpected(Context("open managed secret", path, existing.error()));
    }

    CreateOutcome outcome;
    outcome.kind = CreateOutcome::Kind::Existing;
    outcome.existing = core::fs::FileDescriptor(existing.value().release());
    return outcome;
}

}  // namespace managed_secret
}  // namespace agentenv
