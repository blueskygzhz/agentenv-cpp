// SPDX-License-Identifier: MIT
// Tests for the dense export of a sparse overlaybd layer.
//
// A sparse RW layer stores its data at the *logical* offset, so a layer with
// a 64 MiB address space and two written pages occupies 64 MiB of address
// range. The dense form repacks it so the stored bytes are only the ones that
// exist, while the logical view is unchanged -- and the digest of that dense
// form has to be computable without storing it, because the upload path hashes
// the stream it emits.
//
// Rust: storage/overlaybd/src/dense_export.rs
#include "microtest.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "agentenv/core/digest.h"
#include "agentenv/core/fs.h"
#include "agentenv/storage/overlaybd/dense_export.h"
#include "agentenv/storage/overlaybd/local_file.h"
#include "agentenv/storage/overlaybd/lsmt_readonly.h"
#include "agentenv/storage/overlaybd/lsmt_rw.h"
#include "agentenv/storage/overlaybd/sys.h"

using namespace agentenv::storage::overlaybd;
namespace core = agentenv::core;

namespace {

const uint64_t kPage = 4096;
const uint64_t kVirtualSize = 64ull * 1024 * 1024;
const uint64_t kSecondOffset = 32ull * 1024 * 1024;

std::string MakeTempDir() {
    char tmpl[] = "/tmp/agentenv_de_XXXXXX";
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

/// Build a sealed sparse layer with two written pages far apart, and leave it
/// at `path`. Returns false if the layer could not be built, so a case can
/// report that rather than asserting on a file that does not exist.
bool BuildSparseLayer(const std::string& path, const std::vector<uint8_t>& first,
                      const std::vector<uint8_t>& second) {
    const core::Expected<std::shared_ptr<LocalFile>, std::string> data = LocalFile::New(path);
    if (!data.ok()) return false;

    const core::Expected<std::shared_ptr<LsmtFile>, std::string> sparse = LsmtFile::Create(
        data.value(), std::shared_ptr<VirtualFile>(), kVirtualSize, true);
    if (!sparse.ok()) return false;

    if (!sparse.value()->WriteAt(0, &first[0], first.size()).ok()) return false;
    if (!sparse.value()->WriteAt(kSecondOffset, &second[0], second.size()).ok()) return false;
    if (!sparse.value()->CloseSeal().ok()) return false;
    return true;
}

/// The whole file, as bytes. Used to hash what was actually written, so the
/// streaming digest is compared against a digest of the stored object rather
/// than against another run of the same streaming code.
bool SlurpFile(const std::string& path, std::vector<uint8_t>* out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == 0) return false;
    out->clear();
    uint8_t buf[65536];
    for (;;) {
        const size_t n = std::fread(buf, 1, sizeof(buf), f);
        if (n == 0) break;
        out->insert(out->end(), buf, buf + n);
    }
    std::fclose(f);
    return true;
}

long long FirstMismatch(const uint8_t* got, const uint8_t* want, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        if (got[i] != want[i]) return static_cast<long long>(i);
    }
    return -1;
}

}  // namespace

// ============================================================
// the digest accumulator
// ============================================================

MT_TEST(the_digest_matches_a_plain_hash_of_the_same_bytes) {
    // The streaming digest only means anything if it equals the digest of the
    // finished object, so this compares it against a one-shot hash rather
    // than against itself.
    std::vector<uint8_t> payload(10000, 0);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>((i * 31 + 7) % 256);
    }

    DenseLayerDigest digest;
    // Fed in uneven chunks: a hash that mishandled its block boundary would
    // agree on a single call and disagree here.
    size_t at = 0;
    const size_t steps[5] = {1, 63, 64, 4096, 5776};
    for (int i = 0; i < 5; ++i) {
        MT_EXPECT_TRUE(digest.Absorb(at, &payload[at], steps[i]).ok());
        at += steps[i];
    }
    MT_EXPECT_EQ(static_cast<long long>(at), static_cast<long long>(payload.size()));

    const DenseLayerDescriptor out = digest.Descriptor();
    MT_EXPECT_EQ(static_cast<long long>(out.size), static_cast<long long>(payload.size()));
    MT_EXPECT_TRUE(out.digest == core::Sha256Digest(&payload[0], payload.size()));
}

MT_TEST(an_out_of_order_write_is_refused) {
    // Refused rather than seeked to. A hash absorbs bytes in the order it is
    // given them, so accepting a gap would produce a digest for a byte string
    // that was never stored -- and the upload would be rejected later, by the
    // registry, with nothing pointing back here.
    std::vector<uint8_t> payload(kPage, 0x5A);

    DenseLayerDigest digest;
    MT_EXPECT_TRUE(digest.Absorb(0, &payload[0], payload.size()).ok());

    // A gap.
    MT_EXPECT_TRUE(!digest.Absorb(2 * kPage, &payload[0], payload.size()).ok());
    // A repeat.
    MT_EXPECT_TRUE(!digest.Absorb(0, &payload[0], payload.size()).ok());
    // ...and the rejected writes left the state untouched, so the accumulator
    // is still usable rather than poisoned.
    MT_EXPECT_EQ(static_cast<long long>(digest.Descriptor().size),
                 static_cast<long long>(kPage));
    MT_EXPECT_TRUE(digest.Absorb(kPage, &payload[0], payload.size()).ok());
    MT_EXPECT_EQ(static_cast<long long>(digest.Descriptor().size),
                 static_cast<long long>(2 * kPage));
}

MT_TEST(the_descriptor_can_be_read_mid_stream) {
    // Rust clones the hasher to finalise, so `descriptor()` does not end the
    // stream. Reading it must not disturb what comes next.
    std::vector<uint8_t> payload(kPage, 0x11);

    DenseLayerDigest digest;
    MT_EXPECT_TRUE(digest.Absorb(0, &payload[0], payload.size()).ok());
    const DenseLayerDescriptor mid = digest.Descriptor();
    MT_EXPECT_TRUE(mid.digest == core::Sha256Digest(&payload[0], payload.size()));

    MT_EXPECT_TRUE(digest.Absorb(kPage, &payload[0], payload.size()).ok());
    const DenseLayerDescriptor end = digest.Descriptor();

    std::vector<uint8_t> both(payload);
    both.insert(both.end(), payload.begin(), payload.end());
    MT_EXPECT_TRUE(end.digest == core::Sha256Digest(&both[0], both.size()));
    MT_EXPECT_EQ(static_cast<long long>(end.size), static_cast<long long>(both.size()));
}

// ============================================================
// end to end
// ============================================================

MT_TEST(dense_export_rewrites_sparse_offsets_without_changing_logical_data) {
    // The sparse RW layout recovers which blocks the upper owns from the
    // file's extent map, so this case is meaningless where holes are a
    // filesystem heuristic. Skipped rather than asserted, matching the gate
    // Rust puts on the same test.
    if (!sys::SparseExtentsAreReliable()) {
        MT_EXPECT_TRUE(true);
        return;
    }

    const std::string dir = MakeTempDir();
    const std::string sparse_path = dir + "/snapshot.commit";
    const std::string dense_path = dir + "/dense.commit";

    const std::vector<uint8_t> first(static_cast<size_t>(kPage), 0xAB);
    const std::vector<uint8_t> second(static_cast<size_t>(kPage), 0xCD);
    MT_EXPECT_TRUE(BuildSparseLayer(sparse_path, first, second));

    // The layer has to be recognised as sparse, or `WriteDenseLayerTo` below
    // would be exporting something this module would never have been asked
    // to export.
    MT_EXPECT_TRUE(ShouldDenseExportLayer(sparse_path));

    {
        const core::Expected<std::shared_ptr<LocalFile>, std::string> dense_file =
            LocalFile::New(dense_path);
        MT_EXPECT_TRUE(dense_file.ok());
        if (!dense_file.ok()) {
            RemoveTreeBestEffort(dir);
            return;
        }
        MT_EXPECT_TRUE(WriteDenseLayerToFile(sparse_path, dense_file.value()).ok());
    }

    // The digest computed without storing anything, against the bytes that
    // were actually stored. This is the property the upload path relies on.
    const core::Expected<DenseLayerDescriptor, std::string> described =
        DescribeDenseLayer(sparse_path);
    MT_EXPECT_TRUE(described.ok());
    if (!described.ok()) {
        RemoveTreeBestEffort(dir);
        return;
    }

    std::vector<uint8_t> dense_bytes;
    MT_EXPECT_TRUE(SlurpFile(dense_path, &dense_bytes));
    MT_EXPECT_EQ(static_cast<long long>(described.value().size),
                 static_cast<long long>(dense_bytes.size()));
    MT_EXPECT_TRUE(described.value().digest ==
                   core::Sha256Digest(&dense_bytes[0], dense_bytes.size()));

    // The point of the exercise: the 64 MiB address space collapses to the
    // bytes that exist. Asserted as an upper bound rather than an exact size
    // so index layout changes do not make this brittle, but tight enough that
    // a dense form still carrying the hole fails.
    MT_EXPECT_TRUE(dense_bytes.size() < 1024 * 1024);

    // ...and the logical view is unchanged.
    const core::Expected<std::shared_ptr<LocalFile>, std::string> reopened =
        LocalFile::OpenRo(dense_path);
    MT_EXPECT_TRUE(reopened.ok());
    if (!reopened.ok()) {
        RemoveTreeBestEffort(dir);
        return;
    }
    const core::Expected<std::shared_ptr<LsmtReadOnlyFile>, std::string> dense =
        LsmtReadOnlyFile::Open(reopened.value());
    MT_EXPECT_TRUE(dense.ok());
    if (!dense.ok()) {
        RemoveTreeBestEffort(dir);
        return;
    }

    const core::Expected<ByteBuffer, std::string> got_first =
        dense.value()->ReadAt(0, static_cast<size_t>(kPage));
    MT_EXPECT_TRUE(got_first.ok());
    if (got_first.ok() && got_first.value().size() == first.size()) {
        MT_EXPECT_EQ(FirstMismatch(&got_first.value()[0], &first[0], first.size()),
                     static_cast<long long>(-1));
    }

    const core::Expected<ByteBuffer, std::string> got_second =
        dense.value()->ReadAt(kSecondOffset, static_cast<size_t>(kPage));
    MT_EXPECT_TRUE(got_second.ok());
    if (got_second.ok() && got_second.value().size() == second.size()) {
        MT_EXPECT_EQ(FirstMismatch(&got_second.value()[0], &second[0], second.size()),
                     static_cast<long long>(-1));
    }

    // The hole still reads as zeros -- the dense form dropped the *storage*,
    // not the address space.
    const core::Expected<ByteBuffer, std::string> hole =
        dense.value()->ReadAt(kPage, static_cast<size_t>(kPage));
    MT_EXPECT_TRUE(hole.ok());
    if (hole.ok()) {
        const std::vector<uint8_t> zeros(hole.value().size(), 0);
        MT_EXPECT_TRUE(!hole.value().empty());
        MT_EXPECT_EQ(FirstMismatch(&hole.value()[0], &zeros[0], zeros.size()),
                     static_cast<long long>(-1));
    }

    RemoveTreeBestEffort(dir);
}

MT_TEST(the_dense_form_keeps_the_sources_identity) {
    // This is the whole reason the export goes through
    // `CommitPreservingMetadata` rather than a plain compact: repacking a
    // layer must not re-identify it. Its uuid names it and its parent uuid
    // names the layer it sits on, so a dense copy that dropped either would
    // stop being recognised as that layer's child and the stack would be
    // rejected on open.
    //
    // Added after a mutation survived: zeroing the parent uuid on the way out
    // left every other case in this file passing, because they only look at
    // the data.
    if (!sys::SparseExtentsAreReliable()) {
        MT_EXPECT_TRUE(true);
        return;
    }

    const std::string dir = MakeTempDir();
    const std::string sparse_path = dir + "/stamped.commit";
    const std::string dense_path = dir + "/stamped-dense.commit";

    const std::string uuid = "11111111-2222-3333-4444-555555555555";
    const std::string parent = "66666666-7777-8888-9999-aaaaaaaaaaaa";
    const std::string tag = "dense-export-tag";

    {
        const core::Expected<std::shared_ptr<LocalFile>, std::string> data =
            LocalFile::New(sparse_path);
        MT_EXPECT_TRUE(data.ok());
        if (!data.ok()) {
            RemoveTreeBestEffort(dir);
            return;
        }
        const core::Expected<std::shared_ptr<LsmtFile>, std::string> sparse =
            LsmtFile::CreateWithMetadata(data.value(), std::shared_ptr<VirtualFile>(),
                                         kVirtualSize, kRwSparse, uuid, parent,
                                         reinterpret_cast<const uint8_t*>(tag.data()),
                                         tag.size());
        MT_EXPECT_TRUE(sparse.ok());
        if (!sparse.ok()) {
            RemoveTreeBestEffort(dir);
            return;
        }
        const std::vector<uint8_t> payload(static_cast<size_t>(kPage), 0xEE);
        MT_EXPECT_TRUE(sparse.value()->WriteAt(0, &payload[0], payload.size()).ok());
        MT_EXPECT_TRUE(sparse.value()->CloseSeal().ok());
    }

    {
        const core::Expected<std::shared_ptr<LocalFile>, std::string> dense_file =
            LocalFile::New(dense_path);
        MT_EXPECT_TRUE(dense_file.ok());
        if (!dense_file.ok()) {
            RemoveTreeBestEffort(dir);
            return;
        }
        MT_EXPECT_TRUE(WriteDenseLayerToFile(sparse_path, dense_file.value()).ok());
    }

    // Read the identity straight out of the dense layer's trailer. Compared
    // against the literals above rather than against the source layer, so a
    // mutation that dropped the field on *both* sides cannot agree.
    const core::Expected<std::shared_ptr<LocalFile>, std::string> reopened =
        LocalFile::OpenRo(dense_path);
    MT_EXPECT_TRUE(reopened.ok());
    if (!reopened.ok()) {
        RemoveTreeBestEffort(dir);
        return;
    }
    const core::Expected<uint64_t, std::string> size = reopened.value()->Size();
    MT_EXPECT_TRUE(size.ok());
    if (!size.ok()) {
        RemoveTreeBestEffort(dir);
        return;
    }
    const core::Expected<HeaderTrailer, std::string> trailer =
        VerifyHt(reopened.value().get(), true, size.value());
    MT_EXPECT_TRUE(trailer.ok());
    if (!trailer.ok()) {
        RemoveTreeBestEffort(dir);
        return;
    }

    const std::string got_uuid(reinterpret_cast<const char*>(trailer.value().uuid),
                               uuid.size());
    const std::string got_parent(reinterpret_cast<const char*>(trailer.value().parent_uuid),
                                 parent.size());
    const std::string got_tag(reinterpret_cast<const char*>(trailer.value().user_tag),
                              tag.size());
    MT_EXPECT_TRUE(got_uuid == uuid);
    MT_EXPECT_TRUE(got_parent == parent);
    MT_EXPECT_TRUE(got_tag == tag);

    RemoveTreeBestEffort(dir);
}

MT_TEST(a_non_sparse_layer_is_not_dense_exported) {
    // The classifier is what keeps a dense layer from being produced for a
    // file whose data is already packed. A non-sparse sealed layer must be
    // reported as "upload raw".
    if (!sys::SparseExtentsAreReliable()) {
        MT_EXPECT_TRUE(true);
        return;
    }

    const std::string dir = MakeTempDir();
    const std::string path = dir + "/packed.commit";

    const core::Expected<std::shared_ptr<LocalFile>, std::string> data = LocalFile::New(path);
    MT_EXPECT_TRUE(data.ok());
    if (!data.ok()) {
        RemoveTreeBestEffort(dir);
        return;
    }
    // The log-structured layout needs its own index file: it appends data and
    // records where each block went, rather than storing at the logical
    // offset the way the sparse layout does. `LsmtFile::Create` refuses
    // without one, which is the same requirement `open_upper` places on
    // `UpperMode::LogStructured`.
    const core::Expected<std::shared_ptr<LocalFile>, std::string> index =
        LocalFile::New(dir + "/packed.index");
    MT_EXPECT_TRUE(index.ok());
    if (!index.ok()) {
        RemoveTreeBestEffort(dir);
        return;
    }
    // `sparse_rw = false`: the layout whose data is already packed, so there
    // is no hole for a dense export to remove.
    const core::Expected<std::shared_ptr<LsmtFile>, std::string> packed =
        LsmtFile::Create(data.value(), index.value(), kVirtualSize, false);
    MT_EXPECT_TRUE(packed.ok());
    if (!packed.ok()) {
        RemoveTreeBestEffort(dir);
        return;
    }
    const std::vector<uint8_t> payload(static_cast<size_t>(kPage), 0x77);
    MT_EXPECT_TRUE(packed.value()->WriteAt(0, &payload[0], payload.size()).ok());
    MT_EXPECT_TRUE(packed.value()->CloseSeal().ok());

    MT_EXPECT_TRUE(!ShouldDenseExportLayer(path));
    RemoveTreeBestEffort(dir);
}

MT_TEST(an_unreadable_layer_is_reported_as_upload_raw) {
    // Not an error and not "sparse": a layer whose header cannot be read is
    // uploaded raw, which is larger but correct. Claiming sparse would hand a
    // non-sparse file to the exporter.
    MT_EXPECT_TRUE(!ShouldDenseExportLayer("/tmp/agentenv_de_definitely_absent"));
}

MT_TEST(describing_a_missing_layer_fails) {
    // Distinct from the classifier above: `DescribeDenseLayer` is asked for a
    // digest, and there is no digest of a file that is not there. Reporting
    // one would be worse than failing.
    MT_EXPECT_TRUE(!DescribeDenseLayer("/tmp/agentenv_de_definitely_absent").ok());
}

MT_MAIN
