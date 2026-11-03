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
# Unported subsystems must say so rather than answer with an empty success.
expect "POST snapshot (unported)"  501 -X POST \
    "$BASE/sandboxes/0192f3a1-4c5d-7e80-9abc-def012345678/snapshot"
expect "GET  /templates (unported)" 501 "$BASE/templates"
expect "POST exec (unported)"      501 -X POST -d '{"cmd":["true"]}' \
    "$BASE/sandboxes/0192f3a1-4c5d-7e80-9abc-def012345678/exec"

# A create reaches the backend factory and fails there, on this host, for want
# of a firecracker binary and KVM. What matters is that the error travels: a
# 500 naming the real cause, not a 201 for a sandbox that does not exist.
create_status="$(curl -s -o "$WORK/create" -w '%{http_code}' -X POST \
    -H "x-api-key: $AENV_API_KEY" \
    -H 'Content-Type: application/json' -d '{"template_id":"base"}' "$BASE/sandboxes")"
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
