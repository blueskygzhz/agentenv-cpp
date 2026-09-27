// SPDX-License-Identifier: MIT
// Tests for image::ParseReference.
#include "microtest.h"

#include "agentenv/image/reference.h"

using namespace agentenv;

MT_TEST(parse_plain_repo) {
    auto r = image::ParseReference("alpine");
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(r.value().registry.empty());
    MT_EXPECT_TRUE(r.value().repository == "alpine");
}

MT_TEST(parse_repo_with_tag) {
    auto r = image::ParseReference("library/alpine:3.20");
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(r.value().repository == "library/alpine");
    MT_EXPECT_TRUE(r.value().tag == "3.20");
}

MT_TEST(parse_registry_repo_tag) {
    auto r = image::ParseReference("ghcr.io/kvcache-ai/agentenv:latest");
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(r.value().registry == "ghcr.io");
    MT_EXPECT_TRUE(r.value().repository == "kvcache-ai/agentenv");
    MT_EXPECT_TRUE(r.value().tag == "latest");
}

MT_TEST(parse_digest) {
    auto r = image::ParseReference(
        "alpine@sha256:0000000000000000000000000000000000000000000000000000000000000000");
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(r.value().repository == "alpine");
    MT_EXPECT_TRUE(!r.value().digest.empty());
}

MT_TEST(parse_bad_digest_rejected) {
    auto r = image::ParseReference("alpine@md5:abcd");
    MT_EXPECT_TRUE(!r.ok());
}

MT_TEST(parse_empty_rejected) {
    auto r = image::ParseReference("");
    MT_EXPECT_TRUE(!r.ok());
}

MT_MAIN
