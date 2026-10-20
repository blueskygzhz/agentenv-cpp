// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/metrics.rs
#include "agentenv/storage/overlaybd/metrics.h"

#include <cstddef>
#include <utility>
#include <vector>

#include "agentenv/observability/metrics.h"

namespace agentenv {
namespace storage {
namespace overlaybd {
namespace metrics {

namespace {

const char kRemoteReadDuration[] = "agentenv_overlaybd_remote_read_duration_seconds";
const char kRemoteReadBytes[] = "agentenv_overlaybd_remote_read_bytes_total";
const char kRemoteMetadataDuration[] = "agentenv_overlaybd_remote_metadata_duration_seconds";

const char kZFileOutputBytes[] = "agentenv_overlaybd_zfile_output_bytes_total";
const char kZFileEncodedBytes[] = "agentenv_overlaybd_zfile_encoded_bytes_total";
const char kZFileBlocks[] = "agentenv_overlaybd_zfile_blocks_total";
const char kZFileReadBatches[] = "agentenv_overlaybd_zfile_read_batches_total";
const char kZFileRetries[] = "agentenv_overlaybd_zfile_retries_total";
const char kZFilePreadDuration[] = "agentenv_overlaybd_zfile_pread_duration_seconds";
const char kZFileDecompressDuration[] = "agentenv_overlaybd_zfile_decompress_duration_seconds";

/// The `metrics!` macro takes name + label pairs; the C++ facade is
/// label-free, so labels are folded into the series name in the standard
/// Prometheus `name{k="v",..}` text form. Same convention as
/// `observability/prometheus.cc`, deliberately -- two spellings of the same
/// series would not aggregate.
std::string WithLabels(const char* metric,
                       const std::vector<std::pair<const char*, std::string> >& labels) {
    std::string out(metric);
    if (labels.empty()) return out;
    out += "{";
    for (size_t i = 0; i < labels.size(); ++i) {
        if (i) out += ",";
        out += labels[i].first;
        out += "=\"";
        out += labels[i].second;
        out += "\"";
    }
    out += "}";
    return out;
}

/// Rust `normalize_host` — a low-cardinality host label.
///
/// Strips scheme, userinfo, path, query and fragment, keeping `host[:port]`.
/// The point is cardinality: a label carrying the full URL would open a new
/// time series per blob digest.
std::string NormalizeHost(const std::string& raw) {
    std::string rest = raw;
    const std::string::size_type scheme = rest.find("://");
    if (scheme != std::string::npos) rest = rest.substr(scheme + 3);

    const std::string::size_type cut = rest.find_first_of("/?#");
    std::string authority = cut == std::string::npos ? rest : rest.substr(0, cut);

    // Userinfo may itself contain '@'; Rust uses `rsplit_once`, so the last
    // one wins.
    const std::string::size_type at = authority.rfind('@');
    if (at != std::string::npos) authority = authority.substr(at + 1);

    std::string::size_type b = 0;
    std::string::size_type e = authority.size();
    while (b < e && (authority[b] == ' ' || authority[b] == '\t')) ++b;
    while (e > b && (authority[e - 1] == ' ' || authority[e - 1] == '\t')) --e;
    const std::string host = authority.substr(b, e - b);
    return host.empty() ? std::string("unknown") : host;
}

bool Contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

const std::string kNoneLabel = "none";

}  // namespace

// ---------------------------------------------------------------------------
// RemoteSource
// ---------------------------------------------------------------------------

RemoteSource RemoteSource::Registry(const std::string& accelerate_address,
                                    const std::string& url) {
    // Rust tests `accelerate_address.trim().is_empty()`, so an address of
    // only whitespace counts as unconfigured.
    std::string::size_type b = 0;
    std::string::size_type e = accelerate_address.size();
    while (b < e && (accelerate_address[b] == ' ' || accelerate_address[b] == '\t' ||
                     accelerate_address[b] == '\n' || accelerate_address[b] == '\r')) {
        ++b;
    }
    while (e > b && (accelerate_address[e - 1] == ' ' || accelerate_address[e - 1] == '\t' ||
                     accelerate_address[e - 1] == '\n' || accelerate_address[e - 1] == '\r')) {
        --e;
    }
    if (b == e) return RemoteSource(kRegistryDirect, NormalizeHost(url));
    return RemoteSource(kRegistryP2p, std::string());
}

RemoteSource RemoteSource::Oss() { return RemoteSource(kOssObject, std::string()); }

const char* RemoteSource::AsLabel() const {
    switch (kind_) {
        case kRegistryDirect: return "registry_direct";
        case kRegistryP2p: return "registry_p2p";
        case kOssObject: return "oss_object";
    }
    return "registry_direct";
}

const std::string& RemoteSource::RegistryLabel() const {
    if (kind_ == kRegistryDirect) return host_;
    return kNoneLabel;
}

// ---------------------------------------------------------------------------
// Labels
// ---------------------------------------------------------------------------

const char* RemoteReadOperationLabel(RemoteReadOperation operation) {
    switch (operation) {
        case kReadRange: return "read_range";
        case kReadRangeInto: return "read_range_into";
    }
    return "read_range";
}

const char* RemoteReadStatusLabel(RemoteReadStatus status) {
    switch (status) {
        case kReadOk: return "ok";
        case kReadRateLimited: return "rate_limited";
        case kReadTimeout: return "timeout";
        case kReadError: return "error";
    }
    return "error";
}

RemoteReadStatus RemoteReadStatusFromHttpStatus(uint16_t status) {
    switch (status) {
        case 429: return kReadRateLimited;
        case 408:
        case 504: return kReadTimeout;
        default: return kReadError;
    }
}

RemoteReadStatus ClassifyRemoteReadError(const std::string& error) {
    if (error.empty()) return kReadOk;

    // Ordered as Rust's chain walk is: a timeout is recognised first, because
    // a timed-out request may also carry a status.
    if (Contains(error, "timed out") || Contains(error, "timeout") ||
        Contains(error, "Timeout") || Contains(error, "deadline has elapsed") ||
        Contains(error, "ETIMEDOUT")) {
        return kReadTimeout;
    }
    if (Contains(error, "429") || Contains(error, "rate limit") ||
        Contains(error, "RateLimited") || Contains(error, "Too Many Requests") ||
        Contains(error, "SlowDown")) {
        return kReadRateLimited;
    }
    if (Contains(error, "408") || Contains(error, "504") ||
        Contains(error, "Gateway Time-out") || Contains(error, "Request Timeout")) {
        return kReadTimeout;
    }
    return kReadError;
}

// ---------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------

void RecordRemoteRead(const RemoteSource& source, RemoteReadOperation operation,
                      const std::string& error, uint64_t bytes, double elapsed_seconds) {
    const RemoteReadStatus status = ClassifyRemoteReadError(error);

    std::vector<std::pair<const char*, std::string> > labels;
    labels.push_back(std::make_pair("source", std::string(source.AsLabel())));
    labels.push_back(std::make_pair("registry", source.RegistryLabel()));
    labels.push_back(std::make_pair("operation", std::string(RemoteReadOperationLabel(operation))));
    labels.push_back(std::make_pair("status", std::string(RemoteReadStatusLabel(status))));
    observability::HistogramObserve(WithLabels(kRemoteReadDuration, labels), elapsed_seconds);

    // Only on success, as Rust's `if let Ok(value)` arranges: counting bytes
    // for a failed read would inflate the total with data that never arrived.
    if (status != kReadOk) return;
    labels.pop_back();  // the byte counter carries no `status`
    observability::CounterIncr(WithLabels(kRemoteReadBytes, labels),
                               static_cast<int64_t>(bytes));
}

void RecordRemoteMetadata(const RemoteSource& source, const std::string& error,
                          double elapsed_seconds) {
    const RemoteReadStatus status = ClassifyRemoteReadError(error);

    std::vector<std::pair<const char*, std::string> > labels;
    labels.push_back(std::make_pair("source", std::string(source.AsLabel())));
    labels.push_back(std::make_pair("registry", source.RegistryLabel()));
    labels.push_back(std::make_pair("status", std::string(RemoteReadStatusLabel(status))));
    observability::HistogramObserve(WithLabels(kRemoteMetadataDuration, labels), elapsed_seconds);
}

// ---------------------------------------------------------------------------
// ZFile read metrics
// ---------------------------------------------------------------------------

const char* ZFileCodecLabel(ZFileCodec codec) {
    return codec == kZFileZstd ? "zstd" : "lz4";
}

const char* ZFileReadStatusLabel(ZFileReadStatus status) {
    return status == kZFileReadError ? "error" : "ok";
}

bool SampleZFilePread() {
    // Thread-local, as in Rust: a shared counter would be a contended atomic
    // on the hot read path, and an unbiased sample does not need a global
    // sequence. `static` inside the function so no two translation units can
    // disagree about the counter.
    static __thread uint64_t seen = 0;
    ++seen;
    return (seen % kZFilePreadSampleInterval) == 0;
}

namespace {

/// Every ZFile series name, built once.
///
/// This is the C++ stand-in for Rust's `OnceLock<Counter>` slots, and it is
/// there for the same reason Rust bothers: the comment on `zcounter!` states
/// that key construction happens once per reader, after which a logical
/// `pread` pays only an atomic increment. Formatting the name on each call
/// would put several string allocations into the zfile read loop -- a loop
/// whose whole design (merged batch reads, decode straight into the caller's
/// buffer) exists to avoid exactly that kind of per-read cost.
///
/// Every name is fully determined by the codec plus a fixed label, so the
/// whole set is small and enumerable. Function-local statics, so it costs
/// nothing until the first zfile read; C++11 makes their initialisation
/// thread-safe.
struct ZFileSeries {
    // Indexed by codec: [lz4, zstd].
    std::string output_bytes[2];
    std::string encoded_bytes[2];
    std::string blocks[2];
    std::string read_batches[2];
    std::string retries_checksum[2];
    std::string retries_decompress[2];
    // Indexed [codec][status], status being [ok, error].
    std::string pread_duration[2][2];
    std::string decompress_duration[2];

    ZFileSeries() {
        const ZFileCodec codecs[2] = {kZFileLz4, kZFileZstd};
        const ZFileReadStatus statuses[2] = {kZFileReadOk, kZFileReadError};
        for (int c = 0; c < 2; ++c) {
            std::vector<std::pair<const char*, std::string> > base;
            base.push_back(std::make_pair("codec", std::string(ZFileCodecLabel(codecs[c]))));

            output_bytes[c] = WithLabels(kZFileOutputBytes, base);
            encoded_bytes[c] = WithLabels(kZFileEncodedBytes, base);
            blocks[c] = WithLabels(kZFileBlocks, base);
            read_batches[c] = WithLabels(kZFileReadBatches, base);
            decompress_duration[c] = WithLabels(kZFileDecompressDuration, base);

            std::vector<std::pair<const char*, std::string> > crc(base);
            crc.push_back(std::make_pair("reason", std::string("checksum")));
            retries_checksum[c] = WithLabels(kZFileRetries, crc);

            std::vector<std::pair<const char*, std::string> > dec(base);
            dec.push_back(std::make_pair("reason", std::string("decompress")));
            retries_decompress[c] = WithLabels(kZFileRetries, dec);

            for (int st = 0; st < 2; ++st) {
                std::vector<std::pair<const char*, std::string> > with_status(base);
                with_status.push_back(
                    std::make_pair("status", std::string(ZFileReadStatusLabel(statuses[st]))));
                pread_duration[c][st] = WithLabels(kZFilePreadDuration, with_status);
            }
        }
    }
};

const ZFileSeries& Series() {
    static const ZFileSeries table;
    return table;
}

int CodecIndex(ZFileCodec codec) { return codec == kZFileZstd ? 1 : 0; }
int StatusIndex(ZFileReadStatus status) { return status == kZFileReadError ? 1 : 0; }

}  // namespace

void RecordZFileRead(ZFileCodec codec, const ZFileReadStats& stats) {
    const ZFileSeries& s = Series();
    const int c = CodecIndex(codec);

    // Each guarded by non-zero, as Rust's `if stats.x > 0` arranges:
    // registering a series with a zero increment publishes a metric that
    // carries nothing and cannot be told apart from a genuine zero.
    if (stats.output_bytes > 0) {
        observability::CounterIncr(s.output_bytes[c], static_cast<int64_t>(stats.output_bytes));
    }
    if (stats.encoded_bytes > 0) {
        observability::CounterIncr(s.encoded_bytes[c], static_cast<int64_t>(stats.encoded_bytes));
    }
    if (stats.blocks > 0) {
        observability::CounterIncr(s.blocks[c], static_cast<int64_t>(stats.blocks));
    }
    if (stats.read_batches > 0) {
        observability::CounterIncr(s.read_batches[c], static_cast<int64_t>(stats.read_batches));
    }

    // The two retry counters are one series split by `reason`, exactly as
    // Rust spells it -- telling checksum corruption from decode corruption
    // needs them side by side.
    if (stats.checksum_retries > 0) {
        observability::CounterIncr(s.retries_checksum[c],
                                   static_cast<int64_t>(stats.checksum_retries));
    }
    if (stats.decompress_retries > 0) {
        observability::CounterIncr(s.retries_decompress[c],
                                   static_cast<int64_t>(stats.decompress_retries));
    }
}

void RecordZFileReadDurations(ZFileCodec codec, ZFileReadStatus status, double pread_seconds,
                              double decompress_seconds) {
    const ZFileSeries& s = Series();
    const int c = CodecIndex(codec);
    observability::HistogramObserve(s.pread_duration[c][StatusIndex(status)], pread_seconds);
    // No `status` on this one, matching Rust: the decode time of a failed
    // pread is still decode time, and splitting it would halve each bucket's
    // sample count for a question nobody asks.
    observability::HistogramObserve(s.decompress_duration[c], decompress_seconds);
}

std::string ZFileSeriesNameForTest(ZFileSeriesId id, ZFileCodec codec) {
    const ZFileSeries& s = Series();
    // Deliberately through the same `CodecIndex` the recorders use, so a
    // wrong index is observable here rather than only in whatever the
    // recorder happened to publish.
    const int c = CodecIndex(codec);
    switch (id) {
        case kZFileSeriesOutputBytes:
            return s.output_bytes[c];
        case kZFileSeriesEncodedBytes:
            return s.encoded_bytes[c];
        case kZFileSeriesBlocks:
            return s.blocks[c];
        case kZFileSeriesReadBatches:
            return s.read_batches[c];
        case kZFileSeriesRetriesChecksum:
            return s.retries_checksum[c];
        case kZFileSeriesRetriesDecompress:
            return s.retries_decompress[c];
        case kZFileSeriesDecompressDuration:
            return s.decompress_duration[c];
    }
    return std::string();
}

std::string ZFilePreadDurationNameForTest(ZFileCodec codec, ZFileReadStatus status) {
    const ZFileSeries& s = Series();
    return s.pread_duration[CodecIndex(codec)][StatusIndex(status)];
}

}  // namespace metrics
}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
