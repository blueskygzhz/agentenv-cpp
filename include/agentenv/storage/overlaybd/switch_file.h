// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/backend/switch.rs — the source/local switch.
#ifndef AGENTENV_STORAGE_OVERLAYBD_SWITCH_FILE_H_
#define AGENTENV_STORAGE_OVERLAYBD_SWITCH_FILE_H_

#include <memory>
#include <string>

#include "agentenv/core/expected.h"
#include "agentenv/core/optional.h"
#include "agentenv/storage/overlaybd/virtual_file.h"

namespace agentenv {
namespace storage {
namespace overlaybd {

/// Rust `switch.rs :: try_open_zfile` — wrap `file` in a zfile reader if it is
/// one, otherwise hand it back unchanged.
///
/// Probing is cheap (one 512-byte header read) and the answer decides how
/// every later read is interpreted, so it happens once at open rather than per
/// read. A file that is not a zfile is passed through rather than refused:
/// uncompressed layers are equally valid.
core::Expected<std::shared_ptr<VirtualFile>, std::string> TryOpenZFile(
    std::shared_ptr<VirtualFile> file, bool verify, const char* file_path);

/// Rust `SwitchFile` — a file that can be re-pointed from a remote source to a
/// local copy once one exists.
///
/// Reads go to the local file as soon as one is installed and to the source
/// before that. The switch is one-way and happens at most once: it exists so a
/// layer being downloaded can be read from the registry meanwhile, then served
/// locally without the caller re-opening anything or noticing the change.
class SwitchFile : public VirtualFile {
 public:
    /// Rust `filepath`.
    core::Optional<std::string> FilePath() const;

    /// Rust `set_switch_file` — install the local copy and route reads to it.
    ///
    /// The local file goes through the same tar and zfile detection as the
    /// source did. A committed layer on disk is not necessarily stored the
    /// same way the source was served, so skipping detection here would read
    /// the compressed bytes as content.
    core::Expected<core::Unit, std::string> SetSwitchFile(const std::string& filepath);

    // -- VirtualFile: every operation goes to whichever file is current -----

    core::Expected<ByteBuffer, std::string> ReadAt(uint64_t offset, size_t len) override;
    core::Expected<size_t, std::string> ReadAtInto(uint64_t offset, uint8_t* dst,
                                                     size_t len) override;
    core::Expected<size_t, std::string> WriteAt(uint64_t offset, const uint8_t* data,
                                                  size_t len) override;
    core::Expected<uint64_t, std::string> Size() override;
    core::Expected<core::Unit, std::string> Truncate(uint64_t size) override;
    core::Expected<core::Unit, std::string> Sync() override;
    core::Expected<core::Optional<uint64_t>, std::string> SeekData(uint64_t offset) override;
    core::Expected<core::Optional<uint64_t>, std::string> SeekHole(uint64_t offset) override;
    core::Expected<core::Unit, std::string> EvictRange(uint64_t offset, uint64_t len) override;
    core::Expected<core::Unit, std::string> EvictAll() override;
    core::Expected<ByteBuffer, std::string> FGetXattr(const std::string& name) override;
    core::Expected<std::vector<std::string>, std::string> FListXattr() override;
    core::Expected<core::Unit, std::string> FSetXattr(const std::string& name,
                                                         const uint8_t* value, size_t len,
                                                         int flags) override;
    core::Expected<core::Unit, std::string> FRemoveXattr(const std::string& name) override;

 private:
    friend core::Expected<std::shared_ptr<SwitchFile>, std::string> NewSwitchFile(
        std::shared_ptr<VirtualFile>, bool, const char*);

    SwitchFile() {}
    SwitchFile(const SwitchFile&);
    SwitchFile& operator=(const SwitchFile&);

    /// Rust `current_file` — the local copy if installed, else the source.
    core::Expected<std::shared_ptr<VirtualFile>, std::string> CurrentFile() const;

    std::shared_ptr<VirtualFile> file_;
    std::shared_ptr<VirtualFile> local_file_;
    core::Optional<std::string> filepath_;
};

/// Rust `new_switch_file`.
///
/// `local` says whether `source` is already the local copy. It inverts the
/// zfile verify flag: a remote source is checked, a local one is trusted,
/// because verification exists to catch a bad transfer.
///
/// Retries the open once. The probe reads through whatever cache the source
/// sits behind, so a single torn read is worth one more attempt before
/// failing the layer.
core::Expected<std::shared_ptr<SwitchFile>, std::string> NewSwitchFile(
    std::shared_ptr<VirtualFile> source, bool local, const char* file_path);

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_SWITCH_FILE_H_
