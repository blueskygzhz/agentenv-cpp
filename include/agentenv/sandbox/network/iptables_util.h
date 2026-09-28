// SPDX-License-Identifier: MIT
// Rust: src/sandbox/network/iptables_util.rs
//
// Builds and applies `iptables-restore` scripts. Going through
// `iptables-restore` rather than repeated `iptables` invocations is what makes
// a rule set land atomically: a partially applied egress policy would leave a
// sandbox with allow rules installed and its denies missing.
#ifndef AGENTENV_SANDBOX_NETWORK_IPTABLES_UTIL_H_
#define AGENTENV_SANDBOX_NETWORK_IPTABLES_UTIL_H_

#include <string>
#include <vector>

#include "agentenv/core/expected.h"

namespace agentenv {
namespace sandbox {
namespace network {

/// Rust `enum IptablesRestoreCommand`.
struct IptablesRestoreCommand {
    enum class Kind {
        NewChain,
        FlushChain,
        Insert,
        Append,
        Delete,
    };

    Kind kind = Kind::Append;
    std::string table;
    std::string chain;
    /// Only meaningful for `Insert`; Rust encodes it in the variant.
    int32_t position = 0;
    /// Empty for `NewChain` / `FlushChain`.
    std::string rule;

    static IptablesRestoreCommand NewChain(const std::string& table, const std::string& chain);
    static IptablesRestoreCommand FlushChain(const std::string& table,
                                             const std::string& chain);
    static IptablesRestoreCommand Insert(const std::string& table, const std::string& chain,
                                         int32_t position, const std::string& rule);
    static IptablesRestoreCommand Append(const std::string& table, const std::string& chain,
                                         const std::string& rule);
    static IptablesRestoreCommand Delete(const std::string& table, const std::string& chain,
                                         const std::string& rule);

    bool operator==(const IptablesRestoreCommand& o) const;
    bool operator!=(const IptablesRestoreCommand& o) const { return !(*this == o); }
};

/// Rust `enum OpenFailurePolicy`.
///
/// Teardown paths use `WarnAndIgnore` so that one stale or externally removed
/// rule does not abort the loop and leak every rule after it.
struct OpenFailurePolicy {
    enum class Kind {
        ReturnErr,
        WarnAndIgnore,
    };

    Kind kind = Kind::ReturnErr;
    std::string message;

    static OpenFailurePolicy ReturnErr();
    static OpenFailurePolicy WarnAndIgnore(const std::string& message);
};

/// Rust `build_restore_script`.
///
/// Commands **must** already be grouped by table; the script emits one
/// `*table` / `COMMIT` block per run of equal tables, so interleaving would
/// silently produce a script that reopens a table and loses the earlier
/// block's rules.
std::string BuildRestoreScript(const std::vector<IptablesRestoreCommand>& commands);

/// Rust `is_missing_iptables_rule_error` — recognises the messages
/// `iptables-restore` emits for a delete of a rule that is already gone, which
/// teardown treats as success.
bool IsMissingIptablesRuleError(const std::string& error);

/// Rust `apply_iptables_commands`.
///
/// An all-`Delete` batch is applied one command at a time so that an
/// already-absent rule stays idempotent without parsing rule strings back into
/// argv; every other batch is applied atomically.
core::Expected<core::Unit, std::string> ApplyIptablesCommands(
    const std::vector<IptablesRestoreCommand>& commands,
    const OpenFailurePolicy& open_failure_policy);

/// Seam for tests: replaces the `iptables-restore` invocation. Passing NULL
/// restores the real one.
typedef core::Expected<core::Unit, std::string> (*RestoreScriptRunner)(
    const std::string& script);
void SetRestoreScriptRunnerForTesting(RestoreScriptRunner runner);

}  // namespace network
}  // namespace sandbox
}  // namespace agentenv
#endif  // AGENTENV_SANDBOX_NETWORK_IPTABLES_UTIL_H_
