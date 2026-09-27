// SPDX-License-Identifier: MIT
// Rust: src/snapshot/types/value.rs + src/snapshot/types/snapshot.rs
#include "agentenv/snapshot/types.h"

#include <sstream>

namespace agentenv {
namespace snapshot {

core::Expected<SnapshotAlias, std::string> SnapshotAlias::Parse(const std::string& input) {
    if (input.empty()) {
        return core::make_unexpected(std::string("snapshot alias cannot be empty"));
    }
    for (std::size_t i = 0; i < input.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(input[i]);
        const bool alphanumeric = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                  (c >= '0' && c <= '9');
        if (!alphanumeric && c != '-' && c != '_') {
            std::ostringstream oss;
            oss << "snapshot alias only supports ASCII letters, digits, hyphens, and "
                   "underscores, got: "
                << input;
            return core::make_unexpected(oss.str());
        }
    }
    return SnapshotAlias(input);
}

std::ostream& operator<<(std::ostream& os, const SnapshotAlias& alias) {
    return os << alias.ToString();
}

bool CommandContext::operator==(const CommandContext& o) const {
    if (env_vars != o.env_vars || workdir != o.workdir || exposed_ports != o.exposed_ports ||
        volumes != o.volumes || labels != o.labels) {
        return false;
    }
    if (user.has_value() != o.user.has_value()) return false;
    if (user.has_value() && *user != *o.user) return false;
    // An absent entrypoint and an empty one are different to OCI, so the
    // presence flags are compared before the contents.
    if (entrypoint.has_value() != o.entrypoint.has_value()) return false;
    if (entrypoint.has_value() && *entrypoint != *o.entrypoint) return false;
    if (cmd.has_value() != o.cmd.has_value()) return false;
    if (cmd.has_value() && *cmd != *o.cmd) return false;
    return true;
}

TemplateBuildErrorReason TemplateBuildErrorReason::New(const std::string& message) {
    TemplateBuildErrorReason reason;
    reason.message = message;
    return reason;
}

TemplateBuildErrorReason TemplateBuildErrorReason::WithStep(const std::string& message,
                                                            const std::string& step) {
    TemplateBuildErrorReason reason;
    reason.message = message;
    reason.step = step;
    return reason;
}

bool TemplateBuildErrorReason::operator==(const TemplateBuildErrorReason& o) const {
    if (message != o.message) return false;
    if (step.has_value() != o.step.has_value()) return false;
    return !step.has_value() || *step == *o.step;
}

std::ostream& operator<<(std::ostream& os, const TemplateBuildErrorReason& reason) {
    return os << reason.ToString();
}

}  // namespace snapshot
}  // namespace agentenv
