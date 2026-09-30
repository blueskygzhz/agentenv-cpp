// SPDX-License-Identifier: MIT
// Rust: src/api/impls/image_build.rs
#include "agentenv/api/image_build.h"

#include "agentenv/shell-util/shell.h"

namespace agentenv {
namespace api {

// ---------------------------------------------------------------------------
// Journal
// ---------------------------------------------------------------------------

std::string BuildJournal::JournalKey(const std::string& build_id) {
    return "build/" + build_id;
}

core::Json BuildJournal::ToJson() const {
    core::JsonObject object;
    object["cache"] = core::Json(cache);
    if (parent.has_value()) object["parent"] = core::Json(*parent);
    return core::Json(object);
}

core::Expected<BuildJournal, std::string> BuildJournal::FromJson(const core::Json& json) {
    if (json.kind() != core::Json::Kind::Object) {
        return core::make_unexpected(std::string("build journal must be an object"));
    }
    const core::JsonObject& fields = json.as_object();

    core::JsonObject::const_iterator cache = fields.find("cache");
    if (cache == fields.end() || cache->second.kind() != core::Json::Kind::String) {
        return core::make_unexpected(std::string("build journal is missing 'cache'"));
    }

    BuildJournal journal;
    journal.cache = cache->second.as_string();

    core::JsonObject::const_iterator parent = fields.find("parent");
    // `Option<String>`: absent and null both mean "no parent".
    if (parent != fields.end() && parent->second.kind() == core::Json::Kind::String) {
        journal.parent = core::Optional<std::string>(parent->second.as_string());
    }
    return journal;
}

std::string BuildSeedName(const std::string& build_id) { return "seed-" + build_id; }

// ---------------------------------------------------------------------------
// Session state
// ---------------------------------------------------------------------------

BuildSessionState BuildSessionState::MakeStarting() {
    BuildSessionState state;
    state.kind = Kind::Starting;
    return state;
}

BuildSessionState BuildSessionState::MakeReady(const std::string& address) {
    BuildSessionState state;
    state.kind    = Kind::Ready;
    state.address = address;
    return state;
}

BuildSessionState BuildSessionState::MakePublishing() {
    BuildSessionState state;
    state.kind = Kind::Publishing;
    return state;
}

BuildSessionState BuildSessionState::MakeCancelled() {
    BuildSessionState state;
    state.kind = Kind::Cancelled;
    return state;
}

BuildSessionState BuildSessionState::MakeFinished(
    const core::Optional<snapshot::TemplateBuildErrorReason>& failure) {
    BuildSessionState state;
    state.kind    = Kind::Finished;
    state.failure = failure;
    return state;
}

bool BuildSession::Ready(const std::string& address) {
    // Only a starting build can become ready. A build that was already
    // cancelled must never be observed as ready, or a client would tunnel
    // into a sandbox that is being torn down.
    if (state_.kind != BuildSessionState::Kind::Starting) return false;
    state_ = BuildSessionState::MakeReady(address);
    return true;
}

bool BuildSession::Publishing() {
    // Publishing from `Starting` would mean committing a build whose sandbox
    // never came up.
    if (state_.kind != BuildSessionState::Kind::Ready) return false;
    state_ = BuildSessionState::MakePublishing();
    return true;
}

bool BuildSession::Cancel() {
    // Terminal states stay put: cancelling a finished build would discard the
    // recorded outcome.
    if (IsTerminal()) return false;
    state_ = BuildSessionState::MakeCancelled();
    return true;
}

bool BuildSession::Finish(const core::Optional<snapshot::TemplateBuildErrorReason>& failure) {
    if (IsTerminal()) return false;
    state_ = BuildSessionState::MakeFinished(failure);
    return true;
}

bool BuildSession::IsPublishing() const {
    return state_.kind == BuildSessionState::Kind::Publishing;
}

bool BuildSession::IsTerminal() const {
    return state_.kind == BuildSessionState::Kind::Cancelled ||
           state_.kind == BuildSessionState::Kind::Finished;
}

// ---------------------------------------------------------------------------
// HEALTHCHECK -> ready command
// ---------------------------------------------------------------------------

namespace {

/// Reads a JSON array of strings, the shape both `Healthcheck.Test` and
/// `Shell` use.
core::Expected<std::vector<std::string>, std::string> StringArray(const core::Json& json,
                                                                  const char* what) {
    if (json.kind() != core::Json::Kind::Array) {
        return core::make_unexpected(std::string("parse Dockerfile ") + what);
    }
    const core::JsonArray& items = json.as_array();
    std::vector<std::string> out;
    out.reserve(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (items[i].kind() != core::Json::Kind::String) {
            return core::make_unexpected(std::string("parse Dockerfile ") + what);
        }
        out.push_back(items[i].as_string());
    }
    return out;
}

/// Rust `config.pointer("/Healthcheck/Test")`.
const core::Json* Pointer(const core::Json& config, const char* first, const char* second) {
    if (config.kind() != core::Json::Kind::Object) return NULL;
    const core::JsonObject& top = config.as_object();
    core::JsonObject::const_iterator outer = top.find(first);
    if (outer == top.end() || outer->second.kind() != core::Json::Kind::Object) return NULL;
    const core::JsonObject& inner = outer->second.as_object();
    core::JsonObject::const_iterator found = inner.find(second);
    if (found == inner.end()) return NULL;
    return &found->second;
}

}  // namespace

core::Expected<core::Optional<std::string>, std::string> DockerfileReadyCommand(
    const core::Optional<core::Json>& image_config) {
    if (!image_config.has_value()) return core::Optional<std::string>();

    const core::Json* test_value = Pointer(*image_config, "Healthcheck", "Test");
    // No healthcheck declared: nothing to derive a ready command from.
    if (test_value == NULL) return core::Optional<std::string>();

    const core::Expected<std::vector<std::string>, std::string> test =
        StringArray(*test_value, "HEALTHCHECK");
    if (!test.ok()) return core::make_unexpected(test.error());

    const std::vector<std::string>& parts = test.value();
    if (parts.empty()) return core::Optional<std::string>();
    // Docker's explicit "no healthcheck" marker.
    if (parts.size() == 1 && parts[0] == "NONE") return core::Optional<std::string>();

    std::vector<std::string> command;
    if (parts.size() == 2 && parts[0] == "CMD-SHELL") {
        // Shell form: the command text is one argument to the image's SHELL.
        std::vector<std::string> shell;
        const core::Json* shell_value = NULL;
        if (image_config->kind() == core::Json::Kind::Object) {
            const core::JsonObject& fields = image_config->as_object();
            core::JsonObject::const_iterator found = fields.find("Shell");
            if (found != fields.end()) shell_value = &found->second;
        }
        if (shell_value != NULL) {
            const core::Expected<std::vector<std::string>, std::string> parsed =
                StringArray(*shell_value, "SHELL");
            if (!parsed.ok()) return core::make_unexpected(parsed.error());
            shell = parsed.value();
        } else {
            shell.push_back("/bin/sh");
            shell.push_back("-c");
        }
        if (shell.empty()) {
            return core::make_unexpected(std::string("Dockerfile SHELL must not be empty"));
        }
        command = shell;
        command.push_back(parts[1]);
    } else if (parts.size() >= 2 && parts[0] == "CMD") {
        // Exec form: the arguments are the command, used verbatim.
        command.assign(parts.begin() + 1, parts.end());
    } else {
        // An unrecognised healthcheck must not silently become "always
        // ready": that would hand out a sandbox before it works.
        return core::make_unexpected(std::string("invalid Dockerfile HEALTHCHECK command"));
    }

    std::string rendered;
    for (std::size_t i = 0; i < command.size(); ++i) {
        if (i != 0) rendered += " ";
        // Quoted per-argument: the pieces are re-joined into one shell string,
        // so an argument containing spaces must survive as one word.
        rendered += shellutil::ShellQuote(command[i]);
    }
    return core::Optional<std::string>(rendered);
}

core::Expected<core::Unit, std::string> BuildStartupCommands(
    const core::Optional<std::string>& request_start_cmd,
    const core::Optional<std::string>& request_ready_cmd,
    const snapshot::CommandContext& context, const core::Optional<core::Json>& image_config,
    core::Optional<std::string>* start_cmd, core::Optional<std::string>* ready_cmd) {
    // The request wins; the image only fills what the caller left unsaid.
    *start_cmd = request_start_cmd.has_value() ? request_start_cmd : context.EffectiveStartCmd();

    if (request_ready_cmd.has_value()) {
        *ready_cmd = request_ready_cmd;
        return core::Unit();
    }
    const core::Expected<core::Optional<std::string>, std::string> derived =
        DockerfileReadyCommand(image_config);
    if (!derived.ok()) return core::make_unexpected(derived.error());
    *ready_cmd = derived.value();
    return core::Unit();
}

}  // namespace api
}  // namespace agentenv
