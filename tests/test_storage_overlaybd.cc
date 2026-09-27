// SPDX-License-Identifier: MIT
// Tests for storage layer: ublk caps constants + overlaybd config validation.
#include "microtest.h"

#include "agentenv/storage/ublk/caps.h"
#include "agentenv/storage/overlaybd/config.h"

using namespace agentenv::storage;

MT_TEST(ublk_caps_match_kernel_encoding) {
    // Rust asserts UBLK_U_CMD_UPDATE_SIZE == 0xC020_7515.
    MT_EXPECT_EQ(ublk::kUblkUCmdUpdateSize, 0xC0207515u);
    MT_EXPECT_EQ(static_cast<unsigned long long>(ublk::kUblkFUpdateSize),
                 static_cast<unsigned long long>(1ull << 10));
}

MT_TEST(overlaybd_layer_effective_repo_blob_url) {
    overlaybd::LayerConfig lc;
    std::string image_url = "https://reg.example/repo";
    MT_EXPECT_TRUE(lc.EffectiveRepoBlobUrl(image_url) == image_url);
    lc.repo_blob_url = "https://mirror.example/repo";
    MT_EXPECT_TRUE(lc.EffectiveRepoBlobUrl(image_url) == "https://mirror.example/repo");
}

MT_TEST(overlaybd_validate_upper_config) {
    overlaybd::UpperConfig u;
    // neither data nor index -> valid, returns false (no upper data).
    auto r0 = overlaybd::ValidateUpperConfig(u);
    MT_EXPECT_TRUE(r0.ok());
    MT_EXPECT_TRUE(r0.value() == false);

    // both set -> valid, returns true.
    u.data = "/data"; u.index = "/index";
    auto r1 = overlaybd::ValidateUpperConfig(u);
    MT_EXPECT_TRUE(r1.ok());
    MT_EXPECT_TRUE(r1.value() == true);

    // only one set -> error.
    overlaybd::UpperConfig bad;
    bad.data = "/data";
    auto r2 = overlaybd::ValidateUpperConfig(bad);
    MT_EXPECT_TRUE(!r2.ok());
}

MT_TEST(overlaybd_upper_mode_default) {
    overlaybd::UpperConfig u;
    MT_EXPECT_TRUE(u.WritableMode() == overlaybd::UpperMode::LogStructured);
    u.has_mode = true;
    u.mode = overlaybd::UpperMode::Sparse;
    MT_EXPECT_TRUE(u.WritableMode() == overlaybd::UpperMode::Sparse);
}

MT_MAIN
