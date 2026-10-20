// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/metrics.rs — the remote-read metric labels.
#ifndef AGENTENV_STORAGE_OVERLAYBD_METRICS_H_
#define AGENTENV_STORAGE_OVERLAYBD_METRICS_H_

#include <cstdint>
#include <string>

namespace agentenv {
namespace storage {
namespace overlaybd {
namespace metrics {

/// Rust `RemoteSource` — where a remote read was served from.
///
/// Modelled as a tag plus a host rather than a tagged union: the only variant
/// carrying data is `RegistryDirect`, and C++11 has no sum type that would
/// make the distinction cheaper to express than this.
class RemoteSource {
 public:
    enum Kind { kRegistryDirect, kRegistryP2p, kOssObject };

    /// Rust `registry_source` — direct when no accelerator is configured,
    /// P2P otherwise.
    ///
    /// The host is extracted from `url` only in the direct case, because that
    /// is the only one where it identifies the origin being read from.
    static RemoteSource Registry(const std::string& accelerate_address, const std::string& url);
    static RemoteSource Oss();

    Kind GetKind() const { return kind_; }

    /// Rust `as_label`.
    const char* AsLabel() const;

    /// Rust `registry_label` — the origin host, and `"none"` for anything
    /// that is not a direct registry read.
    ///
    /// P2P reads go through the local facade and OSS reads target an object
    /// store, so neither has an origin registry worth labelling. Keeping them
    /// at a constant also bounds the label's cardinality.
    const std::string& RegistryLabel() const;

 private:
    RemoteSource(Kind kind, const std::string& host) : kind_(kind), host_(host) {}

    Kind kind_;
    std::string host_;
};

/// Rust `RemoteReadOperation`.
enum RemoteReadOperation { kReadRange, kReadRangeInto };
const char* RemoteReadOperationLabel(RemoteReadOperation operation);

/// Rust `RemoteReadStatus`.
enum RemoteReadStatus { kReadOk, kReadRateLimited, kReadTimeout, kReadError };
const char* RemoteReadStatusLabel(RemoteReadStatus status);

/// Rust `RemoteReadStatus::from_http_status`.
RemoteReadStatus RemoteReadStatusFromHttpStatus(uint16_t status);

/// Rust `RemoteReadStatus::from_result` — classify a failure.
///
/// Deviation, and the reason this takes a string: Rust walks the error chain
/// and downcasts to `tokio::time::error::Elapsed`, `reqwest::Error` and
/// OpenDAL's error to classify. Errors here are strings, so the
/// classification matches on their text.
///
/// The match is locale-dependent where the message carries `strerror`:
/// measured, not assumed — under `LANG=zh_CN.UTF-8` `strerror(ETIMEDOUT)` is
/// not English. Tolerable here because the result is only a metric label and
/// nothing branches on it, so a misclassified failure still lands in the
/// `error` bucket and is still counted. A test like this must not decide
/// control flow; see `LocalLayerIsMissing` in `image_file.cc` for the shape
/// to use when it does.
///
/// An empty error means success, so callers do not need a separate overload.
RemoteReadStatus ClassifyRemoteReadError(const std::string& error);

/// Rust `record_remote_read` — duration histogram always, byte counter only
/// on success.
///
/// `bytes` is ignored unless `error` is empty, mirroring Rust's
/// `if let Ok(value) = result`: counting bytes for a failed read would inflate
/// the total with data that never arrived.
void RecordRemoteRead(const RemoteSource& source, RemoteReadOperation operation,
                      const std::string& error, uint64_t bytes, double elapsed_seconds);

/// Rust `record_remote_metadata` — duration histogram for a metadata probe.
void RecordRemoteMetadata(const RemoteSource& source, const std::string& error,
                          double elapsed_seconds);

// ---------------------------------------------------------------------------
// Rust: the ZFile read metrics of metrics.rs
// ---------------------------------------------------------------------------

/// Rust `ZFileCodec` — the `codec` label.
enum ZFileCodec { kZFileLz4, kZFileZstd };
const char* ZFileCodecLabel(ZFileCodec codec);

/// Rust `ZFileReadStatus` — the complete `status` label set of the pread
/// duration histogram.
enum ZFileReadStatus { kZFileReadOk, kZFileReadError };
const char* ZFileReadStatusLabel(ZFileReadStatus status);

/// Rust `ZFileReadStats` — exact counters accumulated across one logical
/// `pread`.
///
/// A plain aggregate on purpose: the read loop increments these as stack
/// integers and the metric backend is touched once, after the loop. Reporting
/// per block would put a map lookup and a string build inside the hot path.
struct ZFileReadStats {
    /// Bytes returned to the caller; set only on success.
    uint64_t output_bytes;
    /// Encoded bytes submitted to exact reads, retry re-reads included.
    uint64_t encoded_bytes;
    /// Logical blocks touched by the initial request. Retries do not
    /// re-increment this, so `blocks` stays a measure of the request's span
    /// rather than of the work done.
    uint64_t blocks;
    /// Initial merged-range reads plus single-block retry reads. A zfile-level
    /// read count, **not** a syscall or HTTP attempt count: one merged read
    /// may be filled by several backing calls.
    uint64_t read_batches;
    /// Block re-reads after a CRC32C mismatch.
    uint64_t checksum_retries;
    /// Block re-reads after a decompress failure.
    uint64_t decompress_retries;

    ZFileReadStats()
        : output_bytes(0), encoded_bytes(0), blocks(0), read_batches(0), checksum_retries(0),
          decompress_retries(0) {}
};

/// Rust `ZFileReadMetrics::record` — the exact counters of one logical pread.
///
/// Called once per pread, on success and on every error exit alike. A counter
/// is emitted only when its value is non-zero, matching Rust: a zero
/// increment would register the series without carrying information.
void RecordZFileRead(ZFileCodec codec, const ZFileReadStats& stats);

/// Rust `ZFileReadMetrics::record_durations` — the sampled duration
/// histograms of one logical pread.
///
/// Called only for preads the sampler selected, so the exported `_count` of
/// both histograms is a **sample count, not an operation count**.
/// `decompress_seconds` is the *sum* of every per-block decode inside this one
/// pread, retry decodes included -- never a per-block observation.
void RecordZFileReadDurations(ZFileCodec codec, ZFileReadStatus status, double pread_seconds,
                              double decompress_seconds);

/// Rust `ZFILE_PREAD_SAMPLE_INTERVAL`.
const uint64_t kZFilePreadSampleInterval = 1024;

/// Rust `sample_zfile_pread` — true for one pread in
/// `kZFilePreadSampleInterval` on each thread.
///
/// Per-thread rather than global: a shared counter would be a contended
/// atomic on the hot read path, and the sample only has to be unbiased, not
/// globally sequenced.
bool SampleZFilePread();

/// The ZFile series the recorder emits.
enum ZFileSeriesId {
    kZFileSeriesOutputBytes,
    kZFileSeriesEncodedBytes,
    kZFileSeriesBlocks,
    kZFileSeriesReadBatches,
    kZFileSeriesRetriesChecksum,
    kZFileSeriesRetriesDecompress,
    kZFileSeriesDecompressDuration
};

/// Test seam onto the composed series names.
///
/// The metric facade is write-only -- `observability::CounterIncr` takes a
/// name and returns nothing -- so reading the composed name back is the only
/// way to check two things that are otherwise invisible: that the series
/// table is indexed the right way round, and that the label text is what Rust
/// exports. Both are silent failures otherwise, because a wrong name does not
/// error, it just publishes a series nobody is looking at.
std::string ZFileSeriesNameForTest(ZFileSeriesId id, ZFileCodec codec);
std::string ZFilePreadDurationNameForTest(ZFileCodec codec, ZFileReadStatus status);

}  // namespace metrics
}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_METRICS_H_
