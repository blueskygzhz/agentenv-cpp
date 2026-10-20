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

}  // namespace metrics
}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
