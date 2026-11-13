// SPDX-License-Identifier: MIT
// Rust: the `reqwest` client inside `src/overlaybd/p2p/facade.rs`.
#ifndef AGENTENV_OVERLAYBD_P2P_ORIGIN_FETCHER_H_
#define AGENTENV_OVERLAYBD_P2P_ORIGIN_FETCHER_H_

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "agentenv/core/expected.h"
#include "agentenv/overlaybd/p2p/facade.h"
#include "agentenv/overlaybd/p2p/http_routes.h"

namespace agentenv {
namespace overlaybd {
namespace p2p {

/// The production `OriginRangeFetcher`: a ranged GET against the layer's
/// origin, over libcurl so HTTPS works.
///
/// This is the facade's fallback path, and it carries the whole correctness of
/// a P2P miss: `HandleOriginRequest` consults peers first and comes here when
/// they do not have the layer, which on a cold cluster is every read.
class CurlOriginRangeFetcher : public OriginRangeFetcher {
 public:
    /// `timeout_secs` bounds the whole exchange. `max_range_bytes` is the
    /// ceiling the facade enforces; a request above it is refused rather than
    /// answered short, for the reason `kMaxFacadeRangeBytes` records.
    explicit CurlOriginRangeFetcher(long timeout_secs = 30,
                                    std::size_t max_range_bytes = kMaxFacadeRangeBytes)
        : timeout_secs_(timeout_secs), max_range_bytes_(max_range_bytes) {}

    core::Expected<OriginRangeResult, std::string> FetchRange(
        const std::string& origin_url, const RequestRange& range,
        const std::vector<std::pair<std::string, std::string> >& forwarded) override;

 private:
    long timeout_secs_;
    std::size_t max_range_bytes_;
};

}  // namespace p2p
}  // namespace overlaybd
}  // namespace agentenv
#endif  // AGENTENV_OVERLAYBD_P2P_ORIGIN_FETCHER_H_
