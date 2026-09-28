// SPDX-License-Identifier: MIT
// Rust: src/sandbox/network/iptables_util.rs
#include "agentenv/sandbox/network/iptables_util.h"

#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cctype>
#include <cerrno>
#include <fcntl.h>
#include <cstdio>
#include <cstring>
#include <sstream>

#include "agentenv/core/capability.h"
#include "agentenv/core/logging.h"
#include "agentenv/privileges.h"

namespace agentenv {
namespace sandbox {
namespace network {
namespace {

RestoreScriptRunner g_runner = NULL;

std::string Trim(const std::string& value) {
    std::size_t begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(begin, end - begin);
}

/// Rust: `error.as_bytes().windows(n).any(|w| w.eq_ignore_ascii_case(needle))`.
bool ContainsCaseInsensitive(const std::string& haystack, const std::string& needle) {
    if (needle.empty() || needle.size() > haystack.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool equal = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            const unsigned char a = static_cast<unsigned char>(haystack[i + j]);
            const unsigned char b = static_cast<unsigned char>(needle[j]);
            if (std::tolower(a) != std::tolower(b)) {
                equal = false;
                break;
            }
        }
        if (equal) return true;
    }
    return false;
}

/// Rust `apply_restore_script`, minus the capability scope, which the caller
/// wraps around it. Feeds the script to `iptables-restore --noflush` on stdin
/// and surfaces its stderr on failure.
core::Expected<core::Unit, std::string> SpawnIptablesRestore(const std::string& script) {
    int stdin_pipe[2];
    int stderr_pipe[2];
    if (::pipe(stdin_pipe) != 0) {
        return core::make_unexpected(std::string("Failed to open stdin for iptables-restore: ") +
                                     std::strerror(errno));
    }
    if (::pipe(stderr_pipe) != 0) {
        ::close(stdin_pipe[0]);
        ::close(stdin_pipe[1]);
        return core::make_unexpected(std::string("Failed to spawn iptables-restore: ") +
                                     std::strerror(errno));
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(stdin_pipe[0]);
        ::close(stdin_pipe[1]);
        ::close(stderr_pipe[0]);
        ::close(stderr_pipe[1]);
        return core::make_unexpected(std::string("Failed to spawn iptables-restore: ") +
                                     std::strerror(errno));
    }

    if (pid == 0) {
        ::dup2(stdin_pipe[0], STDIN_FILENO);
        ::dup2(stderr_pipe[1], STDERR_FILENO);
        const int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDOUT_FILENO);
            ::close(devnull);
        }
        ::close(stdin_pipe[0]);
        ::close(stdin_pipe[1]);
        ::close(stderr_pipe[0]);
        ::close(stderr_pipe[1]);
        ::execlp("iptables-restore", "iptables-restore", "--noflush",
                 static_cast<char*>(NULL));
        ::_exit(127);
    }

    ::close(stdin_pipe[0]);
    ::close(stderr_pipe[1]);

    // SIGPIPE would kill the parent if the child died early; the short write
    // is detected through the exit status instead.
    void (*previous_sigpipe)(int) = ::signal(SIGPIPE, SIG_IGN);
    std::size_t written = 0;
    bool write_failed = false;
    while (written < script.size()) {
        const ssize_t n =
            ::write(stdin_pipe[1], script.data() + written, script.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            write_failed = true;
            break;
        }
        written += static_cast<std::size_t>(n);
    }
    ::close(stdin_pipe[1]);
    ::signal(SIGPIPE, previous_sigpipe);

    std::string stderr_text;
    char buffer[4096];
    while (true) {
        const ssize_t n = ::read(stderr_pipe[0], buffer, sizeof(buffer));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        stderr_text.append(buffer, static_cast<std::size_t>(n));
    }
    ::close(stderr_pipe[0]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return core::make_unexpected(std::string("Failed to wait for iptables-restore: ") +
                                         std::strerror(errno));
        }
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && !write_failed) {
        return core::Unit();
    }
    if (write_failed && stderr_text.empty()) {
        return core::make_unexpected(std::string("Failed to write iptables-restore script"));
    }
    return core::make_unexpected(std::string("iptables-restore failed: ") +
                                 Trim(stderr_text));
}

core::Expected<core::Unit, std::string> ApplyRestoreScript(const std::string& script) {
    if (g_runner != NULL) return g_runner(script);

    // CAP_NET_ADMIN only, on a throwaway thread: the server itself must not
    // keep the privilege between rule applications.
    std::string failure;
    const core::Expected<core::Unit, std::string> scoped =
        privileges::RunWithScopedCapabilities(
            std::vector<int>(1, core::cap::kCapNetAdmin), [&]() -> std::string {
                const core::Expected<core::Unit, std::string> result =
                    SpawnIptablesRestore(script);
                return result.ok() ? std::string() : result.error();
            });
    if (!scoped.ok()) return core::make_unexpected(scoped.error());
    return core::Unit();
}

/// Rust `handle_restore_failure`.
core::Expected<core::Unit, std::string> HandleRestoreFailure(
    const std::string& error, const OpenFailurePolicy& policy) {
    if (policy.kind == OpenFailurePolicy::Kind::ReturnErr) {
        return core::make_unexpected(error);
    }
    AGENTENV_WARN(policy.message.c_str(), " error=", error);
    return core::Unit();
}

}  // namespace

void SetRestoreScriptRunnerForTesting(RestoreScriptRunner runner) { g_runner = runner; }

// ---- IptablesRestoreCommand ----------------------------------------------

IptablesRestoreCommand IptablesRestoreCommand::NewChain(const std::string& table,
                                                        const std::string& chain) {
    IptablesRestoreCommand command;
    command.kind = Kind::NewChain;
    command.table = table;
    command.chain = chain;
    return command;
}

IptablesRestoreCommand IptablesRestoreCommand::FlushChain(const std::string& table,
                                                          const std::string& chain) {
    IptablesRestoreCommand command;
    command.kind = Kind::FlushChain;
    command.table = table;
    command.chain = chain;
    return command;
}

IptablesRestoreCommand IptablesRestoreCommand::Insert(const std::string& table,
                                                      const std::string& chain,
                                                      int32_t position,
                                                      const std::string& rule) {
    IptablesRestoreCommand command;
    command.kind = Kind::Insert;
    command.table = table;
    command.chain = chain;
    command.position = position;
    command.rule = rule;
    return command;
}

IptablesRestoreCommand IptablesRestoreCommand::Append(const std::string& table,
                                                      const std::string& chain,
                                                      const std::string& rule) {
    IptablesRestoreCommand command;
    command.kind = Kind::Append;
    command.table = table;
    command.chain = chain;
    command.rule = rule;
    return command;
}

IptablesRestoreCommand IptablesRestoreCommand::Delete(const std::string& table,
                                                      const std::string& chain,
                                                      const std::string& rule) {
    IptablesRestoreCommand command;
    command.kind = Kind::Delete;
    command.table = table;
    command.chain = chain;
    command.rule = rule;
    return command;
}

bool IptablesRestoreCommand::operator==(const IptablesRestoreCommand& o) const {
    return kind == o.kind && table == o.table && chain == o.chain &&
           position == o.position && rule == o.rule;
}

// ---- OpenFailurePolicy ----------------------------------------------------

OpenFailurePolicy OpenFailurePolicy::ReturnErr() {
    OpenFailurePolicy policy;
    policy.kind = Kind::ReturnErr;
    return policy;
}

OpenFailurePolicy OpenFailurePolicy::WarnAndIgnore(const std::string& message) {
    OpenFailurePolicy policy;
    policy.kind = Kind::WarnAndIgnore;
    policy.message = message;
    return policy;
}

// ---- script building ------------------------------------------------------

std::string BuildRestoreScript(const std::vector<IptablesRestoreCommand>& commands) {
    std::ostringstream script;
    bool have_table = false;
    std::string current_table;

    for (std::size_t i = 0; i < commands.size(); ++i) {
        const IptablesRestoreCommand& command = commands[i];
        if (!have_table || current_table != command.table) {
            if (have_table) script << "COMMIT\n";
            script << "*" << command.table << "\n";
            current_table = command.table;
            have_table = true;
        }
        switch (command.kind) {
            case IptablesRestoreCommand::Kind::NewChain:
                script << "-N " << command.chain << "\n";
                break;
            case IptablesRestoreCommand::Kind::FlushChain:
                script << "-F " << command.chain << "\n";
                break;
            case IptablesRestoreCommand::Kind::Insert:
                script << "-I " << command.chain << " " << command.position << " "
                       << command.rule << "\n";
                break;
            case IptablesRestoreCommand::Kind::Append:
                script << "-A " << command.chain << " " << command.rule << "\n";
                break;
            case IptablesRestoreCommand::Kind::Delete:
                script << "-D " << command.chain << " " << command.rule << "\n";
                break;
        }
    }

    if (have_table) script << "COMMIT\n";
    return script.str();
}

bool IsMissingIptablesRuleError(const std::string& error) {
    const std::string trimmed = Trim(error);
    return ContainsCaseInsensitive(trimmed, "bad rule") ||
           ContainsCaseInsensitive(trimmed, "does a matching rule exist") ||
           ContainsCaseInsensitive(trimmed, "no chain/target/match by that name") ||
           ContainsCaseInsensitive(trimmed, "rule does not exist");
}

core::Expected<core::Unit, std::string> ApplyIptablesCommands(
    const std::vector<IptablesRestoreCommand>& commands,
    const OpenFailurePolicy& open_failure_policy) {
    if (commands.empty()) return core::Unit();

    bool all_deletes = true;
    for (std::size_t i = 0; i < commands.size(); ++i) {
        if (commands[i].kind != IptablesRestoreCommand::Kind::Delete) {
            all_deletes = false;
            break;
        }
    }

    if (all_deletes) {
        // One at a time: a rule that is already gone must not abort the rest
        // of the teardown, and recognising that case needs per-command
        // granularity.
        for (std::size_t i = 0; i < commands.size(); ++i) {
            const std::string script =
                BuildRestoreScript(std::vector<IptablesRestoreCommand>(1, commands[i]));
            const core::Expected<core::Unit, std::string> applied = ApplyRestoreScript(script);
            if (applied.ok()) continue;
            if (IsMissingIptablesRuleError(applied.error())) continue;

            if (open_failure_policy.kind == OpenFailurePolicy::Kind::ReturnErr) {
                return core::make_unexpected(applied.error());
            }
            AGENTENV_WARN(open_failure_policy.message.c_str(), " error=", applied.error());
        }
        return core::Unit();
    }

    // Everything else lands atomically, so a policy is never half-installed.
    const core::Expected<core::Unit, std::string> applied =
        ApplyRestoreScript(BuildRestoreScript(commands));
    if (applied.ok()) return core::Unit();
    return HandleRestoreFailure(applied.error(), open_failure_policy);
}

}  // namespace network
}  // namespace sandbox
}  // namespace agentenv
