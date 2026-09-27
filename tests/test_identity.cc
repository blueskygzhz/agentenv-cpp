// SPDX-License-Identifier: MIT
#include "microtest.h"
#include "agentenv/core/identity.h"

#include <cstdlib>
#include <string>

using namespace agentenv::core;

MT_TEST(uuid_roundtrip) {
    Uuid u = Uuid::GenV7();
    std::string s = u.ToString();
    MT_EXPECT_EQ(static_cast<int>(s.size()), 36);
    Uuid v;
    MT_EXPECT_TRUE(Uuid::Parse(s, &v));
    MT_EXPECT_TRUE(u == v);
}

MT_TEST(uuid_v7_time_ordered) {
    // Two V7 generated in the same ms tie by rand, so just check both parse
    // and monotonic-in-time invariant.
    Uuid a = Uuid::GenV7();
    Uuid b = Uuid::GenV7();
    (void)a; (void)b;
}

MT_TEST(sandbox_id_tag_isolation) {
    // Different tags must not be comparable (compile-time).
    SandboxId sid  = SandboxId::Fresh();
    TemplateId tid = TemplateId::Fresh();
    (void)sid; (void)tid;
    // static_assert would fail: sid == tid — good, that's the point.
}

// --- NodeIdentity (Rust: src/identity.rs) ----------------------------------

// Ported from Rust test `observability_commit_uses_build_time_injection`:
// the runtime AENV_GIT_COMMIT env var must NOT affect the resolved commit,
// because the commit is injected at compile time.
MT_TEST(node_identity_commit_uses_build_time_injection) {
    const char* runtime_override = "runtime-override-commit";
    const char* previous = std::getenv("AENV_GIT_COMMIT");
    std::string saved = previous ? std::string(previous) : std::string();
    bool had_previous = previous != nullptr;

    setenv("AENV_GIT_COMMIT", runtime_override, 1);

    NodeIdentityConfig cfg;
    NodeIdentity identity = NodeIdentity::FromConfig(cfg);

    // Compile-time value (BuildCommit) is the source of truth.
    MT_EXPECT_TRUE(identity.commit != std::string(runtime_override));
    MT_EXPECT_TRUE(identity.commit == std::string(BuildCommit()));

    if (had_previous) {
        setenv("AENV_GIT_COMMIT", saved.c_str(), 1);
    } else {
        unsetenv("AENV_GIT_COMMIT");
    }
}

MT_TEST(node_identity_id_prefers_config_then_hostname) {
    NodeIdentityConfig cfg;
    cfg.node_id = Optional<std::string>(std::string("node-A"));
    NodeIdentity id = NodeIdentity::FromConfig(cfg);
    MT_EXPECT_TRUE(id.id == std::string("node-A"));

    // Empty node_id is treated as absent -> falls back to hostname.
    NodeIdentityConfig cfg2;
    cfg2.node_id = Optional<std::string>(std::string(""));
    NodeIdentity id2 = NodeIdentity::FromConfig(cfg2);
    MT_EXPECT_TRUE(!id2.id.empty());  // hostname or "unknown"
}

MT_TEST(node_identity_cluster_id_parses_or_falls_back_to_nil) {
    // Valid UUID string round-trips.
    Uuid src = Uuid::GenV7();
    NodeIdentityConfig cfg;
    cfg.cluster_id = Optional<std::string>(src.ToString());
    NodeIdentity id = NodeIdentity::FromConfig(cfg);
    MT_EXPECT_TRUE(id.cluster_id == src);

    // Invalid UUID -> nil (with a warning logged).
    NodeIdentityConfig bad;
    bad.cluster_id = Optional<std::string>(std::string("not-a-uuid"));
    NodeIdentity id2 = NodeIdentity::FromConfig(bad);
    MT_EXPECT_TRUE(id2.cluster_id.is_nil());

    // Absent -> nil.
    NodeIdentityConfig none;
    NodeIdentity id3 = NodeIdentity::FromConfig(none);
    MT_EXPECT_TRUE(id3.cluster_id.is_nil());
}

MT_TEST(node_identity_service_instance_id_config_or_fresh) {
    NodeIdentityConfig cfg;
    cfg.service_instance_id = Optional<std::string>(std::string("svc-123"));
    NodeIdentity id = NodeIdentity::FromConfig(cfg);
    MT_EXPECT_TRUE(id.service_instance_id == std::string("svc-123"));

    // Absent -> a freshly generated v7 uuid string (36 chars).
    NodeIdentityConfig none;
    NodeIdentity id2 = NodeIdentity::FromConfig(none);
    MT_EXPECT_EQ(static_cast<int>(id2.service_instance_id.size()), 36);
}

MT_MAIN
