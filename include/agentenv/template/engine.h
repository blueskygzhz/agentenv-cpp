// SPDX-License-Identifier: MIT
// Rust: src/template/  — prebuilt / composed sandbox templates.
#ifndef AGENTENV_TEMPLATE_ENGINE_H_
#define AGENTENV_TEMPLATE_ENGINE_H_

#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/error.h"
#include "agentenv/core/expected.h"

namespace agentenv {
namespace tpl {

struct Template {
    std::string id;
    std::string base_image;                // OCI ref
    std::string kernel;
    std::string cmdline;
    std::vector<std::string> pre_build_cmds;
};

class Engine {
 public:
    virtual ~Engine() = default;
    virtual core::Expected<Template, core::AnyError> Get(const std::string& id) = 0;
    virtual core::Expected<std::vector<Template>, core::AnyError> List() = 0;
    virtual core::Expected<core::Unit, core::AnyError> Put(Template t) = 0;
};

std::unique_ptr<Engine> MakeMemoryEngine();

}  // namespace tpl
}  // namespace agentenv
#endif  // AGENTENV_TEMPLATE_ENGINE_H_
