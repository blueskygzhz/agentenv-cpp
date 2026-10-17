// SPDX-License-Identifier: MIT
// Rust: storage/ublk/src/impls/*.rs — skeleton targets.
#include "agentenv/storage/ublk/impls.h"

#include <cstring>

namespace agentenv {
namespace storage {
namespace ublk {

namespace {
// A placeholder for both COW and overlaybd until the real backends are
// ported. Reads are served as zeroes, which is a defensible answer for an
// unwritten device.
//
// Writes are *refused*, not silently dropped: reporting bytes written for
// data that went nowhere is the failure this codebase avoids everywhere else
// -- the guest would consider the write durable and only discover otherwise
// on a later read, by which time nothing connects the two.
class ZeroTarget : public Target {
 public:
    explicit ZeroTarget(int64_t bytes) : bytes_(bytes) {}
  int64_t DiskBytes() const override { return bytes_; }
    IoResult Handle(const IoRequest& req) override {
        IoResult r;
    r.tag = req.tag;
        if (req.op == 0 /*read*/) {
            if (req.buffer && req.length) {
                std::memset(req.buffer, 0, req.length);
            }
            r.status = static_cast<int32_t>(req.length);
            return r;
        }
        if (req.op == 2 /*flush*/) {
            // Nothing is buffered, so a flush has genuinely completed.
            r.status = 0;
            return r;
        }
        // -EROFS: this target has no backing store to write to.
        r.status = -30;
        return r;
    }
 private:
    int64_t bytes_;
};
}  // namespace

std::shared_ptr<Target> MakeBasicCowTarget(const BasicCowConfig& cfg) {
    return std::shared_ptr<Target>(new ZeroTarget(cfg.disk_bytes));
}

std::shared_ptr<Target> MakeOverlaybdTarget(const OverlaybdTargetConfig& cfg) {
    return std::shared_ptr<Target>(new ZeroTarget(cfg.disk_bytes));
}

}// namespace ublk
}  // namespace storage
}  // namespace agentenv
