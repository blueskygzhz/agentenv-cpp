// SPDX-License-Identifier: MIT
// Rust: storage/overlaybd/src/download_gate.rs
#include "agentenv/storage/overlaybd/download_gate.h"

#include <atomic>

namespace agentenv {
namespace storage {
namespace overlaybd {

namespace {
std::atomic<int64_t>& fg_inflight() {
    static std::atomic<int64_t> v(0);
    return v;
}
std::atomic<int64_t>& bk_inflight() {
    static std::atomic<int64_t> v(0);
    return v;
}
}  // namespace

void DownloadGate::EnterForeground() { fg_inflight().fetch_add(1, std::memory_order_relaxed); }
void DownloadGate::ExitForeground()  { fg_inflight().fetch_sub(1, std::memory_order_relaxed); }
void DownloadGate::EnterBackground() { bk_inflight().fetch_add(1, std::memory_order_relaxed); }
void DownloadGate::ExitBackground()  { bk_inflight().fetch_sub(1, std::memory_order_relaxed); }
void DownloadGate::MarkReady(const std::string& /*device_key*/) {
    // TODO: signal the ready registry so parked background downloads proceed.
}
int64_t DownloadGate::ForegroundInflight() { return fg_inflight().load(std::memory_order_relaxed); }
int64_t DownloadGate::BackgroundInflight() { return bk_inflight().load(std::memory_order_relaxed); }

}  // namespace overlaybd
}  // namespace storage
}  // namespace agentenv
