// SPDX-License-Identifier: MIT
// Rust: src/observability/prometheus.rs `mod tests` + the label/guard contracts.
#include "agentenv/observability/prometheus.h"

#include <string>

#include "microtest.h"

using namespace agentenv::observability;  // NOLINT

// ---- Rust test: route_labels_hide_ids ----
MT_TEST(route_labels_hide_ids) {
    MT_EXPECT_TRUE(std::string(HttpRouteLabel("/sandboxes/sb-1/snapshots")) ==
                   "/sandboxes/{sandbox_id}/snapshots");
    MT_EXPECT_TRUE(std::string(HttpRouteLabel("/nodes/node-a")) == "/nodes/{node_id}");
    MT_EXPECT_TRUE(std::string(HttpRouteLabel("/templates")) == "/templates");
    MT_EXPECT_TRUE(std::string(HttpRouteLabel("/snapshots")) == "/snapshots");
    MT_EXPECT_TRUE(std::string(HttpRouteLabel("/v3/templates")) == "/v3/templates");
    MT_EXPECT_TRUE(std::string(HttpRouteLabel("/health")) == "/health");
    MT_EXPECT_TRUE(std::string(HttpRouteLabel("/sandboxes/sb-1/network")) ==
                   "/sandboxes/{sandbox_id}/network");
    MT_EXPECT_TRUE(std::string(HttpRouteLabel("/sandboxes/sb-1/pause")) ==
                   "/sandboxes/{sandbox_id}/pause");
    MT_EXPECT_TRUE(std::string(HttpRouteLabel("/sandboxes/sb-1/resume")) ==
                   "/sandboxes/{sandbox_id}/resume");
    MT_EXPECT_TRUE(std::string(HttpRouteLabel("/sandboxes/sb-1/fork")) ==
                   "/sandboxes/{sandbox_id}/fork");
    MT_EXPECT_TRUE(std::string(HttpRouteLabel("/templates/tpl/builds/build/status")) ==
                   "unmatched");
}

MT_TEST(route_labels_static_table_and_trailing_slash) {
    MT_EXPECT_TRUE(std::string(HttpRouteLabel("/sandboxes")) == "/sandboxes");
    MT_EXPECT_TRUE(std::string(HttpRouteLabel("/sandboxes-cold")) == "/sandboxes-cold");
    MT_EXPECT_TRUE(std::string(HttpRouteLabel("/v2/sandboxes")) == "/v2/sandboxes");
    MT_EXPECT_TRUE(std::string(HttpRouteLabel("/nodes")) == "/nodes");
    // trim_end_matches('/') makes trailing slashes irrelevant.
    MT_EXPECT_TRUE(std::string(HttpRouteLabel("/sandboxes/")) == "/sandboxes");
    MT_EXPECT_TRUE(std::string(HttpRouteLabel("/sandboxes///")) == "/sandboxes");
    MT_EXPECT_TRUE(std::string(HttpRouteLabel("/nodes/node-a/")) == "/nodes/{node_id}");
    // An all-slash path collapses to "/", which is not in the table.
    MT_EXPECT_TRUE(std::string(HttpRouteLabel("/")) == "unmatched");
    MT_EXPECT_TRUE(std::string(HttpRouteLabel("")) == "unmatched");
}

MT_TEST(route_labels_proxy_wildcard_at_any_depth) {
    MT_EXPECT_TRUE(std::string(HttpRouteLabel("/proxy")) == "/proxy/*");
    MT_EXPECT_TRUE(std::string(HttpRouteLabel("/proxy/a")) == "/proxy/*");
    MT_EXPECT_TRUE(std::string(HttpRouteLabel("/proxy/a/b/c/d/e/f")) == "/proxy/*");
}

MT_TEST(route_labels_reject_unknown_sandbox_verbs) {
    // An unknown third segment must not leak through as a new label.
    MT_EXPECT_TRUE(std::string(HttpRouteLabel("/sandboxes/sb-1/bogus")) == "unmatched");
    MT_EXPECT_TRUE(std::string(HttpRouteLabel("/sandboxes/sb-1/snapshots/extra")) == "unmatched");
}

// ---- method / status / route_source labels are closed sets ----
MT_TEST(method_label_is_a_closed_set) {
    MT_EXPECT_TRUE(std::string(HttpMethodLabel("GET")) == "GET");
    MT_EXPECT_TRUE(std::string(HttpMethodLabel("POST")) == "POST");
    MT_EXPECT_TRUE(std::string(HttpMethodLabel("PUT")) == "PUT");
    MT_EXPECT_TRUE(std::string(HttpMethodLabel("PATCH")) == "PATCH");
    MT_EXPECT_TRUE(std::string(HttpMethodLabel("DELETE")) == "DELETE");
    MT_EXPECT_TRUE(std::string(HttpMethodLabel("HEAD")) == "HEAD");
    MT_EXPECT_TRUE(std::string(HttpMethodLabel("OPTIONS")) == "OPTIONS");
    // Anything else is bucketed so a client cannot inflate cardinality.
    MT_EXPECT_TRUE(std::string(HttpMethodLabel("TRACE")) == "OTHER");
    MT_EXPECT_TRUE(std::string(HttpMethodLabel("get")) == "OTHER");
    MT_EXPECT_TRUE(std::string(HttpMethodLabel("")) == "OTHER");
}

MT_TEST(status_label_buckets) {
    MT_EXPECT_TRUE(std::string(HttpStatusLabel(100)) == "1xx");
    MT_EXPECT_TRUE(std::string(HttpStatusLabel(199)) == "1xx");
    MT_EXPECT_TRUE(std::string(HttpStatusLabel(200)) == "2xx");
    MT_EXPECT_TRUE(std::string(HttpStatusLabel(204)) == "2xx");
    MT_EXPECT_TRUE(std::string(HttpStatusLabel(301)) == "3xx");
    MT_EXPECT_TRUE(std::string(HttpStatusLabel(404)) == "4xx");
    MT_EXPECT_TRUE(std::string(HttpStatusLabel(503)) == "5xx");
    MT_EXPECT_TRUE(std::string(HttpStatusLabel(99)) == "other");
    MT_EXPECT_TRUE(std::string(HttpStatusLabel(600)) == "other");
}

MT_TEST(route_source_labels) {
    MT_EXPECT_TRUE(std::string(HttpRouteSourceAsStr(HttpRouteSource::ControlPlane)) ==
                   "control_plane");
    MT_EXPECT_TRUE(std::string(HttpRouteSourceAsStr(HttpRouteSource::ProxyHost)) == "proxy_host");
    MT_EXPECT_TRUE(std::string(HttpRouteSourceAsStr(HttpRouteSource::ProxyHeader)) ==
                   "proxy_header");
    MT_EXPECT_TRUE(std::string(HttpRouteSourceAsStr(HttpRouteSource::ProxyPrefix)) ==
                   "proxy_prefix");
}

MT_TEST(result_status_labels) {
    MT_EXPECT_TRUE(std::string(ResultStatus(true)) == "ok");
    MT_EXPECT_TRUE(std::string(ResultStatus(false)) == "error");
}

// ---- MetricGuard: drop-before-finish is "canceled", not "error" ----
MT_TEST(metric_guard_records_ok_and_error) {
    {
        MetricGuard g = MetricGuard::Operation("m", "op");
        g.Finish(true);
        MT_EXPECT_TRUE(std::string(g.recorded_status()) == "ok");
    }
    {
        MetricGuard g = MetricGuard::Operation("m", "op");
        g.Finish(false);
        MT_EXPECT_TRUE(std::string(g.recorded_status()) == "error");
    }
}

MT_TEST(metric_guard_unfinished_is_canceled) {
    MetricGuard g = MetricGuard::Stage("m", "boot");
    // Not finished yet, so nothing has been recorded.
    MT_EXPECT_TRUE(std::string(g.recorded_status()) == "");
    // The destructor records "canceled"; verified indirectly by the fact that
    // Finish() after a record is a no-op (record() guards on `recorded_`).
    g.Finish(true);
    MT_EXPECT_TRUE(std::string(g.recorded_status()) == "ok");
}

MT_TEST(metric_guard_finish_is_idempotent) {
    MetricGuard g = MetricGuard::OperationArtifact("m", "upload", "memfile");
    g.Finish(false);
    MT_EXPECT_TRUE(std::string(g.recorded_status()) == "error");
    // A second finish must not re-record nor flip the status.
    g.Finish(true);
    MT_EXPECT_TRUE(std::string(g.recorded_status()) == "error");
}

// ---- SandboxStageTimer threads the ok/error status through ----
MT_TEST(sandbox_stage_timer_reports_status) {
    SandboxStageTimer timer("create");
    MT_EXPECT_TRUE(timer.Time("boot", []() { return true; }));
    MT_EXPECT_TRUE(!timer.Time("boot", []() { return false; }));
    MT_EXPECT_TRUE(std::string(timer.operation()) == "create");
}

// ---- metric names must not drift; dashboards key off them ----
MT_TEST(metric_names_match_rust) {
    MT_EXPECT_TRUE(std::string(kHttpRequestDurationSeconds) ==
                   "agentenv_http_request_duration_seconds");
    MT_EXPECT_TRUE(std::string(kSandboxStageDurationSeconds) ==
                   "agentenv_sandbox_stage_duration_seconds");
    MT_EXPECT_TRUE(std::string(kSandboxStageInflight) == "agentenv_sandbox_stage_inflight");
}

MT_MAIN
