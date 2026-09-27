// SPDX-License-Identifier: MIT
// Tests for services::gateway — host_route parsing + schedule hint.
#include "microtest.h"

#include "services/gateway/internal.h"

using namespace agentenv::services::gateway;

MT_TEST(host_route_parses_valid) {
    std::vector<std::string> domains = NormalizeProxyDomains({"sbx.example"});
    auto r = ParseHostRoute("8080-mybox.sbx.example", domains);
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(r.value().matched);
    MT_EXPECT_TRUE(r.value().route.sandbox_id == "mybox");
    MT_EXPECT_EQ(r.value().route.target_port, 8080);
}

MT_TEST(host_route_apex_not_matched) {
    std::vector<std::string> domains = NormalizeProxyDomains({"sbx.example"});
    auto r = ParseHostRoute("sbx.example", domains);
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(!r.value().matched);
}

MT_TEST(host_route_bad_port) {
    std::vector<std::string> domains = NormalizeProxyDomains({"sbx.example"});
    auto r = ParseHostRoute("99999-box.sbx.example", domains);
    MT_EXPECT_TRUE(!r.ok());  // port out of range
}

MT_TEST(host_route_strips_request_port) {
    std::vector<std::string> domains = NormalizeProxyDomains({"sbx.example"});
    auto r = ParseHostRoute("3000-box.sbx.example:443", domains);
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(r.value().matched);
    MT_EXPECT_EQ(r.value().route.target_port, 3000);
}

MT_TEST(normalize_domains_sorted_desc_length) {
    std::vector<std::string> out = NormalizeProxyDomains({"a.io", "longer.example.io", "a.io"});
    MT_EXPECT_EQ(static_cast<int>(out.size()), 2);   // dedup
    MT_EXPECT_TRUE(out[0].size() >= out[1].size());  // longest first
}

MT_TEST(schedule_hint_post_sandboxes) {
    auto r = BuildScheduleHint("POST", "/sandboxes", "{\"template_id\":\"py311\"}");
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(r.value().has_hint);
    MT_EXPECT_TRUE(r.value().hint.kind == "new_sandbox");
    MT_EXPECT_TRUE(r.value().hint.image_ref == "py311");
}

MT_TEST(schedule_hint_get_no_hint) {
    auto r = BuildScheduleHint("GET", "/sandboxes", "");
    MT_EXPECT_TRUE(r.ok());
    MT_EXPECT_TRUE(!r.value().has_hint);
}

MT_MAIN
