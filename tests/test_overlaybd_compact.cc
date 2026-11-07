// SPDX-License-Identifier: MIT
// Rust: src/overlaybd/compact.rs :: compact_layers — and its
// `compact_layers_mixed_input_and_configured_output` test.
//
// `CompactLayers` is the publish path's merge step: it resolves a layer stack
// into one sealed layer and writes it in the physical form the operator's
// `publish_compression` switch asks for. Two properties are load-bearing and
// neither is visible from the return value:
//
//   1. The merge must resolve shadowing *and* keep block order. A compaction
//      that lost either would still produce a file that opens and parses,
//      which is why every assertion here reads the content back rather than
//      checking the container.
//   2. The publish must be atomic. `output_path` is a path other processes may
//      read, so a half-written layer must never be observable there, and a
//      failed attempt must not leave its staging file behind.
#include "agentenv/overlaybd/compact.h"

#include <dirent.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "agentenv/core/fs.h"
#include "agentenv/storage/overlaybd/local_file.h"
#include "agentenv/storage/overlaybd/lsmt_readonly.h"
#include "agentenv/storage/overlaybd/lsmt_rw.h"
#include "agentenv/storage/overlaybd/switch_file.h"
#include "agentenv/storage/overlaybd/tar.h"
#include "agentenv/storage/overlaybd/zfile.h"
#include "microtest.h"

using namespace agentenv;   // NOLINT

namespace obd = agentenv::storage::overlaybd;
namespace obdz = agentenv::storage::overlaybd::zfile;

namespace {

const uint64_t kPage = 4096;
const uint64_t kPages = 4;
const uint64_t kVirtualSize = kPages * kPage;

std::string MakeTempDir() {
    char tmpl[] = "/tmp/agentenv_compact_XXXXXX";
    char* made = ::mkdtemp(tmpl);
    if (made == 0) {
        std::fprintf(stderr, "  FAIL: mkdtemp\n");
        std::exit(1);
    }
    return std::string(made);
}

void RemoveTreeBestEffort(const std::string& dir) {
    const std::string cmd = "rm -rf '" + dir + "'";
    if (::system(cmd.c_str()) != 0) { /* ignored on purpose */ }
}

overlaybd::OverlaybdCompactOutput Lz4Mode() {
    return overlaybd::OverlaybdCompactOutput::ZFile(cfg::OverlaybdCompressionAlgorithm::Lz4, 1);
}

/// Rust `has_temp_file` — whether a compaction staging file survived.
///
/// Scans for the `.tmp` suffix rather than for a known name, because the
/// sibling path carries a fresh uuid per call and the point is that *none*
/// remain.
bool HasTempFile(const std::string& dir) {
    DIR* d = ::opendir(dir.c_str());
    if (d == 0) return false;
    bool found = false;
    for (struct dirent* e = ::readdir(d); e != 0; e = ::readdir(d)) {
        const std::string name(e->d_name);
        if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) {
            found = true;
            break;
        }
    }
    ::closedir(d);
    return found;
}

/// A sealed single-file overlaybd layer that fills only `pages` with `fill`.
///
/// Sparse on purpose: the pages left unwritten are holes, which is what makes
/// a stack of these exercise shadowing -- an upper layer's hole must let the
/// lower layer's content through, while a written page must mask it.
bool WriteSparseLayer(const std::string& path, const std::vector<uint64_t>& pages, uint8_t fill) {
    const core::Expected<std::shared_ptr<obd::LocalFile>, std::string> data =
        obd::LocalFile::New(path);
    if (!data.ok()) return false;
    const core::Expected<std::shared_ptr<obd::LsmtFile>, std::string> layer =
        obd::LsmtFile::Create(data.value(), std::shared_ptr<obd::VirtualFile>(), kVirtualSize,
                              true);
    if (!layer.ok()) return false;
    for (size_t i = 0; i < pages.size(); ++i) {
        const std::vector<uint8_t> page(static_cast<size_t>(kPage), fill);
        if (!layer.value()->WriteAt(pages[i] * kPage, &page[0], page.size()).ok()) return false;
    }
    return layer.value()->CloseSeal().ok();
}

/// Read a sealed layer back through the runtime's open chain, so a zfile
/// output is transparently decompressed by the switch file.
bool ReadSealedLayer(const std::string& path, size_t len, std::vector<uint8_t>* out) {
    const core::Expected<std::shared_ptr<obd::LocalFile>, std::string> local =
        obd::LocalFile::OpenRo(path);
    if (!local.ok()) return false;
    const core::Expected<std::shared_ptr<obd::VirtualFile>, std::string> tar_adapted =
        obd::NewTarFileAdaptor(local.value());
    if (!tar_adapted.ok()) return false;
    const core::Expected<std::shared_ptr<obd::SwitchFile>, std::string> switched =
        obd::NewSwitchFile(tar_adapted.value(), true, path.c_str());
    if (!switched.ok()) return false;

    std::vector<std::shared_ptr<obd::VirtualFile> > files;
    files.push_back(std::shared_ptr<obd::VirtualFile>(switched.value()));
    const core::Expected<std::shared_ptr<obd::LsmtReadOnlyFile>, std::string> merged =
        obd::OpenFilesRo(files);
    if (!merged.ok()) return false;

    out->assign(len, 0);
    const core::Expected<size_t, std::string> got =
        merged.value()->ReadAtInto(0, &(*out)[0], len);
    return got.ok() && got.value() == len;
}

int ProbeZFile(const std::string& path) {
    const core::Expected<std::shared_ptr<obd::LocalFile>, std::string> f =
        obd::LocalFile::OpenRo(path);
    if (!f.ok()) return -1;
    const core::Expected<int, std::string> out = obdz::IsZFile(f.value().get());
    return out.ok() ? out.value() : -1;
}

std::vector<obd::LayerConfig> Layers(const std::vector<std::string>& paths) {
    std::vector<obd::LayerConfig> out(paths.size());
    for (size_t i = 0; i < paths.size(); ++i) out[i].file = paths[i];
    return out;
}

uint64_t FileSize(const std::string& path) {
    const core::Expected<std::shared_ptr<obd::LocalFile>, std::string> f =
        obd::LocalFile::OpenRo(path);
    if (!f.ok()) return 0;
    const core::Expected<uint64_t, std::string> size = f.value()->Size();
    return size.ok() ? size.value() : 0;
}

/// The image a three-layer stack is expected to resolve to.
///
/// bottom fills every page with 0x11; middle overwrites page 1 with 0x22;
/// top overwrites page 2 with 0x33. Page 3 stays 0x11, which is what proves
/// the upper layers' holes did not mask the base.
std::vector<uint8_t> ExpectedStackImage() {
    std::vector<uint8_t> expected(static_cast<size_t>(kVirtualSize), 0x11);
    for (size_t i = 0; i < kPage; ++i) expected[kPage + i] = 0x22;
    for (size_t i = 0; i < kPage; ++i) expected[2 * kPage + i] = 0x33;
    return expected;
}

bool BuildStack(const std::string& dir, std::vector<std::string>* paths) {
    const std::string bottom = dir + "/bottom.commit";
    const std::string middle = dir + "/middle.commit";
    const std::string top = dir + "/top.commit";

    std::vector<uint64_t> all;
    for (uint64_t p = 0; p < kPages; ++p) all.push_back(p);
    if (!WriteSparseLayer(bottom, all, 0x11)) return false;
    if (!WriteSparseLayer(middle, std::vector<uint64_t>(1, 1), 0x22)) return false;
    if (!WriteSparseLayer(top, std::vector<uint64_t>(1, 2), 0x33)) return false;

    paths->clear();
    paths->push_back(bottom);
    paths->push_back(middle);
    paths->push_back(top);
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// The merge
// ---------------------------------------------------------------------------

/// A stack resolves bottom-to-top, in every output mode, and the result reads
/// back as the same image regardless of how it was stored.
///
/// The three modes share one expected image on purpose: compression is a
/// storage decision, so an output whose content depended on it would be the
/// bug. Rust asserts exactly this (`compact_layers_mixed_input_and_configured_output`).
MT_TEST(a_layer_stack_resolves_bottom_to_top_in_every_output_mode) {
    const std::string dir = MakeTempDir();
    std::vector<std::string> stack;
    MT_EXPECT_TRUE(BuildStack(dir, &stack));
    const std::vector<uint8_t> expected = ExpectedStackImage();

    struct Case {
        const char* name;
        overlaybd::OverlaybdCompactOutput mode;
        int expect_zfile;
    };
    const Case cases[] = {
        {"raw", overlaybd::OverlaybdCompactOutput::Raw(), 0},
        // workers > 1 drives the segmented parallel compression path.
        {"lz4",
         overlaybd::OverlaybdCompactOutput::ZFile(cfg::OverlaybdCompressionAlgorithm::Lz4, 2), 1},
        {"zstd",
         overlaybd::OverlaybdCompactOutput::ZFile(cfg::OverlaybdCompressionAlgorithm::Zstd, 1),
         1},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const std::string output = dir + "/compacted-" + cases[i].name + ".commit";
        const core::Expected<core::Optional<std::string>, std::string> published =
            overlaybd::CompactLayers(Layers(stack), output, cases[i].mode);
        MT_EXPECT_TRUE(published.ok());
        if (!published.ok()) {
            std::fprintf(stderr, "  note: %s: %s\n", cases[i].name, published.error().c_str());
            continue;
        }
        MT_EXPECT_TRUE(published.value().has_value());
        MT_EXPECT_EQ(*published.value(), output);
        // The staging file is gone, so the rename -- not a copy -- is what
        // published the result.
        MT_EXPECT_TRUE(!HasTempFile(dir));
        MT_EXPECT_EQ(ProbeZFile(output), cases[i].expect_zfile);

        std::vector<uint8_t> read_back;
        MT_EXPECT_TRUE(ReadSealedLayer(output, expected.size(), &read_back));
        MT_EXPECT_TRUE(read_back == expected);
    }
    RemoveTreeBestEffort(dir);
}

/// A zfile output of compressible content is smaller than the raw one.
///
/// Without this, every other zfile assertion here would still pass on an
/// implementation that wrote a zfile container around uncompressed blocks --
/// the readback would succeed and the probe would answer 1.
MT_TEST(a_zfile_output_is_actually_smaller_than_the_raw_one) {
    const std::string dir = MakeTempDir();
    std::vector<std::string> stack;
    MT_EXPECT_TRUE(BuildStack(dir, &stack));

    const std::string raw_out = dir + "/raw.commit";
    const std::string zfile_out = dir + "/zfile.commit";
    MT_EXPECT_TRUE(
        overlaybd::CompactLayers(Layers(stack), raw_out, overlaybd::OverlaybdCompactOutput::Raw())
            .ok());
    MT_EXPECT_TRUE(overlaybd::CompactLayers(Layers(stack), zfile_out, Lz4Mode()).ok());

    const uint64_t raw_size = FileSize(raw_out);
    const uint64_t zfile_size = FileSize(zfile_out);
    MT_EXPECT_TRUE(raw_size > 0 && zfile_size > 0);
    MT_EXPECT_TRUE(zfile_size < raw_size);
    RemoveTreeBestEffort(dir);
}

/// Raw and zfile inputs can be mixed in one compaction.
///
/// This is what the tar-adaptor + switch-file open chain buys: the switch file
/// detects a compressed layer and decompresses it transparently, so the
/// merge never learns which of its inputs were compressed. The zfile input is
/// produced by `CompactLayers` itself, which keeps the test from depending on
/// a second way of writing one.
MT_TEST(raw_and_zfile_inputs_can_be_mixed_in_one_compaction) {
    const std::string dir = MakeTempDir();
    std::vector<std::string> stack;
    MT_EXPECT_TRUE(BuildStack(dir, &stack));

    // Recompress the middle layer, then substitute it back into the stack.
    const std::string middle_zfile = dir + "/middle-zfile.commit";
    MT_EXPECT_TRUE(
        overlaybd::CompactLayers(Layers(std::vector<std::string>(1, stack[1])), middle_zfile,
                                 Lz4Mode())
            .ok());
    MT_EXPECT_EQ(ProbeZFile(middle_zfile), 1);
    MT_EXPECT_EQ(ProbeZFile(stack[0]), 0);

    std::vector<std::string> mixed;
    mixed.push_back(stack[0]);     // raw
    mixed.push_back(middle_zfile); // zfile
    mixed.push_back(stack[2]);     // raw

    const std::string output = dir + "/mixed.commit";
    const core::Expected<core::Optional<std::string>, std::string> published =
        overlaybd::CompactLayers(Layers(mixed), output, Lz4Mode());
    MT_EXPECT_TRUE(published.ok());
    if (!published.ok()) {
        std::fprintf(stderr, "  note: %s\n", published.error().c_str());
        RemoveTreeBestEffort(dir);
        return;
    }

    std::vector<uint8_t> read_back;
    const std::vector<uint8_t> expected = ExpectedStackImage();
    MT_EXPECT_TRUE(ReadSealedLayer(output, expected.size(), &read_back));
    MT_EXPECT_TRUE(read_back == expected);
    RemoveTreeBestEffort(dir);
}

/// Compacting an already-compacted layer reproduces the same image.
///
/// The recontainerize path does exactly this -- it compacts a single sealed
/// layer -- so the one-input case is not a degenerate shape to skip.
MT_TEST(compacting_a_single_layer_round_trips_it) {
    const std::string dir = MakeTempDir();
    const std::string source = dir + "/only.commit";
    std::vector<uint64_t> all;
    for (uint64_t p = 0; p < kPages; ++p) all.push_back(p);
    MT_EXPECT_TRUE(WriteSparseLayer(source, all, 0x5A));

    const std::string output = dir + "/out.commit";
    MT_EXPECT_TRUE(
        overlaybd::CompactLayers(Layers(std::vector<std::string>(1, source)), output, Lz4Mode())
            .ok());

    std::vector<uint8_t> read_back;
    MT_EXPECT_TRUE(ReadSealedLayer(output, static_cast<size_t>(kVirtualSize), &read_back));
    MT_EXPECT_TRUE(read_back == std::vector<uint8_t>(static_cast<size_t>(kVirtualSize), 0x5A));
    RemoveTreeBestEffort(dir);
}

// ---------------------------------------------------------------------------
// Publish atomicity
// ---------------------------------------------------------------------------

/// A failed compaction leaves the previously published output byte-identical
/// and removes its staging file.
///
/// Both halves are asserted because they fail independently: writing straight
/// to `output_path` would corrupt the old layer, while renaming without
/// cleanup would accumulate a full layer's worth of garbage per failed
/// publish. Rust asserts the same pair.
MT_TEST(a_failed_compaction_preserves_the_published_output_and_removes_its_staging_file) {
    const std::string dir = MakeTempDir();
    std::vector<std::string> stack;
    MT_EXPECT_TRUE(BuildStack(dir, &stack));

    const std::string output = dir + "/published.commit";
    MT_EXPECT_TRUE(overlaybd::CompactLayers(Layers(stack), output, Lz4Mode()).ok());
    const core::Expected<std::string, std::string> before = core::fs::ReadToString(output);
    MT_EXPECT_TRUE(before.ok());

    // A file that is not an overlaybd layer at all: the open chain accepts it
    // (a plain file passes the tar probe) but the merge has no index to read.
    const std::string bogus = dir + "/bogus.commit";
    MT_EXPECT_TRUE(core::fs::Write(bogus, std::string(4096, '\xAB')).ok());
    std::vector<std::string> broken = stack;
    broken.push_back(bogus);

    const core::Expected<core::Optional<std::string>, std::string> failed =
        overlaybd::CompactLayers(Layers(broken), output, Lz4Mode());
    MT_EXPECT_TRUE(!failed.ok());

    const core::Expected<std::string, std::string> after = core::fs::ReadToString(output);
    MT_EXPECT_TRUE(after.ok());
    if (before.ok() && after.ok()) MT_EXPECT_TRUE(before.value() == after.value());
    MT_EXPECT_TRUE(!HasTempFile(dir));
    RemoveTreeBestEffort(dir);
}

/// A missing input layer fails before anything is written.
MT_TEST(a_missing_input_layer_fails_without_creating_the_output) {
    const std::string dir = MakeTempDir();
    const std::string output = dir + "/out.commit";
    const core::Expected<core::Optional<std::string>, std::string> failed =
        overlaybd::CompactLayers(Layers(std::vector<std::string>(1, dir + "/absent.commit")),
                                 output, Lz4Mode());
    MT_EXPECT_TRUE(!failed.ok());
    if (!failed.ok()) {
        MT_EXPECT_TRUE(failed.error().find("open layer for compaction") != std::string::npos);
    }
    MT_EXPECT_TRUE(!core::fs::Exists(output));
    MT_EXPECT_TRUE(!HasTempFile(dir));
    RemoveTreeBestEffort(dir);
}

// ---------------------------------------------------------------------------
// Degenerate input
// ---------------------------------------------------------------------------

/// An empty layer list reports "no output" rather than failing.
///
/// Rust returns `Ok(None)`, and the distinction is load-bearing: the
/// recontainerize caller treats it as "fall back to the original bytes",
/// which an error would turn into a failed publish.
MT_TEST(an_empty_layer_list_reports_no_output_rather_than_failing) {
    const std::string dir = MakeTempDir();
    const std::string output = dir + "/out.commit";
    const core::Expected<core::Optional<std::string>, std::string> published =
        overlaybd::CompactLayers(std::vector<obd::LayerConfig>(), output, Lz4Mode());
    MT_EXPECT_TRUE(published.ok());
    if (published.ok()) MT_EXPECT_TRUE(!published.value().has_value());
    MT_EXPECT_TRUE(!core::fs::Exists(output));
    RemoveTreeBestEffort(dir);
}

MT_MAIN
