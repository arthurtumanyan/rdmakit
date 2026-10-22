#!/bin/bash
# Smoke test of the examples: starts hello_server on a port that no socket uses, runs hello_client against it over TCP and
# (when DUCT_TEST_ADDR is set) RDMA, and checks the replies.
#   run_examples.sh BUILD_DIR
set -euo pipefail
dir=${1:?build directory}
fail=0

# Prints a port from 20000 to 27999 that no socket uses (TCP or UDP), so that the server can bind it.
free_port() {
    local p
    for _ in $(seq 1 100); do
        p=$((20000 + RANDOM % 8000))
        if [ -z "$(ss -Htan "sport = :$p" 2>/dev/null)" ] && [ -z "$(ss -Huan "sport = :$p" 2>/dev/null)" ]; then
            echo "$p"
            return
        fi
    done
    echo "no free port found" >&2
    exit 1
}

check() {
    transport=$1
    host=$2
    port=$(free_port)
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
    check rdma "$DUCT_TEST_ADDR"
fi
exit $fail
