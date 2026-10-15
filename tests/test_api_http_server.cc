// SPDX-License-Identifier: MIT
// Rust: src/api/mod.rs — request framing, path routing and the status codes
// `impl From<OrchestratorError> for models::Error` assigns.
//
// The framing and routing are tested as pure functions, because that is where
// the failures are silent: a half-read body reaches a handler as a shorter
// request rather than as an error, and a path pattern that matches one segment
// too many routes to the wrong handler with a truncated id. Both produce a
// well-formed response to the wrong question.
#include "microtest.h"

#include <map>
#include <string>

#include "agentenv/api/http_server.h"
#include "agentenv/api/impls.h"

using namespace agentenv;
using namespace agentenv::api;

namespace {

std::string Get(const std::map<std::string, std::string>& m, const char* k) {
    const std::map<std::string, std::string>::const_iterator it = m.find(k);
    return it == m.end() ? std::string("<missing>") : it->second;
}

std::string QueryOf(const HttpRequest& r, const char* k) {
    const std::unordered_map<std::string, std::string>::const_iterator it = r.query.find(k);
    return it == r.query.end() ? std::string("<missing>") : it->second;
}

}  // namespace

// ============================================================
// request framing
// ============================================================

MT_TEST(a_request_split_across_reads_is_not_parsed_early) {
    // TCP delivers a request in however many pieces it likes. A parser that
    // treats the first piece as the whole request hands the handler a body
    // with the tail missing -- and a create request whose JSON is cut short
    // either fails to parse or, worse, parses into something smaller.
    const std::string whole =
        "POST /sandboxes HTTP/1.1\r\nContent-Length: 17\r\n\r\n{\"template_id\":1}";

    // Every prefix short of the whole must report Incomplete, never Complete
    // and never Malformed.
    for (std::size_t cut = 1; cut < whole.size(); ++cut) {
        const FramedRequest partial = ParseRequest(whole.substr(0, cut));
        if (partial.framing != RequestFraming::Incomplete) {
            MT_EXPECT_EQ(static_cast<int>(partial.framing),
                         static_cast<int>(RequestFraming::Incomplete));
            return;
        }
    }

    const FramedRequest full = ParseRequest(whole);
    MT_EXPECT_TRUE(full.framing == RequestFraming::Complete);
    MT_EXPECT_EQ(full.request.body, std::string("{\"template_id\":1}"));
    MT_EXPECT_EQ(full.consumed, whole.size());
}

MT_TEST(a_body_longer_than_content_length_does_not_bleed_into_the_request) {
    // The surplus belongs to whatever comes next on the connection. Handing it
    // to the handler would append a second request's bytes to this one's JSON.
    const std::string buffer =
        "POST /sandboxes HTTP/1.1\r\nContent-Length: 2\r\n\r\n{}GET /health HTTP/1.1\r\n\r\n";
    const FramedRequest framed = ParseRequest(buffer);
    MT_EXPECT_TRUE(framed.framing == RequestFraming::Complete);
    MT_EXPECT_EQ(framed.request.body, std::string("{}"));
    // `consumed` marks where the next request starts.
    MT_EXPECT_EQ(framed.consumed, buffer.find("GET /health"));
}

MT_TEST(an_oversized_declared_body_is_refused_before_it_is_buffered) {
    // Rejected on the announced length, not after reading it: waiting to find
    // out means allocating whatever the client claimed it would send.
    const FramedRequest framed = ParseRequest(
        "POST /sandboxes HTTP/1.1\r\nContent-Length: 999999999\r\n\r\n");
    MT_EXPECT_TRUE(framed.framing == RequestFraming::Malformed);
    MT_EXPECT_TRUE(framed.error.find("size limit") != std::string::npos);
}

MT_TEST(a_malformed_content_length_is_an_error_not_a_zero) {
    // Treating an unparseable length as 0 would quietly deliver an empty body
    // to a handler that was sent a real one.
    const FramedRequest framed =
        ParseRequest("POST /sandboxes HTTP/1.1\r\nContent-Length: abc\r\n\r\n{}");
    MT_EXPECT_TRUE(framed.framing == RequestFraming::Malformed);
    MT_EXPECT_TRUE(framed.error.find("Content-Length") != std::string::npos);
}

MT_TEST(headers_are_matched_case_insensitively) {
    const FramedRequest framed = ParseRequest(
        "GET /health HTTP/1.1\r\nCONTENT-LENGTH: 0\r\nX-Token: abc\r\n\r\n");
    MT_EXPECT_TRUE(framed.framing == RequestFraming::Complete);
    // Stored lowercased, so a handler looking for `x-token` finds the header
    // whatever case the client sent it in.
    MT_EXPECT_EQ(framed.request.headers.count("x-token"), static_cast<std::size_t>(1));
}

MT_TEST(the_query_string_is_split_off_the_path) {
    const FramedRequest framed =
        ParseRequest("GET /sandboxes?limit=10&state=running HTTP/1.1\r\n\r\n");
    MT_EXPECT_TRUE(framed.framing == RequestFraming::Complete);
    // The path must not carry the query, or no route would ever match it.
    MT_EXPECT_EQ(framed.request.path, std::string("/sandboxes"));
    MT_EXPECT_EQ(QueryOf(framed.request, "limit"), std::string("10"));
    MT_EXPECT_EQ(QueryOf(framed.request, "state"), std::string("running"));
}

// ============================================================
// percent-decoding and query parsing
// ============================================================

MT_TEST(percent_escapes_and_plus_are_decoded) {
    MT_EXPECT_EQ(PercentDecode("a%20b"), std::string("a b"));
    MT_EXPECT_EQ(PercentDecode("a+b"), std::string("a b"));
    MT_EXPECT_EQ(PercentDecode("%2Fetc%2Fpasswd"), std::string("/etc/passwd"));
    // A truncated escape is kept verbatim rather than dropped: deleting it
    // would change the value into one the client never sent.
    MT_EXPECT_EQ(PercentDecode("100%"), std::string("100%"));
    MT_EXPECT_EQ(PercentDecode("%zz"), std::string("%zz"));
}

MT_TEST(a_valueless_query_key_is_present_with_an_empty_value) {
    const std::map<std::string, std::string> q = ParseQuery("verbose&limit=5");
    // Distinguishable from absent, which is what a flag means.
    MT_EXPECT_EQ(Get(q, "verbose"), std::string(""));
    MT_EXPECT_EQ(Get(q, "limit"), std::string("5"));
}

// ============================================================
// path matching
// ============================================================

MT_TEST(a_placeholder_matches_exactly_one_segment) {
    std::map<std::string, std::string> params;
    MT_EXPECT_TRUE(MatchPath("/sandboxes/{id}", "/sandboxes/abc", &params));
    MT_EXPECT_EQ(Get(params, "id"), std::string("abc"));

    // The one a prefix comparison gets wrong: this must route to the exec
    // handler, not to GET /sandboxes/{id} with a truncated id.
    MT_EXPECT_TRUE(!MatchPath("/sandboxes/{id}", "/sandboxes/abc/exec", NULL));
    // An empty segment is not an id.
    MT_EXPECT_TRUE(!MatchPath("/sandboxes/{id}", "/sandboxes/", NULL));
    MT_EXPECT_TRUE(!MatchPath("/sandboxes/{id}", "/sandboxes", NULL));
}

MT_TEST(a_trailing_slash_does_not_change_the_route) {
    MT_EXPECT_TRUE(MatchPath("/sandboxes", "/sandboxes/", NULL));
    std::map<std::string, std::string> params;
    MT_EXPECT_TRUE(MatchPath("/sandboxes/{id}/exec", "/sandboxes/xyz/exec/", &params));
    MT_EXPECT_EQ(Get(params, "id"), std::string("xyz"));
}

MT_TEST(a_percent_encoded_segment_is_decoded_into_the_parameter) {
    std::map<std::string, std::string> params;
    MT_EXPECT_TRUE(MatchPath("/sandboxes/{id}", "/sandboxes/a%2Db", &params));
    MT_EXPECT_EQ(Get(params, "id"), std::string("a-b"));
}

// ============================================================
// dispatch
// ============================================================

MT_TEST(a_known_path_with_the_wrong_method_is_405_not_404) {
    // A client told 404 concludes the sandbox is gone and stops retrying.
    // The resource exists here; the method is what is wrong.
    HttpServer server;
    server.Route("GET", "/sandboxes", [](const HttpRequest&, HttpResponse* r) {
        r->status = 200;
    });

    HttpRequest req;
    req.method = "PUT";
    req.path = "/sandboxes";
    MT_EXPECT_EQ(server.Dispatch(req).status, 405);

    req.path = "/nope";
    MT_EXPECT_EQ(server.Dispatch(req).status, 404);
}

MT_TEST(a_path_parameter_reaches_the_handler) {
    HttpServer server;
    std::string seen;
    server.Route("GET", "/sandboxes/{id}",
                 [&seen](const HttpRequest& req, HttpResponse* r) {
        const std::unordered_map<std::string, std::string>::const_iterator it =
            req.query.find("id");
        seen = (it == req.query.end()) ? std::string("<missing>") : it->second;
        r->status = 200;
    });

    HttpRequest req;
    req.method = "GET";
    req.path = "/sandboxes/sbx-42";
    MT_EXPECT_EQ(server.Dispatch(req).status, 200);
    MT_EXPECT_EQ(seen, std::string("sbx-42"));
}

MT_TEST(a_path_parameter_is_not_shadowed_by_a_query_of_the_same_name) {
    // The segment is the resource being addressed; a query string must not be
    // able to redirect the handler at a different sandbox.
    HttpServer server;
    std::string seen;
    server.Route("GET", "/sandboxes/{id}",
                 [&seen](const HttpRequest& req, HttpResponse* r) {
        const std::unordered_map<std::string, std::string>::const_iterator it =
            req.query.find("id");
        seen = (it == req.query.end()) ? std::string("<missing>") : it->second;
        r->status = 200;
    });

    HttpRequest req;
    req.method = "GET";
    req.path = "/sandboxes/real";
    req.query["id"] = "spoofed";
    server.Dispatch(req);
    MT_EXPECT_EQ(seen, std::string("real"));
}

// ============================================================
// response framing
// ============================================================

MT_TEST(every_response_carries_an_explicit_content_length) {
    HttpResponse resp;
    resp.status = 204;
    const std::string wire = SerializeResponse(resp);
    // Including for an empty body: without it the client reads until close,
    // so each reply would end the connection instead of framing itself.
    MT_EXPECT_TRUE(wire.find("Content-Length: 0\r\n") != std::string::npos);

    resp.status = 200;
    resp.body = "{\"a\":1}";
    MT_EXPECT_TRUE(SerializeResponse(resp).find("Content-Length: 7\r\n") !=
                   std::string::npos);
}

MT_TEST(a_handler_cannot_contradict_the_framing_headers) {
    HttpResponse resp;
    resp.status = 200;
    resp.body = "{}";
    resp.headers["Content-Length"] = "9999";
    const std::string wire = SerializeResponse(resp);
    MT_EXPECT_TRUE(wire.find("Content-Length: 2\r\n") != std::string::npos);
    MT_EXPECT_TRUE(wire.find("9999") == std::string::npos);
}

// ============================================================
// bind address
// ============================================================

MT_TEST(a_bind_address_without_a_host_means_all_interfaces) {
    std::string host;
    uint16_t port = 0;
    MT_EXPECT_TRUE(ParseBindAddr(":8080", &host, &port).ok());
    MT_EXPECT_EQ(host, std::string("0.0.0.0"));
    MT_EXPECT_EQ(static_cast<int>(port), 8080);

    MT_EXPECT_TRUE(ParseBindAddr("127.0.0.1:1", &host, &port).ok());
    MT_EXPECT_EQ(host, std::string("127.0.0.1"));
    MT_EXPECT_EQ(static_cast<int>(port), 1);

    // Refused rather than defaulted: a server that silently picks a port
    // nobody asked for is not reachable by whatever was going to call it.
    MT_EXPECT_TRUE(!ParseBindAddr("127.0.0.1", &host, &port).ok());
    MT_EXPECT_TRUE(!ParseBindAddr("127.0.0.1:99999", &host, &port).ok());
    MT_EXPECT_TRUE(!ParseBindAddr("127.0.0.1:http", &host, &port).ok());
}

// ============================================================
// OrchestratorError -> HTTP status
// ============================================================

MT_TEST(each_orchestrator_error_keeps_its_own_status) {
    // Rust `impl From<OrchestratorError> for models::Error`. The status is the
    // part a client acts on: it retries a 503, backs off on a 409 and gives up
    // on a 400. Collapsing them would make every failure look alike.
    const core::SandboxId id = core::SandboxId::Fresh();

    MT_EXPECT_EQ(ToApiError(orchestrator::OrchestratorError::ShuttingDown()).code, 503);
    MT_EXPECT_EQ(ToApiError(orchestrator::OrchestratorError::SandboxNotFound(id)).code, 404);
    MT_EXPECT_EQ(ToApiError(orchestrator::OrchestratorError::InvalidSandboxState(
                                id, orchestrator::SandboxState::Running)).code, 400);
    MT_EXPECT_EQ(ToApiError(orchestrator::OrchestratorError::SandboxOperationConflict(
                                id, orchestrator::SandboxOperation::Pause)).code, 409);
    MT_EXPECT_EQ(ToApiError(orchestrator::OrchestratorError::SandboxOperationFailed(
                                id, orchestrator::SandboxOperation::Build, "boom")).code, 500);
    MT_EXPECT_EQ(ToApiError(orchestrator::OrchestratorError::StoreOperationFailed("x")).code,
                 500);

    // The id is in the 404 body, so an operator reading a log knows which
    // sandbox the client asked for.
    const ApiError not_found =
        ToApiError(orchestrator::OrchestratorError::SandboxNotFound(id));
    MT_EXPECT_TRUE(not_found.message.find(id.ToString()) != std::string::npos);
}

MT_MAIN
