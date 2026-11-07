// SPDX-License-Identifier: MIT
// Rust: src/overlaybd/compact.rs
#include "agentenv/overlaybd/compact.h"

#include <cstdio>
#include <memory>

#include "agentenv/core/fs.h"
#include "agentenv/core/identity.h"
#include "agentenv/core/logging.h"
#include "agentenv/storage/overlaybd/compact_writer_sink.h"
#include "agentenv/storage/overlaybd/local_file.h"
#include "agentenv/storage/overlaybd/lsmt_readonly.h"
#include "agentenv/storage/overlaybd/switch_file.h"
#include "agentenv/storage/overlaybd/tar.h"
#include "agentenv/storage/overlaybd/virtual_file.h"
#include "agentenv/storage/overlaybd/zfile.h"
#include "agentenv/storage/util/compact_writer.h"

namespace agentenv {
namespace overlaybd {

namespace obd = storage::overlaybd;

const std::size_t kMaxCompressionWorkers = 64;

OverlaybdCompactOutput OverlaybdCompactOutput::FromPublishCompressionConfig(
    const cfg::SnapshotPublishCompressionConfig& config) {
    if (!config.enabled) return Raw();
    // Rust `clamp(1, MAX)`. The lower bound matters as much as the upper one:
    // `workers = 0` would otherwise mean "no compression threads", which is
    // not a slower compression but no progress at all.
    std::size_t workers = config.workers;
    if (workers < 1) workers = 1;
    if (workers > kMaxCompressionWorkers) workers = kMaxCompressionWorkers;
    return ZFile(config.algorithm, workers);
}

namespace {

/// Rust `Path::with_extension(format!("commit.{}.tmp", Uuid::now_v7()))`.
///
/// `with_extension` *replaces* the final extension rather than appending to
/// it, so `x/y.commit` becomes `x/y.commit.<uuid>.tmp` but `x/y.foo` becomes
/// `x/y.commit.<uuid>.tmp` too -- the `.foo` is dropped. Reproduced exactly,
/// because the sibling name is what the failure cleanup removes and what the
/// `.tmp` suffix scan in the staging directory recognises.
///
/// The uuid is per-call, so two concurrent compactions of the same output do
/// not share a temp file and clobber each other's partial writes.
std::string TempOutputPath(const std::string& output_path) {
    const std::string suffix = "commit." + core::Uuid::GenV7().ToString() + ".tmp";

    // Only the basename may carry an extension: a dot in a parent directory
    // name is part of that directory, not of this file.
    const size_t slash = output_path.find_last_of('/');
    const size_t name_begin = (slash == std::string::npos) ? 0 : slash + 1;
    const size_t dot = output_path.find_last_of('.');

    // A leading dot makes a hidden file, not an extension -- `.commit` has no
    // stem to keep, so the suffix is appended instead of replacing the name.
    const bool has_extension = dot != std::string::npos && dot > name_begin;
    const std::string stem = has_extension ? output_path.substr(0, dot) : output_path;
    return stem + "." + suffix;
}

/// Rust: the `LocalFile::open_ro` → `new_tar_file_adaptor` → `new_switch_file`
/// chain, which is what lets one compaction mix raw and zfile inputs.
///
/// `local = true`: these are files on this host, so the switch file trusts
/// them and leaves the zfile verify flag off. Verification exists to catch a
/// bad transfer, and there was none.
core::Expected<std::shared_ptr<obd::VirtualFile>, std::string> OpenLayerForCompaction(
    const std::string& path) {
    const core::Expected<std::shared_ptr<obd::LocalFile>, std::string> local =
        obd::LocalFile::OpenRo(path);
    if (!local.ok()) {
        return core::make_unexpected("open layer for compaction: " + path + ": " + local.error());
    }
    const core::Expected<std::shared_ptr<obd::VirtualFile>, std::string> tar_adapted =
        obd::NewTarFileAdaptor(local.value());
    if (!tar_adapted.ok()) {
        return core::make_unexpected("adapt layer as tar file: " + path + ": " +
                                     tar_adapted.error());
    }
    const core::Expected<std::shared_ptr<obd::SwitchFile>, std::string> switched =
        obd::NewSwitchFile(tar_adapted.value(), true, path.c_str());
    if (!switched.ok()) {
        return core::make_unexpected("open layer via switch file: " + path + ": " +
                                     switched.error());
    }
    return std::shared_ptr<obd::VirtualFile>(switched.value());
}

/// Rust `create_commit_args`' zfile arm — the compressor the output is
/// written through.
///
/// `block_size` and `verify` are fixed (4096 / off) rather than configurable,
/// matching `CompressOptions::new(algorithm, DEFAULT_BLOCK_SIZE, 0)`: the
/// operator switch exposes only the algorithm and the worker count. `verify`
/// stays off because the destination is a file about to be hashed and
/// uploaded, so a per-block checksum would pay for a second integrity layer
/// over the digest that already covers it.
core::Expected<std::shared_ptr<obd::zfile::ZFileCompactWriter>, std::string>
NewZFileWriterFor(std::shared_ptr<obd::VirtualFile> output, const OverlaybdCompactOutput& mode) {
    obd::zfile::CompressOptions opt;
    opt.algo = mode.algorithm == cfg::OverlaybdCompressionAlgorithm::Zstd
                   ? obd::zfile::CompressOptions::kZstd
                   : obd::zfile::CompressOptions::kLz4;
    opt.block_size = obd::zfile::CompressOptions::kDefaultBlockSize;
    opt.verify = 0;

    obd::zfile::CompressArgs args(opt);
    // Rust `workers.max(1)`. `mode.workers` is already clamped by
    // `FromPublishCompressionConfig`, but a directly constructed mode is not,
    // and zero workers would mean no progress rather than slow progress.
    args.workers = mode.workers < 1 ? 1 : mode.workers;
    return obd::zfile::ZFileCompactWriter::Create(output, args);
}

/// The merge itself, factored out so the caller can clean up the temp file on
/// any failure path without repeating the removal at each `return`.
core::Expected<core::Unit, std::string> MergeIntoTemp(
    const std::vector<std::shared_ptr<obd::VirtualFile> >& src_files,
    const std::string& temp_path, const std::string& output_path,
    const OverlaybdCompactOutput& mode) {
    const core::Expected<std::shared_ptr<obd::LocalFile>, std::string> output =
        obd::LocalFile::New(temp_path);
    if (!output.ok()) {
        return core::make_unexpected("create compacted layer output file: " + output.error());
    }

    const core::Expected<std::shared_ptr<obd::LsmtReadOnlyFile>, std::string> merged =
        obd::OpenFilesRo(src_files);
    if (!merged.ok()) {
        return core::make_unexpected("merge layers: " + merged.error());
    }

    if (mode.is_zfile()) {
        const core::Expected<std::shared_ptr<obd::zfile::ZFileCompactWriter>, std::string>
            writer = NewZFileWriterFor(output.value(), mode);
        if (!writer.ok()) {
            return core::make_unexpected("create zfile compact writer: " + writer.error());
        }
        obd::CompactWriterSink sink(
            std::shared_ptr<storage::util::CompactWriter>(writer.value()));
        const core::Expected<core::Unit, std::string> flattened =
            merged.value()->FlattenTo(&sink);
        if (!flattened.ok()) {
            // `CompactSink::WriteAt` can only answer bool, so `FlattenTo`'s
            // message is the generic "write data failed". The sink kept the
            // real reason -- typically an offset the compressor refused --
            // and reporting only the generic one would send a reader of this
            // error looking at the wrong layer of the stack.
            if (sink.failed()) {
                return core::make_unexpected("merge layers: " + flattened.error() + ": " +
                                             sink.error());
            }
            return core::make_unexpected("merge layers: " + flattened.error());
        }
        // Rust's `compact_to` ends with `writer.finalize()`; this port's
        // `CompactTo` has no such step, and without it the output would hold
        // a header and data but no block index -- a file every reader
        // rejects. See `CompactWriterSink`.
        const core::Expected<core::Unit, std::string> finalized = sink.Finalize();
        if (!finalized.ok()) {
            return core::make_unexpected("finalize zfile layer: " + finalized.error());
        }
    } else {
        const core::Expected<core::Unit, std::string> flattened =
            merged.value()->Flatten(output.value());
        if (!flattened.ok()) {
            return core::make_unexpected("merge layers: " + flattened.error());
        }
    }

    // Rename last, so `output_path` goes from "previous contents" to
    // "complete new layer" with nothing observable in between.
    if (::rename(temp_path.c_str(), output_path.c_str()) != 0) {
        return core::make_unexpected("publish compacted layer to " + output_path + " failed");
    }
    return core::Unit();
}

}  // namespace

core::Expected<core::Optional<std::string>, std::string> CompactLayers(
    const std::vector<storage::overlaybd::LayerConfig>& layers, const std::string& output_path,
    const OverlaybdCompactOutput& mode) {
    if (layers.empty()) return core::Optional<std::string>();

    std::vector<std::shared_ptr<obd::VirtualFile> > src_files;
    src_files.reserve(layers.size());
    for (size_t i = 0; i < layers.size(); ++i) {
        const core::Expected<std::shared_ptr<obd::VirtualFile>, std::string> opened =
            OpenLayerForCompaction(layers[i].file);
        if (!opened.ok()) return core::make_unexpected(opened.error());
        src_files.push_back(opened.value());
    }

    const std::string temp_path = TempOutputPath(output_path);
    const core::Expected<core::Unit, std::string> merged =
        MergeIntoTemp(src_files, temp_path, output_path, mode);
    if (!merged.ok()) {
        // Best effort, and deliberately not reported: the merge error is the
        // one worth propagating, and a cleanup failure on a path that may
        // never have been created would only bury it.
        core::fs::RemoveFile(temp_path);
        return core::make_unexpected(merged.error());
    }

    AGENTENV_DEBUG("compacted overlaybd layers: output=" << output_path
                                                         << " input_layers=" << layers.size());
    return core::Optional<std::string>(output_path);
}

}  // namespace overlaybd
}  // namespace agentenv
