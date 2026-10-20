// SPDX-License-Identifier: MIT
// Tests for the ZFile read metrics: the label values and the pread sampler.
//
// Scope, stated plainly: this covers the label spelling and the sampler.
// It does **not** cover the read loop's accounting (`PreadObserved` filling
// `ZFileReadStats`), because there is no way to build a zfile here -- the
// encode half of the codec is deliberately not ported, so nothing in this
// codebase writes one. See the note at the end of this file.
//
// Rust: storage/overlaybd/src/metrics.rs, storage/overlaybd/src/compression/
//       zfile.rs (`sample_zfile_pread`)
#include "microtest.h"

#include <pthread.h>

#include <string>
#include <vector>

#include "agentenv/storage/overlaybd/metrics.h"

using namespace agentenv::storage::overlaybd;

// ============================================================
// labels
// ============================================================

MT_TEST(codec_and_status_labels_match_the_rust_spelling) {
    // These are label *values*, so they are part of the exported series
    // identity: renaming one silently splits a time series in two and the
    // dashboards built on the old name go flat rather than erroring.
    MT_EXPECT_TRUE(std::string(metrics::ZFileCodecLabel(metrics::kZFileLz4)) == "lz4");
    MT_EXPECT_TRUE(std::string(metrics::ZFileCodecLabel(metrics::kZFileZstd)) == "zstd");
    MT_EXPECT_TRUE(std::string(metrics::ZFileReadStatusLabel(metrics::kZFileReadOk)) == "ok");
    MT_EXPECT_TRUE(std::string(metrics::ZFileReadStatusLabel(metrics::kZFileReadError)) ==
                   "error");
}

MT_TEST(read_stats_start_at_zero) {
    // The read loop adds into these, so a non-zero default would report work
    // that never happened on the very first pread.
    const metrics::ZFileReadStats stats;
    MT_EXPECT_EQ(static_cast<long long>(stats.output_bytes), static_cast<long long>(0));
    MT_EXPECT_EQ(static_cast<long long>(stats.encoded_bytes), static_cast<long long>(0));
    MT_EXPECT_EQ(static_cast<long long>(stats.blocks), static_cast<long long>(0));
    MT_EXPECT_EQ(static_cast<long long>(stats.read_batches), static_cast<long long>(0));
    MT_EXPECT_EQ(static_cast<long long>(stats.checksum_retries), static_cast<long long>(0));
    MT_EXPECT_EQ(static_cast<long long>(stats.decompress_retries), static_cast<long long>(0));
}

// ============================================================
// the sampler
// ============================================================

MT_TEST(the_sampler_selects_one_pread_in_the_interval) {
    // The sampled count is what the duration histograms' `_count` reports, so
    // the rate is part of the metric's meaning: a sampler firing twice as
    // often would make every rate derived from `_count` wrong by 2x without
    // looking wrong.
    const uint64_t interval = metrics::kZFilePreadSampleInterval;
    MT_EXPECT_EQ(static_cast<long long>(interval), static_cast<long long>(1024));

    // Three full intervals, so the result cannot be satisfied by a sampler
    // that happens to fire once and then never again.
    uint64_t selected = 0;
    std::vector<uint64_t> at;
    for (uint64_t i = 1; i <= 3 * interval; ++i) {
        if (metrics::SampleZFilePread()) {
            ++selected;
            at.push_back(i);
        }
    }

    MT_EXPECT_EQ(static_cast<long long>(selected), static_cast<long long>(3));
    // ...and they are evenly spaced, which a sampler using a random draw
    // would not be. This is the assertion that distinguishes the counter from
    // "fires roughly often enough".
    if (at.size() == 3) {
        MT_EXPECT_EQ(static_cast<long long>(at[0]), static_cast<long long>(interval));
        MT_EXPECT_EQ(static_cast<long long>(at[1]), static_cast<long long>(2 * interval));
        MT_EXPECT_EQ(static_cast<long long>(at[2]), static_cast<long long>(3 * interval));
    }
}

namespace {

struct SamplerProbe {
    uint64_t calls;
    uint64_t selected;
};

void* RunSampler(void* arg) {
    SamplerProbe* probe = static_cast<SamplerProbe*>(arg);
    for (uint64_t i = 0; i < probe->calls; ++i) {
        if (metrics::SampleZFilePread()) ++probe->selected;
    }
    return 0;
}

}  // namespace

MT_TEST(the_sampler_counts_per_thread) {
    // Per-thread by design: a shared counter would be a contended atomic on
    // the hot read path.
    //
    // Getting a discriminating assertion here took a second attempt. The
    // obvious one -- two threads each doing a full interval, each expecting
    // one sample -- has no teeth at all, because *any* run of `interval`
    // consecutive values contains exactly one multiple of `interval`. A
    // shared counter produces 1 + 1 just as readily, and a mutation making
    // the counter shared survived that version of this test.
    //
    // What actually separates them is the *starting position*. The main
    // thread is first walked to a boundary and then `interval - 1` calls
    // past it, so a shared counter would be one call away from firing. Each
    // child then makes a single call:
    //
    //   per-thread -> the child starts at 0, one call, no sample;
    //   shared     -> the first child's call completes the interval and fires.
    const uint64_t interval = metrics::kZFilePreadSampleInterval;

    // Walk to a boundary first, so this test does not depend on how many
    // calls the cases above happened to make.
    for (uint64_t i = 0; i < 2 * interval; ++i) {
        if (metrics::SampleZFilePread()) break;
    }
    // ...and then to one short of the next one.
    for (uint64_t i = 0; i + 1 < interval; ++i) metrics::SampleZFilePread();

    SamplerProbe first;
    first.calls = 1;
    first.selected = 0;
    SamplerProbe second;
    second.calls = 1;
    second.selected = 0;

    // Sequential join, not concurrent: with a shared non-atomic counter the
    // two children would race, and a racing failure is a flaky test rather
    // than a verdict. Run one at a time, the shared case is deterministic.
    pthread_t t1;
    MT_EXPECT_TRUE(pthread_create(&t1, 0, RunSampler, &first) == 0);
    MT_EXPECT_TRUE(pthread_join(t1, 0) == 0);

    pthread_t t2;
    MT_EXPECT_TRUE(pthread_create(&t2, 0, RunSampler, &second) == 0);
    MT_EXPECT_TRUE(pthread_join(t2, 0) == 0);

    MT_EXPECT_EQ(static_cast<long long>(first.selected), static_cast<long long>(0));
    MT_EXPECT_EQ(static_cast<long long>(second.selected), static_cast<long long>(0));
}

// ============================================================
// recording
// ============================================================

MT_TEST(every_series_name_matches_what_rust_exports) {
    // These are the exported series identities. A wrong one does not fail
    // anything at runtime -- the facade takes any string -- it just publishes
    // a metric nobody queries, so the name is only checkable by reading it
    // back.
    //
    // The first version of this case only called the recorders and asserted
    // `true`, reasoning that a bad table index would show up under a
    // sanitiser. It did not: a mutation changing the codec index to an
    // out-of-range 3 survived, because nothing was looking. This version
    // reads the composed names.
    MT_EXPECT_TRUE(metrics::ZFileSeriesNameForTest(metrics::kZFileSeriesOutputBytes,
                                                   metrics::kZFileLz4) ==
                   "agentenv_overlaybd_zfile_output_bytes_total{codec=\"lz4\"}");
    MT_EXPECT_TRUE(metrics::ZFileSeriesNameForTest(metrics::kZFileSeriesEncodedBytes,
                                                   metrics::kZFileZstd) ==
                   "agentenv_overlaybd_zfile_encoded_bytes_total{codec=\"zstd\"}");
    MT_EXPECT_TRUE(metrics::ZFileSeriesNameForTest(metrics::kZFileSeriesBlocks,
                                                   metrics::kZFileLz4) ==
                   "agentenv_overlaybd_zfile_blocks_total{codec=\"lz4\"}");
    MT_EXPECT_TRUE(metrics::ZFileSeriesNameForTest(metrics::kZFileSeriesReadBatches,
                                                   metrics::kZFileZstd) ==
                   "agentenv_overlaybd_zfile_read_batches_total{codec=\"zstd\"}");

    // The two retry reasons are one series split by a label, as Rust spells
    // it -- not two series names.
    MT_EXPECT_TRUE(metrics::ZFileSeriesNameForTest(metrics::kZFileSeriesRetriesChecksum,
                                                   metrics::kZFileLz4) ==
                   "agentenv_overlaybd_zfile_retries_total{codec=\"lz4\",reason=\"checksum\"}");
    MT_EXPECT_TRUE(metrics::ZFileSeriesNameForTest(metrics::kZFileSeriesRetriesDecompress,
                                                   metrics::kZFileLz4) ==
                   "agentenv_overlaybd_zfile_retries_total{codec=\"lz4\",reason=\"decompress\"}");

    // The decompress histogram carries no `status`, unlike the pread one.
    MT_EXPECT_TRUE(metrics::ZFileSeriesNameForTest(metrics::kZFileSeriesDecompressDuration,
                                                   metrics::kZFileZstd) ==
                   "agentenv_overlaybd_zfile_decompress_duration_seconds{codec=\"zstd\"}");

    MT_EXPECT_TRUE(
        metrics::ZFilePreadDurationNameForTest(metrics::kZFileLz4, metrics::kZFileReadOk) ==
        "agentenv_overlaybd_zfile_pread_duration_seconds{codec=\"lz4\",status=\"ok\"}");
    MT_EXPECT_TRUE(
        metrics::ZFilePreadDurationNameForTest(metrics::kZFileZstd, metrics::kZFileReadError) ==
        "agentenv_overlaybd_zfile_pread_duration_seconds{codec=\"zstd\",status=\"error\"}");
}

MT_TEST(recording_accepts_the_zero_and_non_zero_cases) {
    // The facade is write-only, so this asserts only that both branches of
    // every non-zero guard are reachable without crashing -- the all-zero
    // call must take the false branch of all six rather than publishing six
    // empty series. What the guards *do* is pinned by the names above and by
    // the read-loop accounting, which is not covered here (see the file
    // header).
    metrics::ZFileReadStats stats;
    stats.output_bytes = 4096;
    stats.encoded_bytes = 1024;
    stats.blocks = 2;
    stats.read_batches = 1;
    stats.checksum_retries = 1;
    stats.decompress_retries = 1;

    const metrics::ZFileCodec codecs[2] = {metrics::kZFileLz4, metrics::kZFileZstd};
    const metrics::ZFileReadStatus statuses[2] = {metrics::kZFileReadOk,
                                                  metrics::kZFileReadError};
    for (int c = 0; c < 2; ++c) {
        metrics::RecordZFileRead(codecs[c], stats);
        metrics::RecordZFileRead(codecs[c], metrics::ZFileReadStats());
        for (int s = 0; s < 2; ++s) {
            metrics::RecordZFileReadDurations(codecs[c], statuses[s], 0.001, 0.0005);
        }
    }
    MT_EXPECT_TRUE(true);
}

MT_MAIN
