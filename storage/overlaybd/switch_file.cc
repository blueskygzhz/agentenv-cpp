// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/backend/switch.rs
#include "agentenv/storage/overlaybd/switch_file.h"

#include <string>

#include "agentenv/storage/overlaybd/local_file.h"
#include "agentenv/storage/overlaybd/tar.h"
#include "agentenv/storage/overlaybd/zfile.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

namespace {

std::string WithPath(const std::string& message, const char* file_path,
                     const std::string& prefix) {
    if (file_path == 0) return message;
    return prefix + " `" + std::string(file_path) + "` failed: " + message;
}

}  // namespace

core::Expected<std::shared_ptr<VirtualFile>, std::string> TryOpenZFile(
    std::shared_ptr<VirtualFile> file, bool verify, const char* file_path) {
    if (!file) return core::make_unexpected(std::string("try open zfile: null file"));

    const core::Expected<int, std::string> detected = zfile::IsZFile(file.get());
    if (!detected.ok()) {
        return core::make_unexpected(
            WithPath(detected.error(), file_path, "check file type"));
    }

    // Not a zfile: handed back untouched. An uncompressed layer is just as
    // valid, so this is a detection, not a requirement.
    if (detected.value() != 1) return file;

    const core::Expected<std::shared_ptr<VirtualFile>, std::string> opened =
        zfile::ZFileOpenRoVfile(file, verify);
    if (!opened.ok()) {
        return core::make_unexpected(WithPath(opened.error(), file_path, "open zfile"));
    }
    return opened.value();
}

core::Expected<std::shared_ptr<VirtualFile>, std::string> SwitchFile::CurrentFile() const {
    // The local copy wins as soon as it exists; that is the whole switch.
    if (local_file_) return local_file_;
    if (file_) return file_;
    return core::make_unexpected(std::string("switch file has no active backing file"));
}

core::Optional<std::string> SwitchFile::FilePath() const { return filepath_; }

core::Expected<core::Unit, std::string> SwitchFile::SetSwitchFile(const std::string& filepath) {
    filepath_ = core::Optional<std::string>(filepath);

    const core::Expected<std::shared_ptr<LocalFile>, std::string> local =
        LocalFile::OpenRo(filepath);
    if (!local.ok()) {
        return core::make_unexpected("open commit file `" + filepath +
                                     "` failed: " + local.error());
    }

    // The same detection chain the source went through. A committed layer is
    // not necessarily stored the way the source was served, so skipping this
    // would read compressed bytes as content.
    const core::Expected<std::shared_ptr<VirtualFile>, std::string> untarred =
        NewTarFileAdaptor(local.value());
    if (!untarred.ok()) {
        return core::make_unexpected("open commit file as tar file `" + filepath +
                                     "` failed: " + untarred.error());
    }

    // Verification off: this file is local, and verification exists to catch
    // a bad transfer.
    const core::Expected<std::shared_ptr<VirtualFile>, std::string> opened =
        TryOpenZFile(untarred.value(), false, filepath.c_str());
    if (!opened.ok()) return core::make_unexpected(opened.error());

    local_file_ = opened.value();
    return core::Unit();
}

core::Expected<std::shared_ptr<SwitchFile>, std::string> NewSwitchFile(
    std::shared_ptr<VirtualFile> source, bool local, const char* file_path) {
    if (!source) return core::make_unexpected(std::string("new switch file: null source"));

    // One retry: the probe reads through whatever cache the source sits
    // behind, so a single torn read is worth another attempt before the whole
    // layer fails.
    int retry = 1;
    core::Expected<std::shared_ptr<VirtualFile>, std::string> opened =
        core::make_unexpected(std::string());
    for (;;) {
        // `!local`: a remote source is verified, a local one is trusted.
        opened = TryOpenZFile(source, !local, file_path);
        if (opened.ok()) break;
        if (retry > 0) {
            --retry;
            continue;
        }
        return core::make_unexpected(
            WithPath(opened.error(), file_path, "open source file as zfile"));
    }

    std::shared_ptr<SwitchFile> sf(new SwitchFile());
    // Exactly one of the two slots is filled. A file opened as already-local
    // goes straight into the local slot, so no switch is pending for it.
    if (local) {
        sf->local_file_ = opened.value();
    } else {
        sf->file_ = opened.value();
    }
    if (file_path != 0) sf->filepath_ = core::Optional<std::string>(std::string(file_path));
    return sf;
}

// ---------------------------------------------------------------------------
// VirtualFile — every call resolves the current file first
// ---------------------------------------------------------------------------

core::Expected<ByteBuffer, std::string> SwitchFile::ReadAt(uint64_t offset, size_t len) {
    const core::Expected<std::shared_ptr<VirtualFile>, std::string> f = CurrentFile();
    if (!f.ok()) return core::make_unexpected(f.error());
    return f.value()->ReadAt(offset, len);
}

core::Expected<size_t, std::string> SwitchFile::ReadAtInto(uint64_t offset, uint8_t* dst,
                                                             size_t len) {
    const core::Expected<std::shared_ptr<VirtualFile>, std::string> f = CurrentFile();
    if (!f.ok()) return core::make_unexpected(f.error());
    return f.value()->ReadAtInto(offset, dst, len);
}

core::Expected<size_t, std::string> SwitchFile::WriteAt(uint64_t offset, const uint8_t* data,
                                                          size_t len) {
    const core::Expected<std::shared_ptr<VirtualFile>, std::string> f = CurrentFile();
    if (!f.ok()) return core::make_unexpected(f.error());
    return f.value()->WriteAt(offset, data, len);
}

core::Expected<uint64_t, std::string> SwitchFile::Size() {
    const core::Expected<std::shared_ptr<VirtualFile>, std::string> f = CurrentFile();
    if (!f.ok()) return core::make_unexpected(f.error());
    return f.value()->Size();
}

core::Expected<core::Unit, std::string> SwitchFile::Truncate(uint64_t size) {
    const core::Expected<std::shared_ptr<VirtualFile>, std::string> f = CurrentFile();
    if (!f.ok()) return core::make_unexpected(f.error());
    return f.value()->Truncate(size);
}

core::Expected<core::Unit, std::string> SwitchFile::Sync() {
    const core::Expected<std::shared_ptr<VirtualFile>, std::string> f = CurrentFile();
    if (!f.ok()) return core::make_unexpected(f.error());
    return f.value()->Sync();
}

core::Expected<core::Optional<uint64_t>, std::string> SwitchFile::SeekData(uint64_t offset) {
    const core::Expected<std::shared_ptr<VirtualFile>, std::string> f = CurrentFile();
    if (!f.ok()) return core::make_unexpected(f.error());
    return f.value()->SeekData(offset);
}

core::Expected<core::Optional<uint64_t>, std::string> SwitchFile::SeekHole(uint64_t offset) {
    const core::Expected<std::shared_ptr<VirtualFile>, std::string> f = CurrentFile();
    if (!f.ok()) return core::make_unexpected(f.error());
    return f.value()->SeekHole(offset);
}

core::Expected<core::Unit, std::string> SwitchFile::EvictRange(uint64_t offset, uint64_t len) {
    const core::Expected<std::shared_ptr<VirtualFile>, std::string> f = CurrentFile();
    if (!f.ok()) return core::make_unexpected(f.error());
    return f.value()->EvictRange(offset, len);
}

core::Expected<core::Unit, std::string> SwitchFile::EvictAll() {
    const core::Expected<std::shared_ptr<VirtualFile>, std::string> f = CurrentFile();
    if (!f.ok()) return core::make_unexpected(f.error());
    return f.value()->EvictAll();
}

core::Expected<ByteBuffer, std::string> SwitchFile::FGetXattr(const std::string& name) {
    const core::Expected<std::shared_ptr<VirtualFile>, std::string> f = CurrentFile();
    if (!f.ok()) return core::make_unexpected(f.error());
    return f.value()->FGetXattr(name);
}

core::Expected<std::vector<std::string>, std::string> SwitchFile::FListXattr() {
    const core::Expected<std::shared_ptr<VirtualFile>, std::string> f = CurrentFile();
    if (!f.ok()) return core::make_unexpected(f.error());
    return f.value()->FListXattr();
}

core::Expected<core::Unit, std::string> SwitchFile::FSetXattr(const std::string& name,
                                                                 const uint8_t* value,
                                                                 size_t len, int flags) {
    const core::Expected<std::shared_ptr<VirtualFile>, std::string> f = CurrentFile();
    if (!f.ok()) return core::make_unexpected(f.error());
    return f.value()->FSetXattr(name, value, len, flags);
}

core::Expected<core::Unit, std::string> SwitchFile::FRemoveXattr(const std::string& name) {
    const core::Expected<std::shared_ptr<VirtualFile>, std::string> f = CurrentFile();
    if (!f.ok()) return core::make_unexpected(f.error());
    return f.value()->FRemoveXattr(name);
}

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
