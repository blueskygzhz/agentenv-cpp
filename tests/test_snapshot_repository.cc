// SPDX-License-Identifier: MIT
// Tests for snapshot::repository — in-memory backend + filter + error.
#include "microtest.h"

#include "agentenv/snapshot/repository/backends.h"

using namespace agentenv::snapshot::repository;

static SnapshotRecord tmpl(const std::string& id, const std::string& alias) {
    SnapshotRecord r;
    r.id = id;
    r.alias = alias;
    r.source = SnapshotSourceKind::Template;
    r.build_status = TemplateBuildStatus::Waiting;
    return r;
}

MT_TEST(repo_create_get_by_id_and_alias) {
    InMemorySnapshotRepository repo;
    auto c = repo.Create(tmpl("id-1", "python:3.11"));
    MT_EXPECT_TRUE(c.ok());

    auto by_id = repo.Get("id-1");
    MT_EXPECT_TRUE(by_id.ok() && by_id.value().has_value());
    auto by_alias = repo.Get("python:3.11");
    MT_EXPECT_TRUE(by_alias.ok() && by_alias.value().has_value());
    MT_EXPECT_TRUE(by_alias.value()->id == "id-1");
}

MT_TEST(repo_create_rejects_committed) {
    InMemorySnapshotRepository repo;
    SnapshotRecord r = tmpl("id-2", "");
    r.committed = true;
    MT_EXPECT_TRUE(!repo.Create(r).ok());
}

MT_TEST(repo_try_start_build_transitions) {
    InMemorySnapshotRepository repo;
    repo.Create(tmpl("id-3", ""));
    auto s1 = repo.TryStartBuild("id-3");
    MT_EXPECT_TRUE(s1.ok());
    MT_EXPECT_TRUE(s1.value().build_status == TemplateBuildStatus::Building);
    // Second start must fail (no longer waiting).
    auto s2 = repo.TryStartBuild("id-3");
    MT_EXPECT_TRUE(!s2.ok());
}

MT_TEST(repo_delete_idempotent) {
    InMemorySnapshotRepository repo;
    repo.Create(tmpl("id-4", "a"));
    MT_EXPECT_TRUE(repo.Delete("id-4").ok());
    MT_EXPECT_TRUE(repo.Delete("id-4").ok());  // idempotent
    auto g = repo.Get("id-4");
    MT_EXPECT_TRUE(g.ok() && !g.value().has_value());
}

MT_TEST(repo_list_templates_filter) {
    InMemorySnapshotRepository repo;
    repo.Create(tmpl("t1", ""));
    repo.Create(tmpl("t2", ""));
    auto l = repo.List(SnapshotListFilter::Templates());
    MT_EXPECT_TRUE(l.ok());
    MT_EXPECT_EQ(static_cast<int>(l.value().size()), 2);
}

MT_TEST(repo_error_to_string) {
    RepositoryError e = RepositoryError::AliasConflict("py", "old", "new");
    MT_EXPECT_TRUE(e.ToString().find("py") != std::string::npos);
    MT_EXPECT_TRUE(e.ToString().find("old") != std::string::npos);
}

MT_MAIN
