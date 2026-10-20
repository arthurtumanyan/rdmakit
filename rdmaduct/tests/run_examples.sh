#!/bin/bash
# Smoke test of the examples: starts hello_server on a free port, runs hello_client against it over TCP and
# (when DUCT_TEST_ADDR is set) RDMA, and checks the replies.
#   run_examples.sh BUILD_DIR
set -euo pipefail
dir=${1:?build directory}
port=$((20000 + RANDOM % 20000))
fail=0

check() {
    transport=$1
    host=$2
    "$dir/hello_server" "$transport" "$port" >/dev/null &
    server=$!
    sleep 0.5
    out=$("$dir/hello_client" "$transport" "$host" "$port" abc "rdma transport")
    kill "$server" 2>/dev/null || true
    wait "$server" 2>/dev/null || true
    expect=$'abc -> cba\nrdma transport -> tropsnart amdr'
    if [ "$out" != "$expect" ]; then
        echo "FAIL: $transport examples returned: $out" >&2
        fail=1
    else
        echo "examples over $transport: ok"
    fi
}

check tcp 127.0.0.1
if [ -n "${DUCT_TEST_ADDR:-}" ] && [ -f "$dir/.rdma_enabled" ]; then
    port=$((port + 1))
    check rdma "$DUCT_TEST_ADDR"
fi
exit $fail
