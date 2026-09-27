// SPDX-License-Identifier: MIT
// Rust: src/privileges.rs
#include "agentenv/privileges.h"

#include <cerrno>
#include <cstring>
#include <sstream>
#include <thread>

#include <unistd.h>

#include "agentenv/core/fs.h"
#include "agentenv/core/logging.h"

namespace agentenv {
namespace privileges {
namespace {

using core::Unit;
namespace cap = core::cap;

/// The capability contract the non-root runtime is started with.
std::vector<int> RequiredCapabilities() {
    std::vector<int> required;
    required.push_back(cap::kCapNetAdmin);
    required.push_back(cap::kCapSysAdmin);
    return required;
}

}  // namespace

core::Expected<Unit, std::string> RequireRuntimeCapabilities() {
    const core::Expected<cap::CapabilitySets, std::string> sets = cap::CapabilitySets::Current();
    if (!sets.ok()) {
        return core::make_unexpected(std::string("read process capability sets: ") +
                                     sets.error());
    }

    // Root already holds everything in the permitted set, so only the
    // effective bit is meaningful there. A non-root runtime must additionally
    // be able to *delegate*, hence the stricter check.
    const bool is_root = core::fs::EffectiveUidIsRoot();
    const std::vector<int> required = RequiredCapabilities();

    std::vector<std::string> missing;
    for (std::size_t i = 0; i < required.size(); ++i) {
        const core::Expected<bool, std::string> present =
            is_root ? sets.value().Effective(required[i])
                    : sets.value().IsDelegable(required[i]);
        // Rust uses `.unwrap_or(false)`: an unreadable bit counts as missing
        // rather than aborting the check.
        if (!present.ok() || !present.value()) {
            missing.push_back(cap::CapabilityName(required[i]));
        }
    }

    if (!missing.empty()) {
        std::ostringstream joined;
        for (std::size_t i = 0; i < missing.size(); ++i) {
            if (i > 0) joined << ", ";
            joined << missing[i];
        }
        std::ostringstream oss;
        oss << "AENV runtime is missing required Linux capabilities: " << joined.str()
            << ". Start it with CAP_NET_ADMIN and CAP_SYS_ADMIN in the inheritable, permitted, "
               "and effective sets (the installed systemd unit configures this automatically)";
        return core::make_unexpected(oss.str());
    }
    return Unit();
}

core::Expected<Unit, std::string> ClearAmbientCapabilities() {
    const core::Expected<Unit, std::string> cleared = cap::ClearAmbientCapabilities();
    if (!cleared.ok()) {
        return core::make_unexpected(std::string("clear ambient Linux capabilities: ") +
                                     cleared.error());
    }
    return Unit();
}

core::Expected<Unit, std::string> RunWithScopedCapabilities(
    const std::vector<int>& capabilities, const std::function<std::string()>& operation) {
    // Capabilities are per-thread on Linux, so the scope is a thread: it takes
    // the requested set, runs, and dies. The caller's own privileges are
    // untouched throughout.
    std::string failure;
    bool scoped = false;

    std::thread worker([&capabilities, &operation, &failure, &scoped]() {
        const core::Expected<Unit, std::string> configured =
            cap::ConfigureCurrentProcessCapabilities(capabilities);
        if (!configured.ok()) {
            failure = "scope command capabilities: " + configured.error();
            return;
        }
        scoped = true;
        failure = operation();
    });
    worker.join();

    if (!scoped && failure.empty()) {
        return core::make_unexpected(
            std::string("capability-scoped command thread produced no result"));
    }
    if (!failure.empty()) return core::make_unexpected(failure);
    return Unit();
}

core::Expected<pid_t, std::string> SpawnScoped(const ScopedSpawnRequest& request) {
    if (request.argv.empty()) {
        return core::make_unexpected(std::string("scoped spawn requires a program to run"));
    }

    std::string failure;
    pid_t child = -1;

    std::thread launcher([&request, &failure, &child]() {
        // Namespace entry happens first, while CAP_SYS_ADMIN is still held.
        if (request.before_capability_scope) {
            const std::string error = request.before_capability_scope();
            if (!error.empty()) {
                failure = error;
                return;
            }
        }

        const core::Expected<Unit, std::string> configured =
            cap::ConfigureCurrentProcessCapabilities(request.capabilities);
        if (!configured.ok()) {
            failure = configured.error();
            return;
        }

        // The child inherits this thread's capability and namespace state, so
        // no pre-exec hook is needed in the (multi-threaded) server.
        const pid_t pid = ::fork();
        if (pid < 0) {
            failure = std::string("fork: ") + std::strerror(errno);
            return;
        }
        if (pid == 0) {
            std::vector<char*> argv;
            argv.reserve(request.argv.size() + 1);
            for (std::size_t i = 0; i < request.argv.size(); ++i) {
                argv.push_back(const_cast<char*>(request.argv[i].c_str()));
            }
            argv.push_back(NULL);
            ::execvp(argv[0], &argv[0]);
            ::_exit(127);  // exec failed
        }
        child = pid;
    });
    launcher.join();

    if (!failure.empty()) return core::make_unexpected(failure);
    if (child < 0) {
        return core::make_unexpected(
            std::string("scoped command launcher exited without returning a child"));
    }
    return child;
}

}  // namespace privileges
}  // namespace agentenv
