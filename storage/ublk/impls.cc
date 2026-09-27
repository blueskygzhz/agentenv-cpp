// SPDX-License-Identifier: MIT
// Rust: storage/ublk/src/impls/*.rs — skeleton targets.
#include "agentenv/storage/ublk/impls.h"

namespace agentenv {
namespace storage {
namespace ublk {

namespace {
// A trivial zero-filled target used as a placeholder for both COW and overlaybd
// until the real backends are ported.
class ZeroTarget : public Target {
 public:
    explicit ZeroTarget(int64_t bytes) : bytes_(bytes) {}
  int64_t DiskBytes() const override { return bytes_; }
    IoResult Handle(const IoRequest& req) override {
        IoResult r;
    r.tag = req.tag;
      if (req.op == 0 /*read*/ && req.buffer && req.length) {
            for (uint32_t i = 0; i < req.length; ++i)
         static_cast<uint8_t*>(req.buffer)[i] = 0;
        }
        r.status = static_cast<int32_t>(req.length);
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
