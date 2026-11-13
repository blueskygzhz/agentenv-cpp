#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# End-to-end smoke test: starts the real binary, drives it over a real socket
# with curl, and asserts the status codes.
#
# Everything else in tests/ runs against in-process fakes. This is the only
# check that the process actually binds, routes and answers -- the gap that let
# the binary build and pass every test while `ListenAndServe` returned
# "not implemented" and no route was registered at all.
set -u

BIN="${1:?usage: smoke_api.sh <path-to-agentenv-binary>}"
PORT="${2:-0}"

WORK="$(mktemp -d /tmp/aenv-smoke-XXXXXX)"
trap 'if [ -n "${SERVER_PID:-}" ]; then kill "$SERVER_PID" 2>/dev/null; fi; rm -rf "$WORK"' EXIT

# Port 0 would be picked by the kernel, but the client needs to know it, and
# the config file is written before the server starts. Pick a free one here.
if [ "$PORT" = "0" ]; then
    PORT="$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')"
fi
BASE="http://127.0.0.1:${PORT}"

cat > "$WORK/config.toml" <<EOF
data_dir = "$WORK/data"
server_bind_addr = "127.0.0.1:$PORT"
EOF

# The auth layer wraps every route, so the control-plane requests below need a
# credential. Supplying it through AENV_API_KEY exercises the real resolution
# path (`ApiKey::Resolve`) rather than disabling auth for the test -- which
# would leave the one check that the server actually authorizes requests
# untested. 32 URL-safe characters is the documented minimum.
export AENV_API_KEY="smoke0000api0000key0000000000abcd"

"$BIN" "$WORK/config.toml" > "$WORK/server.log" 2>&1 &
SERVER_PID=$!

# Poll for readiness rather than sleeping a fixed amount: a fixed wait is
# either flaky on a loaded machine or slow on an idle one.
ready=0
for _ in $(seq 1 100); do
    if curl -sf -o /dev/null "$BASE/health" 2>/dev/null; then ready=1; break; fi
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        # The runtime refuses to start without CAP_NET_ADMIN / CAP_SYS_ADMIN,
        # which an unprivileged container cannot grant. That is a missing
        # precondition for this test, not a defect in what it checks, so it is
        # reported as a skip -- failing would train everyone to ignore it, and
        # removing the capability gate to suit the test would drop a real
        # check from the production path.
        #
        # Matched on the binary's own message rather than probing capabilities
        # here, so there is one definition of "sufficiently privileged" and the
        # test cannot disagree with the code it is testing.
        if grep -q "missing required Linux capabilities" "$WORK/server.log"; then
            echo "SKIP: needs CAP_NET_ADMIN and CAP_SYS_ADMIN (run as root or"
            echo "      with those capabilities to exercise this test)"
            exit 77
        fi
        echo "FAIL: server exited during startup"
        cat "$WORK/server.log"
        exit 1
    fi
    sleep 0.1
done
if [ "$ready" -ne 1 ]; then
    echo "FAIL: server did not become ready"
    cat "$WORK/server.log"
    exit 1
fi

failures=0

# `expect <what> <wanted-status> <curl args...>`
#
# Every request carries the API key. `/health` does not need one, but sending
# it anyway keeps the helper uniform and still asserts the documented
# behaviour: the bypass is by path, not by the absence of a credential.
expect() {
    local what="$1"; shift
    local want="$1"; shift
    local got
    got="$(curl -s -o "$WORK/body" -w '%{http_code}' \
           -H "x-api-key: $AENV_API_KEY" "$@")"
    if [ "$got" = "$want" ]; then
        echo "  ok    $what ($got)"
    else
        echo "  FAIL  $what: expected $want, got $got"
        echo "        body: $(cat "$WORK/body")"
        failures=$((failures + 1))
    fi
}

echo "smoke: $BASE"

expect "GET  /health"              200 "$BASE/health"
expect "GET  /sandboxes"           200 "$BASE/sandboxes"

# The credential has to actually matter. Without this, every assertion above
# would pass just as well against a server that ignored the key entirely --
# which is the state the auth wiring exists to end. Checked with the one
# request known to answer 200 when authorized, so a 401 here can only be the
# missing key.
noauth="$(curl -s -o /dev/null -w '%{http_code}' "$BASE/sandboxes")"
if [ "$noauth" = "401" ]; then
    echo "  ok    GET  /sandboxes without a key is refused (401)"
else
    echo "  FAIL  GET  /sandboxes without a key: expected 401, got $noauth"
    failures=$((failures + 1))
fi
# `/health` stays reachable unauthenticated -- the bypass is by path, and the
# readiness poll above depends on it.
noauth_health="$(curl -s -o /dev/null -w '%{http_code}' "$BASE/health")"
if [ "$noauth_health" = "200" ]; then
    echo "  ok    GET  /health without a key is allowed (200)"
else
    echo "  FAIL  GET  /health without a key: expected 200, got $noauth_health"
    failures=$((failures + 1))
fi

expect "GET  unknown sandbox"      404 "$BASE/sandboxes/0192f3a1-4c5d-7e80-9abc-def012345678"
expect "GET  malformed id"         404 "$BASE/sandboxes/not-a-uuid"
expect "GET  unknown route"        404 "$BASE/nope"
expect "PUT  /sandboxes (method)"  405 -X PUT "$BASE/sandboxes"
expect "POST malformed JSON"       400 -X POST -d '{broken' "$BASE/sandboxes"
# These three used to assert 501 against "unported subsystems". All three had
# since been ported or removed, and this file kept asserting the old answers --
# it could only fail, and never did, because the skip above fires on every
# machine without CAP_NET_ADMIN. A test that cannot run cannot notice that what
# it checks has changed underneath it.
#
# The route *table* is asserted by `test_api_routes.cc`, which runs everywhere.
# What these add is that a real process over a real socket agrees with it.

# `snapshots`, plural. The singular spelling probed here before was never a
# route on either side, so the old 501 could not have come from the handler --
# it was a 404 from the router, read as a pass because 501 was never checked
# against anything.
#
# Body first: upstream's `requestBody` is `required: true`, and
# `CreateSnapshotReq`'s only field is optional, so an omitted body would
# otherwise publish an unaliased snapshot the caller meant to name.
expect "POST /sandboxes/{id}/snapshots (no body)" 400 -X POST \
    "$BASE/sandboxes/0192f3a1-4c5d-7e80-9abc-def012345678/snapshots"
# With a body, the handler runs and the unknown sandbox is what refuses it.
# This is the assertion that the route reaches `CreateSnapshot` at all.
expect "POST /sandboxes/{id}/snapshots (unknown)" 404 -X POST \
    -H 'Content-Type: application/json' -d '{}' \
    "$BASE/sandboxes/0192f3a1-4c5d-7e80-9abc-def012345678/snapshots"
# Templates are snapshot records filtered by source; an empty catalog lists as
# an empty array rather than failing, so a fresh node answers 200 with `[]`.
expect "GET  /templates"           200 "$BASE/templates"
# Not 501, and not a gap: upstream serves no `exec` endpoint -- `openapi.yml`
# never mentions it, because running a command is a data plane call that
# reaches envd through the proxy. This port invented a 501 route and it was
# removed, so the honest answer is the router's 404. Asserted to keep it from
# being reinvented.
expect "POST exec (not an endpoint)" 404 -X POST -d '{"cmd":["true"]}' \
    "$BASE/sandboxes/0192f3a1-4c5d-7e80-9abc-def012345678/exec"

# A create reaches the backend factory and fails there, on this host, for want
# of a firecracker binary and KVM. What matters is that the error travels: a
# 500 naming the real cause, not a 201 for a sandbox that does not exist.
create_status="$(curl -s -o "$WORK/create" -w '%{http_code}' -X POST \
    -H "x-api-key: $AENV_API_KEY" \
    -H 'Content-Type: application/json' -d '{"templateID":"base"}' "$BASE/sandboxes")"
if [ "$create_status" = "500" ] || [ "$create_status" = "201" ]; then
    echo "  ok    POST /sandboxes reaches the backend ($create_status)"
else
    echo "  FAIL  POST /sandboxes: expected 201 or 500, got $create_status"
    echo "        body: $(cat "$WORK/create")"
    failures=$((failures + 1))
fi
# Whatever the outcome, it must not be a silent success with no sandbox.
if [ "$create_status" = "500" ] && ! grep -q '"code":500' "$WORK/create"; then
    echo "  FAIL  POST /sandboxes 500 did not carry an error body"
    failures=$((failures + 1))
fi

# The server survives all of the above and still answers.
expect "GET  /health (after load)" 200 "$BASE/health"

if [ "$failures" -ne 0 ]; then
    echo "--- server log ---"
    cat "$WORK/server.log"
    echo "smoke: $failures failed"
    exit 1
fi
echo "smoke: all passed"
exit 0
