// SPDX-License-Identifier: MIT
// Rust: crates/object-store-operator/  — S3/OSS/local upload/download wrapper.
#ifndef AGENTENV_OBJECT_STORE_H_
#define AGENTENV_OBJECT_STORE_H_

#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/error.h"
#include "agentenv/core/expected.h"

namespace agentenv {
namespace object_store {

class Operator {
 public:
    virtual ~Operator() = default;

    virtual core::Expected<core::Unit, core::AnyError>
        Put(const std::string& key, const std::string& local_path) = 0;
    virtual core::Expected<core::Unit, core::AnyError>
        Get(const std::string& key, const std::string& local_path) = 0;
    virtual core::Expected<std::vector<std::string>, core::AnyError>
        List(const std::string& prefix) = 0;
    virtual core::Expected<core::Unit, core::AnyError>
        Delete(const std::string& key) = 0;
};

/// Kinds: "local" | "s3" (needs libcurl) | "mock".
std::unique_ptr<Operator> MakeOperator(const std::string& kind,
                                        const std::string& config);

}  // namespace object_store
}  // namespace agentenv
#endif  // AGENTENV_OBJECT_STORE_H_
