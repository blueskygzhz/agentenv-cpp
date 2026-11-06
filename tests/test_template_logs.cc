// SPDX-License-Identifier: MIT
// Rust: src/template/logs.rs — `mod tests`.
//
// Upstream's first and third tests drive the tracing layer and the
// `BuildLogs`/`BuildLogSession` registry, neither of which is ported (see the
// header for why). What they actually assert about the log itself -- the
// bounds, that a late write cannot change a closed stream, and that the
// document survives a new reader and is removed by a delete -- is asserted
// here directly against the buffer and a real POSIX repository.
#include "microtest.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "agentenv/core/fs.h"
#include "agentenv/snapshot/record.h"
#include "agentenv/snapshot/repository/documents.h"
#include "agentenv/snapshot/repository/posixfs/artifacts.h"
#include "agentenv/snapshot/repository/posixfs/backend.h"
#include "agentenv/template/logs.h"

using namespace agentenv;
using namespace agentenv::tpl;
using agentenv::snapshot::repository::DocumentKey;
using agentenv::snapshot::repository::SnapshotRepository;

namespace {

core::Optional<std::string> NoStep() { return core::Optional<std::string>(); }

core::Optional<std::string> Step(const std::string& name) {
    return core::Optional<std::string>(name);
}

/// The real POSIX backend over `root`, built the way `main.cc` builds it.
///
/// The artifact store is not optional here even though no test stores a blob:
/// `Delete` routes through `DeleteSnapshotBlobs`, which reports
/// `unsupported("blob storage")` without one -- so a catalog-only repository
/// could never run the delete this file asserts against.
std::unique_ptr<SnapshotRepository> OpenRepository(const std::string& root) {
    return std::unique_ptr<SnapshotRepository>(
        new agentenv::snapshot::repository::posixfs::PosixFsSnapshotRepository(
            std::make_shared<agentenv::snapshot::repository::posixfs::PosixFsCatalogStore>(root),
            std::make_shared<agentenv::snapshot::repository::posixfs::PosixFsArtifactStore>(
                root)));
}

}  // namespace

// ---- BuildLogEntry encoding -------------------------------------------------

MT_TEST(entry_json_round_trip_keeps_an_absent_step_absent) {
    BuildLogEntry entry;
    entry.timestamp = core::SystemTime(1730000000123456789LL);
    entry.level = BuildLogLevel::Error;
    entry.message = "boom";

    const core::Json encoded = entry.ToJson();
    // `skip_serializing_if`: the key must not appear at all, not as null -- a
    // client parsing the document distinguishes the two.
    MT_EXPECT_TRUE(encoded.as_object().find("step") == encoded.as_object().end());

    const core::Expected<BuildLogEntry, std::string> decoded = BuildLogEntry::FromJson(encoded);
    MT_EXPECT_TRUE(decoded.ok());
    MT_EXPECT_TRUE(decoded.value() == entry);
    MT_EXPECT_TRUE(!decoded.value().step.has_value());

    entry.step = Step("install");
    const core::Json with_step = entry.ToJson();
    MT_EXPECT_TRUE(with_step.as_object().find("step") != with_step.as_object().end());
    MT_EXPECT_TRUE(BuildLogEntry::FromJson(with_step).value() == entry);
}

MT_TEST(entry_json_rejects_a_level_it_cannot_represent) {
    // The four levels and nothing else. A fifth spelling in a document would
    // otherwise decode to a default and silently change a line's severity.
    const char* const levels[] = {"debug", "info", "warn", "error"};
    const BuildLogLevel expected[] = {BuildLogLevel::Debug, BuildLogLevel::Info,
                                      BuildLogLevel::Warn, BuildLogLevel::Error};
    for (std::size_t i = 0; i < 4; ++i) {
        MT_EXPECT_TRUE(BuildLogLevelFromString(levels[i]).has_value());
        MT_EXPECT_TRUE(*BuildLogLevelFromString(levels[i]) == expected[i]);
        MT_EXPECT_TRUE(std::string(BuildLogLevelToString(expected[i])) == levels[i]);
    }
    MT_EXPECT_TRUE(!BuildLogLevelFromString("trace").has_value());
    MT_EXPECT_TRUE(!BuildLogLevelFromString("INFO").has_value());
    MT_EXPECT_TRUE(!BuildLogLevelFromString("").has_value());

    const char* const broken[] = {
        "{}",
        "[]",
        "{\"level\":\"info\",\"message\":\"m\"}",                       // no timestamp
        "{\"timestamp\":\"not-a-time\",\"level\":\"info\",\"message\":\"m\"}",
        "{\"timestamp\":\"2024-01-01T00:00:00Z\",\"message\":\"m\"}",   // no level
        "{\"timestamp\":\"2024-01-01T00:00:00Z\",\"level\":\"trace\",\"message\":\"m\"}",
        "{\"timestamp\":\"2024-01-01T00:00:00Z\",\"level\":\"info\"}",  // no message
    };
    for (std::size_t i = 0; i < sizeof(broken) / sizeof(broken[0]); ++i) {
        const core::Expected<core::Json, core::AnyError> json = core::Json::Parse(broken[i]);
        MT_EXPECT_TRUE(json.ok());
        MT_EXPECT_TRUE(!BuildLogEntry::FromJson(json.value()).ok());
    }

    // A `"step": null` from a writer that did not skip the field decodes as
    // absent rather than failing: the entry is otherwise complete.
    const core::Expected<core::Json, core::AnyError> null_step = core::Json::Parse(
        "{\"timestamp\":\"2024-01-01T00:00:00Z\",\"level\":\"info\",\"message\":\"m\","
        "\"step\":null}");
    const core::Expected<BuildLogEntry, std::string> decoded =
        BuildLogEntry::FromJson(null_step.value());
    MT_EXPECT_TRUE(decoded.ok());
    MT_EXPECT_TRUE(!decoded.value().step.has_value());
}

// ---- Output line splitting --------------------------------------------------

MT_TEST(output_splits_lines_and_maps_the_stream_to_a_level) {
    BuildLogBuffer buffer;

    // Empty input yields nothing: a child that writes nothing must not add a
    // blank entry.
    buffer.Output(NoStep(), false, "");
    MT_EXPECT_TRUE(buffer.entries().empty());

    buffer.Output(Step("build"), false, "one\ntwo\n");
    MT_EXPECT_EQ(buffer.entries().size(), static_cast<std::size_t>(2));
    MT_EXPECT_TRUE(buffer.entries()[0].message == "one");
    MT_EXPECT_TRUE(buffer.entries()[1].message == "two");
    MT_EXPECT_TRUE(buffer.entries()[0].level == BuildLogLevel::Info);
    MT_EXPECT_TRUE(buffer.entries()[0].step.has_value());
    MT_EXPECT_TRUE(*buffer.entries()[0].step == "build");

    // A trailing newline does not produce an extra empty entry; a line with
    // no newline at the end still produces one.
    buffer.Output(NoStep(), true, "warned");
    MT_EXPECT_EQ(buffer.entries().size(), static_cast<std::size_t>(3));
    MT_EXPECT_TRUE(buffer.entries()[2].message == "warned");
    MT_EXPECT_TRUE(buffer.entries()[2].level == BuildLogLevel::Warn);
    MT_EXPECT_TRUE(!buffer.entries()[2].step.has_value());

    // CRLF: the `\r` is stripped so a line does not end with a control
    // character a client then renders.
    BuildLogBuffer crlf;
    crlf.Output(NoStep(), false, "a\r\nb\r\n");
    MT_EXPECT_EQ(crlf.entries().size(), static_cast<std::size_t>(2));
    MT_EXPECT_TRUE(crlf.entries()[0].message == "a");
    MT_EXPECT_TRUE(crlf.entries()[1].message == "b");

    // Blank lines in the middle are kept -- they are output the build produced.
    BuildLogBuffer blanks;
    blanks.Output(NoStep(), false, "a\n\nb");
    MT_EXPECT_EQ(blanks.entries().size(), static_cast<std::size_t>(3));
    MT_EXPECT_TRUE(blanks.entries()[1].message.empty());
}

// ---- the three bounds -------------------------------------------------------

MT_TEST(message_cap_truncates_without_splitting_a_utf8_sequence) {
    BuildLogBuffer buffer;
    // Lands the cut inside a 3-byte sequence: the last `a` is at
    // kBuildLogMaxMessageBytes - 2, so byte [max] is a continuation byte.
    const std::string head(kBuildLogMaxMessageBytes - 1, 'a');
    buffer.Log(BuildLogLevel::Info, NoStep(), head + "\xE4\xBD\xA0");

    MT_EXPECT_EQ(buffer.entries().size(), static_cast<std::size_t>(1));
    const std::string& message = buffer.entries()[0].message;
    MT_EXPECT_TRUE(message == head + " [truncated]");
    // The partial sequence is gone entirely rather than cut in half, so the
    // message is still valid UTF-8 and the JSON encoder has nothing to mangle.
    MT_EXPECT_TRUE(message.find('\xE4') == std::string::npos);
    // Truncating a message is not truncating the log: the build keeps logging.
    MT_EXPECT_TRUE(!buffer.truncated());
    buffer.Log(BuildLogLevel::Info, NoStep(), "still open");
    MT_EXPECT_EQ(buffer.entries().size(), static_cast<std::size_t>(2));

    // Exactly at the cap is left alone -- the suffix is only for a real cut.
    BuildLogBuffer exact;
    const std::string full(kBuildLogMaxMessageBytes, 'b');
    exact.Log(BuildLogLevel::Info, NoStep(), full);
    MT_EXPECT_TRUE(exact.entries()[0].message == full);
}

MT_TEST(build_log_limits_emit_one_notice) {
    // Upstream `build_log_limits_emit_one_notice`.
    BuildLogBuffer buffer;
    for (std::size_t i = 0; i < kBuildLogMaxEntries + 2; ++i) {
        buffer.Log(BuildLogLevel::Info, NoStep(), "");
    }
    MT_EXPECT_EQ(buffer.entries().size(), kBuildLogMaxEntries + 1);
    MT_EXPECT_TRUE(buffer.entries().back().level == BuildLogLevel::Warn);
    MT_EXPECT_TRUE(buffer.entries().back().message == kBuildLogLimitNotice);
    MT_EXPECT_TRUE(buffer.truncated());
    // The notice carries no step: it is the log's own statement, not a line
    // attributable to whichever step happened to overflow it.
    MT_EXPECT_TRUE(!buffer.entries().back().step.has_value());
}

MT_TEST(byte_cap_emits_one_notice_before_the_entry_cap) {
    BuildLogBuffer buffer;
    const std::string chunk(kBuildLogMaxMessageBytes, 'x');
    const std::size_t fits = kBuildLogMaxBytes / kBuildLogMaxMessageBytes;
    for (std::size_t i = 0; i < fits; ++i) {
        buffer.Log(BuildLogLevel::Info, Step("s"), chunk);
    }
    MT_EXPECT_EQ(buffer.entries().size(), fits);
    MT_EXPECT_TRUE(!buffer.truncated());

    // One more byte over and the log closes, well short of the entry cap --
    // which is the point of having both bounds.
    buffer.Log(BuildLogLevel::Info, Step("s"), chunk);
    MT_EXPECT_EQ(buffer.entries().size(), fits + 1);
    MT_EXPECT_TRUE(buffer.entries().back().message == kBuildLogLimitNotice);
    MT_EXPECT_TRUE(buffer.truncated());
    MT_EXPECT_TRUE(fits + 1 < kBuildLogMaxEntries);

    buffer.Log(BuildLogLevel::Info, Step("s"), "dropped");
    MT_EXPECT_EQ(buffer.entries().size(), fits + 1);
}

MT_TEST(sealed_buffer_ignores_late_writes) {
    // Upstream's "late write must not change the closed stream": the flush has
    // already persisted the document, so an accepted entry would be one a
    // reader never sees.
    BuildLogBuffer buffer;
    buffer.Log(BuildLogLevel::Info, NoStep(), "before");
    MT_EXPECT_TRUE(!buffer.sealed());

    buffer.Seal();
    MT_EXPECT_TRUE(buffer.sealed());
    buffer.Log(BuildLogLevel::Info, NoStep(), "late write must not change the closed stream");
    buffer.Output(NoStep(), true, "late\nstderr\n");
    MT_EXPECT_EQ(buffer.entries().size(), static_cast<std::size_t>(1));
    MT_EXPECT_TRUE(buffer.entries()[0].message == "before");
}

MT_TEST(timestamps_never_move_backwards) {
    // The clock-rollback branch cannot be driven directly without a settable
    // clock, so what is pinned here is the invariant a reader depends on: the
    // stream is non-decreasing, and sorting by timestamp is order-preserving.
    BuildLogBuffer buffer;
    for (int i = 0; i < 500; ++i) {
        buffer.Log(BuildLogLevel::Info, NoStep(), "line");
    }
    MT_EXPECT_EQ(buffer.entries().size(), static_cast<std::size_t>(500));
    for (std::size_t i = 1; i < buffer.entries().size(); ++i) {
        MT_EXPECT_TRUE(buffer.entries()[i - 1].timestamp <= buffer.entries()[i].timestamp);
    }
}

// ---- persistence ------------------------------------------------------------

MT_TEST(build_logs_persist_in_one_file_and_survive_new_reader_and_delete) {
    // Upstream `build_logs_persist_in_one_file_and_survive_new_reader_and_delete`,
    // minus the session registry: the buffer is sealed by hand where upstream
    // calls `session.finish()`.
    const core::Expected<std::string, std::string> temp =
        core::fs::CreateTempDir("agentenv-build-logs-");
    MT_EXPECT_TRUE(temp.ok());
    const std::string root = temp.value() + "/repository";

    std::unique_ptr<SnapshotRepository> repository = OpenRepository(root);
    const core::SnapshotId id = core::SnapshotId::Fresh();
    MT_EXPECT_TRUE(repository
                       ->Create(snapshot::SnapshotRecord::TemplateWaiting(
                           id, core::Optional<snapshot::SnapshotAlias>(),
                           sandbox::SandboxResources()))
                       .ok());

    BuildLogBuffer buffer;
    for (int n = 0; n < 600; ++n) {
        char line[32];
        std::snprintf(line, sizeof(line), "line %d", n);
        buffer.Log(BuildLogLevel::Info, NoStep(), line);
    }
    const snapshot::repository::RepositoryResult<core::Unit> written =
        WriteBuildLogs(*repository, id, buffer.entries());
    // Reported rather than just asserted: a repository write can fail for a
    // dozen filesystem reasons and a bare `ok()` failure says none of them.
    if (!written.ok()) std::fprintf(stderr, "  write: %s\n", written.error().ToString().c_str());
    MT_EXPECT_TRUE(written.ok());
    buffer.Seal();
    buffer.Log(BuildLogLevel::Info, NoStep(), "late write must not change the closed stream");

    // A second repository over the same root, as upstream does: the log must
    // live in the shared key space, not in the writer's memory.
    std::unique_ptr<SnapshotRepository> reader = OpenRepository(root);
    core::Expected<std::vector<BuildLogEntry>, snapshot::repository::RepositoryError> persisted =
        ReadBuildLogs(*reader, id);
    MT_EXPECT_TRUE(persisted.ok());
    MT_EXPECT_EQ(persisted.value().size(), static_cast<std::size_t>(600));
    MT_EXPECT_TRUE(persisted.value() == buffer.entries());

    // Re-writing what was read must be a fixed point -- i.e. the encoding
    // loses nothing, including the nanosecond timestamps.
    MT_EXPECT_TRUE(WriteBuildLogs(*repository, id, persisted.value()).ok());
    MT_EXPECT_TRUE(ReadBuildLogs(*reader, id).value() == persisted.value());

    // One file, at the path upstream uses, so a repository root written by
    // either implementation is readable by the other.
    const std::string path = root + "/" + DocumentKey::BuildLog(id).PosixPath();
    MT_EXPECT_TRUE(path == root + "/snapshots/build-logs/" + id.ToString() + ".json");
    MT_EXPECT_TRUE(core::fs::IsFile(path));

    // Deleting the snapshot takes the log with it; reading it back is empty
    // rather than an error.
    MT_EXPECT_TRUE(repository->Delete(id.ToString()).ok());
    MT_EXPECT_TRUE(ReadBuildLogs(*reader, id).value().empty());

    // And a delete by id also clears a log orphaned after its record is gone.
    MT_EXPECT_TRUE(WriteBuildLogs(*repository, id, persisted.value()).ok());
    MT_EXPECT_TRUE(repository->Delete(id.ToString()).ok());
    MT_EXPECT_TRUE(ReadBuildLogs(*reader, id).value().empty());
}

MT_TEST(reading_an_absent_log_is_empty_and_a_corrupt_one_is_an_error) {
    const core::Expected<std::string, std::string> temp =
        core::fs::CreateTempDir("agentenv-build-logs-absent-");
    MT_EXPECT_TRUE(temp.ok());
    std::unique_ptr<SnapshotRepository> repository = OpenRepository(temp.value() + "/repository");
    const core::SnapshotId id = core::SnapshotId::Fresh();

    // No document at all: a build that has not logged yet.
    const core::Expected<std::vector<BuildLogEntry>, snapshot::repository::RepositoryError>
        empty = ReadBuildLogs(*repository, id);
    MT_EXPECT_TRUE(empty.ok());
    MT_EXPECT_TRUE(empty.value().empty());

    // An empty log round-trips as an empty array, not as absent.
    MT_EXPECT_TRUE(WriteBuildLogs(*repository, id, std::vector<BuildLogEntry>()).ok());
    MT_EXPECT_TRUE(ReadBuildLogs(*repository, id).value().empty());

    // Garbage is reported, not silently read as an empty log: an operator
    // debugging a failed build must not be told it produced no output.
    MT_EXPECT_TRUE(repository->PutDocument(DocumentKey::BuildLog(id), "{not json").ok());
    MT_EXPECT_TRUE(!ReadBuildLogs(*repository, id).ok());
    MT_EXPECT_TRUE(repository->PutDocument(DocumentKey::BuildLog(id), "{\"a\":1}").ok());
    MT_EXPECT_TRUE(!ReadBuildLogs(*repository, id).ok());
    MT_EXPECT_TRUE(repository->PutDocument(DocumentKey::BuildLog(id), "[{\"level\":\"x\"}]").ok());
    MT_EXPECT_TRUE(!ReadBuildLogs(*repository, id).ok());
}

MT_MAIN
