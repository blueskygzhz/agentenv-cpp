// SPDX-License-Identifier: MIT
#include "agentenv/storage/ublk/target.h"

namespace agentenv {
namespace storage {
namespace ublk {

class StubRunner final : public TargetRunner {
 public:
    core::Expected<core::Unit, core::AnyError> Run(std::shared_ptr<Target>) override {
        return core::make_unexpected(core::err(
            "ublk::TargetRunner::Run: not implemented "
            "(build with AGENTENV_WITH_LIBURING and a Linux ≥ 6.8 kernel)"));
    }
    void Stop() override {}
};

std::unique_ptr<TargetRunner> MakeRunner() {
    return std::unique_ptr<TargetRunner>(new StubRunner());
}

}  // namespace ublk
}  // namespace storage
}  // namespace agentenv
