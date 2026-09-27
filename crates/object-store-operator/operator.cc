// SPDX-License-Identifier: MIT
#include "agentenv/object-store-operator/operator.h"

namespace agentenv {
namespace object_store {

class MockOp final : public Operator {
 public:
    core::Expected<core::Unit, core::AnyError>
    Put(const std::string&, const std::string&) override { return core::Unit{}; }
    core::Expected<core::Unit, core::AnyError>
    Get(const std::string&, const std::string&) override { return core::Unit{}; }
    core::Expected<std::vector<std::string>, core::AnyError>
    List(const std::string&) override { return std::vector<std::string>{}; }
    core::Expected<core::Unit, core::AnyError>
    Delete(const std::string&) override { return core::Unit{}; }
};

std::unique_ptr<Operator> MakeOperator(const std::string& /*kind*/,
                                        const std::string& /*config*/) {
    return std::unique_ptr<Operator>(new MockOp());
}

}  // namespace object_store
}  // namespace agentenv
