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

"$BIN" "$WORK/config.toml" > "$WORK/server.log" 2>&1 &
SERVER_PID=$!

# Poll for readiness rather than sleeping a fixed amount: a fixed wait is
# either flaky on a loaded machine or slow on an idle one.
ready=0
for _ in $(seq 1 100); do
    if curl -sf -o /dev/null "$BASE/health" 2>/dev/null; then ready=1; break; fi
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
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
expect() {
    local what="$1"; shift
    local want="$1"; shift
    local got
    got="$(curl -s -o "$WORK/body" -w '%{http_code}' "$@")"
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
