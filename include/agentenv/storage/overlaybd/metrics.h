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

}  // namespace metrics
}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
#endif  // AGENTENV_STORAGE_OVERLAYBD_METRICS_H_
