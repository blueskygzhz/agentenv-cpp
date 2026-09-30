// SPDX-License-Identifier: MIT
// Rust: src/api/impls/image_build.rs — HEALTHCHECK translation and the
// build-session state machine.
#include "agentenv/api/image_build.h"

#include <string>
#include <vector>

#include "microtest.h"

using namespace agentenv;       // NOLINT
using namespace agentenv::api;  // NOLINT

namespace {

using agentenv::snapshot::TemplateBuildErrorReason;

core::Json StringArray(const std::vector<std::string>& items) {
    core::JsonArray array;
    for (std::size_t i = 0; i < items.size(); ++i) array.push_back(core::Json(items[i]));
    return core::Json(array);
}

/// Builds `{"Healthcheck": {"Test": [...]}}`, optionally with a `Shell`.
core::Json ConfigWithHealthcheck(const std::vector<std::string>& test,
                                 const std::vector<std::string>* shell = NULL) {
    core::JsonObject healthcheck;
    healthcheck["Test"] = StringArray(test);

    core::JsonObject config;
    config["Healthcheck"] = core::Json(healthcheck);
    if (shell != NULL) config["Shell"] = StringArray(*shell);
    return core::Json(config);
}

std::vector<std::string> V(const std::string& a) {
    std::vector<std::string> out;
    out.push_back(a);
    return out;
}

std::vector<std::string> V(const std::string& a, const std::string& b) {
    std::vector<std::string> out;
    out.push_back(a);
    out.push_back(b);
    return out;
}

std::vector<std::string> V(const std::string& a, const std::string& b, const std::string& c) {
    std::vector<std::string> out;
    out.push_back(a);
    out.push_back(b);
    out.push_back(c);
    return out;
}

core::Optional<std::string> Ready(const core::Json& config) {
    const core::Expected<core::Optional<std::string>, std::string> command =
        DockerfileReadyCommand(core::Optional<core::Json>(config));
    MT_EXPECT_TRUE(command.ok());
    return command.value();
}

}  // namespace

// ---- HEALTHCHECK -----------------------------------------------------------

MT_TEST(image_build_ready_command_is_absent_without_a_healthcheck) {
    MT_EXPECT_TRUE(!DockerfileReadyCommand(core::Optional<core::Json>()).value().has_value());
    // An image config with no Healthcheck key at all.
    MT_EXPECT_TRUE(!Ready(core::Json(core::JsonObject())).has_value());
}

MT_TEST(image_build_ready_command_honours_the_none_marker) {
    // Docker's explicit "healthcheck disabled" marker must not become a ready
    // command.
    MT_EXPECT_TRUE(!Ready(ConfigWithHealthcheck(V("NONE"))).has_value());
    // An empty Test array is equally "nothing declared".
    MT_EXPECT_TRUE(!Ready(ConfigWithHealthcheck(std::vector<std::string>())).has_value());
}

MT_TEST(image_build_ready_command_renders_cmd_shell_through_the_default_shell) {
    const core::Optional<std::string> command =
        Ready(ConfigWithHealthcheck(V("CMD-SHELL", "curl -f http://localhost || exit 1")));
    MT_EXPECT_TRUE(command.has_value());
    // The command text is one argument to the shell, so it must survive as a
    // single quoted word.
    MT_EXPECT_EQ(*command,
                 std::string("/bin/sh -c 'curl -f http://localhost || exit 1'"));
}

MT_TEST(image_build_ready_command_uses_the_images_own_shell) {
    const std::vector<std::string> shell = V("/bin/bash", "-lc");
    const core::Optional<std::string> command =
        Ready(ConfigWithHealthcheck(V("CMD-SHELL", "test -f /ready"), &shell));
    MT_EXPECT_TRUE(command.has_value());
    MT_EXPECT_EQ(*command, std::string("/bin/bash -lc 'test -f /ready'"));
}

MT_TEST(image_build_ready_command_rejects_an_empty_shell) {
    const std::vector<std::string> shell;
    const core::Expected<core::Optional<std::string>, std::string> command =
        DockerfileReadyCommand(
            core::Optional<core::Json>(ConfigWithHealthcheck(V("CMD-SHELL", "true"), &shell)));
    MT_EXPECT_TRUE(!command.ok());
    MT_EXPECT_TRUE(command.error().find("SHELL must not be empty") != std::string::npos);
}

MT_TEST(image_build_ready_command_renders_exec_form_verbatim) {
    const core::Optional<std::string> command =
        Ready(ConfigWithHealthcheck(V("CMD", "curl", "-f")));
    MT_EXPECT_TRUE(command.has_value());
    // Exec form: the arguments are the command, with no shell wrapper.
    MT_EXPECT_EQ(*command, std::string("curl -f"));
}

MT_TEST(image_build_ready_command_quotes_exec_arguments_individually) {
    const core::Optional<std::string> command =
        Ready(ConfigWithHealthcheck(V("CMD", "test", "a b")));
    MT_EXPECT_TRUE(command.has_value());
    // The pieces are re-joined into one shell string, so an argument
    // containing a space must stay one word.
    MT_EXPECT_EQ(*command, std::string("test 'a b'"));
}

MT_TEST(image_build_ready_command_rejects_unrecognised_forms) {
    // A bare CMD with no arguments, an unknown mode, and a lone non-NONE
    // token are all errors rather than silent skips: an unrecognised
    // healthcheck would otherwise mean "always ready" and hand out a sandbox
    // before it works.
    MT_EXPECT_TRUE(!DockerfileReadyCommand(
                        core::Optional<core::Json>(ConfigWithHealthcheck(V("CMD"))))
                        .ok());
    MT_EXPECT_TRUE(!DockerfileReadyCommand(
                        core::Optional<core::Json>(ConfigWithHealthcheck(V("BOGUS", "x"))))
                        .ok());
    MT_EXPECT_TRUE(!DockerfileReadyCommand(
                        core::Optional<core::Json>(ConfigWithHealthcheck(V("SOMETHING"))))
                        .ok());
}

MT_TEST(image_build_ready_command_rejects_a_malformed_test_array) {
    core::JsonObject healthcheck;
    // Not an array of strings.
    healthcheck["Test"] = core::Json(std::string("CMD true"));
    core::JsonObject config;
    config["Healthcheck"] = core::Json(healthcheck);

    MT_EXPECT_TRUE(!DockerfileReadyCommand(core::Optional<core::Json>(core::Json(config))).ok());
}

// ---- startup commands ------------------------------------------------------

MT_TEST(image_build_startup_prefers_the_request) {
    snapshot::CommandContext context;
    core::Optional<std::string> start;
    core::Optional<std::string> ready;

    MT_EXPECT_TRUE(BuildStartupCommands(
                       core::Optional<std::string>(std::string("/from-request")),
                       core::Optional<std::string>(std::string("request-ready")), context,
                       core::Optional<core::Json>(ConfigWithHealthcheck(V("CMD", "image"))),
                       &start, &ready)
                       .ok());
    // An explicit request value is the caller's decision and wins over the
    // image's own healthcheck.
    MT_EXPECT_EQ(*start, std::string("/from-request"));
    MT_EXPECT_EQ(*ready, std::string("request-ready"));
}

MT_TEST(image_build_startup_falls_back_to_the_healthcheck) {
    snapshot::CommandContext context;
    core::Optional<std::string> start;
    core::Optional<std::string> ready;

    MT_EXPECT_TRUE(BuildStartupCommands(
                       core::Optional<std::string>(std::string("/from-request")),
                       core::Optional<std::string>(), context,
                       core::Optional<core::Json>(ConfigWithHealthcheck(V("CMD", "probe"))),
                       &start, &ready)
                       .ok());
    // Only the half the request left unsaid comes from the image.
    MT_EXPECT_EQ(*ready, std::string("probe"));
}

MT_TEST(image_build_startup_propagates_a_healthcheck_failure) {
    snapshot::CommandContext context;
    core::Optional<std::string> start;
    core::Optional<std::string> ready;

    MT_EXPECT_TRUE(!BuildStartupCommands(
                        core::Optional<std::string>(), core::Optional<std::string>(), context,
                        core::Optional<core::Json>(ConfigWithHealthcheck(V("BOGUS", "x"))),
                        &start, &ready)
                        .ok());
}

// ---- journal ---------------------------------------------------------------

MT_TEST(image_build_journal_round_trips) {
    BuildJournal journal;
    journal.cache  = "cache-image";
    journal.parent = core::Optional<std::string>(std::string("parent-build"));

    const core::Expected<BuildJournal, std::string> parsed =
        BuildJournal::FromJson(journal.ToJson());
    MT_EXPECT_TRUE(parsed.ok());
    MT_EXPECT_EQ(parsed.value().cache, std::string("cache-image"));
    MT_EXPECT_EQ(*parsed.value().parent, std::string("parent-build"));
}

MT_TEST(image_build_journal_parent_is_optional) {
    BuildJournal journal;
    journal.cache = "cache-image";

    const core::Expected<BuildJournal, std::string> parsed =
        BuildJournal::FromJson(journal.ToJson());
    MT_EXPECT_TRUE(parsed.ok());
    MT_EXPECT_TRUE(!parsed.value().parent.has_value());
}

MT_TEST(image_build_journal_requires_a_cache) {
    MT_EXPECT_TRUE(!BuildJournal::FromJson(core::Json(core::JsonObject())).ok());
    MT_EXPECT_TRUE(!BuildJournal::FromJson(core::Json(std::string("nope"))).ok());
}

MT_TEST(image_build_journal_key_is_namespaced) {
    MT_EXPECT_EQ(BuildJournal::JournalKey("abc"), std::string("build/abc"));
    MT_EXPECT_EQ(BuildSeedName("abc"), std::string("seed-abc"));
}

// ---- session state machine -------------------------------------------------

MT_TEST(image_build_session_starts_in_starting) {
    const BuildSession session;
    MT_EXPECT_TRUE(session.state().kind == BuildSessionState::Kind::Starting);
    MT_EXPECT_TRUE(!session.IsPublishing());
    MT_EXPECT_TRUE(!session.IsTerminal());
}

MT_TEST(image_build_session_happy_path) {
    BuildSession session;
    MT_EXPECT_TRUE(session.Ready("127.0.0.1:1234"));
    MT_EXPECT_TRUE(session.state().kind == BuildSessionState::Kind::Ready);
    MT_EXPECT_EQ(session.state().address, std::string("127.0.0.1:1234"));

    MT_EXPECT_TRUE(session.Publishing());
    MT_EXPECT_TRUE(session.IsPublishing());

    MT_EXPECT_TRUE(session.Finish(core::Optional<TemplateBuildErrorReason>()));
    MT_EXPECT_TRUE(session.IsTerminal());
    // Absent failure means the build succeeded.
    MT_EXPECT_TRUE(!session.state().failure.has_value());
}

MT_TEST(image_build_session_ready_only_applies_once) {
    BuildSession session;
    MT_EXPECT_TRUE(session.Ready("a"));
    // A second ready is not a transition; the address must not change under a
    // client that already connected.
    MT_EXPECT_TRUE(!session.Ready("b"));
    MT_EXPECT_EQ(session.state().address, std::string("a"));
}

MT_TEST(image_build_session_cancelled_can_never_become_ready) {
    BuildSession session;
    MT_EXPECT_TRUE(session.Cancel());
    // This is the property that keeps a client from tunnelling into a sandbox
    // that is being torn down.
    MT_EXPECT_TRUE(!session.Ready("127.0.0.1:1234"));
    MT_EXPECT_TRUE(session.state().kind == BuildSessionState::Kind::Cancelled);
}

MT_TEST(image_build_session_cannot_publish_before_ready) {
    BuildSession session;
    // Publishing from Starting would commit a build whose sandbox never came
    // up.
    MT_EXPECT_TRUE(!session.Publishing());
    MT_EXPECT_TRUE(session.state().kind == BuildSessionState::Kind::Starting);
}

MT_TEST(image_build_session_terminal_states_are_final) {
    BuildSession finished;
    MT_EXPECT_TRUE(finished.Finish(core::Optional<TemplateBuildErrorReason>(
        TemplateBuildErrorReason::WithStep("boom", "2"))));
    // Cancelling a finished build would discard the recorded outcome.
    MT_EXPECT_TRUE(!finished.Cancel());
    MT_EXPECT_TRUE(!finished.Finish(core::Optional<TemplateBuildErrorReason>()));
    MT_EXPECT_EQ(finished.state().failure->message, std::string("boom"));
    MT_EXPECT_EQ(*finished.state().failure->step, std::string("2"));

    BuildSession cancelled;
    MT_EXPECT_TRUE(cancelled.Cancel());
    MT_EXPECT_TRUE(!cancelled.Cancel());
    MT_EXPECT_TRUE(!cancelled.Finish(core::Optional<TemplateBuildErrorReason>()));
}

MT_TEST(image_build_session_can_be_cancelled_mid_flight) {
    BuildSession from_starting;
    MT_EXPECT_TRUE(from_starting.Cancel());

    BuildSession from_ready;
    MT_EXPECT_TRUE(from_ready.Ready("a"));
    MT_EXPECT_TRUE(from_ready.Cancel());

    BuildSession from_publishing;
    MT_EXPECT_TRUE(from_publishing.Ready("a"));
    MT_EXPECT_TRUE(from_publishing.Publishing());
    MT_EXPECT_TRUE(from_publishing.Cancel());
}

int main() { return microtest::RunAll(); }
