// SPDX-License-Identifier: MIT
// Tests for snapshot::repository — in-memory backend + filter + error.
//
// This file had stopped compiling against the current record types while
// `ctest` kept reporting it green off a stale binary, so the assertions below
// are rebuilt against the real API: `SnapshotRecord` is keyed by a `SnapshotId`
// (a UUID, not a label), its alias is a validated `SnapshotAlias`, and the
// build status lives inside `source.build` rather than on the record.
#include "microtest.h"

#include "agentenv/snapshot/repository/backends.h"

using namespace agentenv::snapshot::repository;
namespace core = agentenv::core;
namespace snapshot = agentenv::snapshot;

namespace {

/// `SnapshotAlias::Parse` accepts ASCII alphanumerics, `-` and `_` only, so an
/// alias like `python:3.11` is rejected -- which is why these use `python-311`.
core::Optional<snapshot::SnapshotAlias> Alias(const std::string& text) {
    if (text.empty()) return core::Optional<snapshot::SnapshotAlias>();
    const core::Expected<snapshot::SnapshotAlias, std::string> parsed =
        snapshot::SnapshotAlias::Parse(text);
    if (!parsed.ok()) return core::Optional<snapshot::SnapshotAlias>();
    return core::Optional<snapshot::SnapshotAlias>(parsed.value());
}

/// A waiting template record, built through the factory the production code
/// uses rather than by assigning fields: `TemplateWaiting` is what sets the
/// `source` to a template with a `Waiting` build, and open-coding that here
/// would let the two drift apart.
snapshot::SnapshotRecord Waiting(const core::SnapshotId& id, const std::string& alias) {
    return snapshot::SnapshotRecord::TemplateWaiting(
        id, Alias(alias), agentenv::sandbox::SandboxResources());
}

}  // namespace

MT_TEST(repo_create_get_by_id_and_alias) {
    InMemorySnapshotRepository repo;
    const core::SnapshotId id = core::SnapshotId::Fresh();
    MT_EXPECT_TRUE(repo.Create(Waiting(id, "python-311")).ok());

    // `Get` takes an id *or* an alias, as Rust does, and both must reach the
    // same record -- an alias that resolved to nothing would make a template
    // unreachable by the only name a caller knows it by.
    RepositoryResult<core::Optional<snapshot::SnapshotRecord> > by_id = repo.Get(id.ToString());
    MT_EXPECT_TRUE(by_id.ok() && by_id.value().has_value());
    RepositoryResult<core::Optional<snapshot::SnapshotRecord> > by_alias = repo.Get("python-311");
    MT_EXPECT_TRUE(by_alias.ok() && by_alias.value().has_value());
    if (by_alias.ok() && by_alias.value().has_value()) {
        MT_EXPECT_TRUE(by_alias.value()->id == id);
    }
}

MT_TEST(repo_create_rejects_a_record_that_is_already_committed) {
    InMemorySnapshotRepository repo;
    snapshot::SnapshotRecord r = Waiting(core::SnapshotId::Fresh(), "");
    // `Create` is the start of a build, so a record arriving with a capture
    // already attached did not come from this path. Accepting it would publish
    // a snapshot no build on this node ever produced.
    r.committed = core::Optional<snapshot::CommittedSnapshot>(snapshot::CommittedSnapshot());
    MT_EXPECT_TRUE(!repo.Create(r).ok());
}

MT_TEST(repo_try_start_build_transitions_once) {
    InMemorySnapshotRepository repo;
    const core::SnapshotId id = core::SnapshotId::Fresh();
    MT_EXPECT_TRUE(repo.Create(Waiting(id, "")).ok());

    RepositoryResult<snapshot::SnapshotRecord> s1 = repo.TryStartBuild(id);
    MT_EXPECT_TRUE(s1.ok());
    if (s1.ok()) {
        MT_EXPECT_TRUE(s1.value().source.build.status ==
                       snapshot::TemplateBuildStatus::Building);
    }
    // The second start fails: the transition is the claim on the build, so two
    // workers that both succeeded here would build the same template twice and
    // race to write its artifacts.
    MT_EXPECT_TRUE(!repo.TryStartBuild(id).ok());
}

MT_TEST(repo_delete_is_idempotent) {
    InMemorySnapshotRepository repo;
    const core::SnapshotId id = core::SnapshotId::Fresh();
    MT_EXPECT_TRUE(repo.Create(Waiting(id, "to-delete")).ok());

    MT_EXPECT_TRUE(repo.Delete(id.ToString()).ok());
    // Idempotent, so a retried delete after a lost response does not report a
    // failure for work that is already done.
    MT_EXPECT_TRUE(repo.Delete(id.ToString()).ok());

    RepositoryResult<core::Optional<snapshot::SnapshotRecord> > g = repo.Get(id.ToString());
    MT_EXPECT_TRUE(g.ok() && !g.value().has_value());
    // The alias went with it; leaving it behind would resolve to a record that
    // no longer exists.
    RepositoryResult<core::Optional<core::SnapshotId> > a = repo.ResolveAlias("to-delete");
    MT_EXPECT_TRUE(a.ok() && !a.value().has_value());
}

MT_TEST(repo_list_templates_filter) {
    InMemorySnapshotRepository repo;
    MT_EXPECT_TRUE(repo.Create(Waiting(core::SnapshotId::Fresh(), "")).ok());
    MT_EXPECT_TRUE(repo.Create(Waiting(core::SnapshotId::Fresh(), "")).ok());

    RepositoryResult<std::vector<snapshot::SnapshotRecord> > l =
        repo.List(SnapshotListFilter::Templates());
    MT_EXPECT_TRUE(l.ok());
    if (l.ok()) MT_EXPECT_EQ(static_cast<int>(l.value().size()), 2);
}

MT_TEST(repo_error_names_both_sides_of_an_alias_conflict) {
    RepositoryError e = RepositoryError::AliasConflict("py", "old", "new");
    // Both ids are in the message: knowing an alias is taken is not actionable
    // without knowing what holds it and what tried to take it.
    MT_EXPECT_TRUE(e.ToString().find("py") != std::string::npos);
    MT_EXPECT_TRUE(e.ToString().find("old") != std::string::npos);
    MT_EXPECT_TRUE(e.ToString().find("new") != std::string::npos);
    MT_EXPECT_TRUE(e.kind == RepositoryErrorKind::AliasConflict);
}

MT_MAIN
