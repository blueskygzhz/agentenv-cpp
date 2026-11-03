// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/tools/packaging.rs — package a raw filesystem
// image as a sealed overlaybd lower layer.
#ifndef AGENTENV_STORAGE_OVERLAYBD_TOOLS_PACKAGING_H_
#define AGENTENV_STORAGE_OVERLAYBD_TOOLS_PACKAGING_H_

#include <cstdint>
#include <string>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/storage/overlaybd/lsmt.h"
#include "agentenv/storage/overlaybd/virtual_file.h"

namespace agentenv {
namespace storage {
namespace overlaybd {
namespace tools {

/// Rust `lsmt/file/helper.rs :: create_mappings_from_sparse`.
///
/// Declared here rather than in `lsmt.h` because it reads through a
/// `VirtualFile`, and `lsmt.h` deliberately does not depend on the io layer
/// (see the note at the top of `internal.h`). What it produces is the input
/// `CompactTo` consumes: the *allocated* extents of a sparse file, expressed
/// in 512B units, with holes left unmapped so they cost no space in the
/// output and read back as zeros.
///
/// `start_offset` must be 512B-aligned. A file no longer than `start_offset`
/// yields no mappings rather than an error, matching Rust's early return.
///
/// An extent boundary that is not 512B-aligned is refused rather than
/// truncated: the block arithmetic truncates, so a trailing partial block
/// would silently read back as zeros with nothing reporting it. Both platforms
/// report extents at filesystem-block granularity, so in practice this only
/// fires on a source whose *size* is not a multiple of 512.
core::Expected<std::vector<lsmt::SegmentMapping>, std::string>
    CreateMappingsFromSparse(VirtualFile* file, uint64_t start_offset);

/// Rust `tools/packaging.rs :: package_raw_as_overlaybd`.
///
/// Compacts the raw file at `source` into a sealed LSMT lower layer at
/// `output`. The virtual size of the layer is the source's length, and only
/// its allocated extents are copied.
///
/// Built under a `.tmp` sibling and renamed into place, because a reader
/// cannot tell a truncated commit file from a short one -- so the final name
/// must appear only once the layer is complete. A failure removes the
/// temporary rather than leaving it to be mistaken for a usable layer.
core::Expected<core::Unit, std::string>
    PackageRawAsOverlaybd(const std::string& source, const std::string& output);

}  // namespace tools
}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_TOOLS_PACKAGING_H_
