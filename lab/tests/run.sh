#!/bin/bash
# Runs lab steps 7 to 12 (rdmaduct) as server and client on one machine and checks their output.
#   tests/run.sh BUILD_DIR
# Over TCP on 127.0.0.1 always; over RDMA as well when DUCT_TEST_ADDR holds the address of an RDMA interface and
# the steps were built with RDMA support. Ports are chosen from 20000 to 27999 among those no socket uses.
# Exits 0 when every check passes.
set -uo pipefail
dir=${1:?build directory}
fail=0
tmp=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; wait 2>/dev/null; rm -rf "$tmp"' EXIT

free_port() {
    local p
    for _ in $(seq 1 100); do
        p=$((20000 + RANDOM % 8000))
        if [ -z "$(ss -Htan "sport = :$p" 2>/dev/null)" ]; then
            echo "$p"
            return
        fi
    done
    echo "no free port found" >&2
    exit 1
}

# expect NAME FILE REGEX: the file must contain a line matching the extended regular expression.
expect() {
    if grep -Eq "$3" "$2"; then
        echo "ok   $1"
    else
        echo "FAIL $1: no line matches /$3/ in:" >&2
        sed 's/^/    /' "$2" >&2
        fail=1
    fi
}

# start_server STEP ARGS...: starts a server in the background on a fresh port ($port) and waits until it
# prints its listening line.
start_server() {
    local step=$1
    shift
    port=$(free_port)
    "$dir/$step" server --port "$port" "$@" > "$tmp/server.log" 2>&1 &
    server=$!
    for _ in $(seq 1 100); do
        grep -q "listening on port" "$tmp/server.log" 2>/dev/null && return
        sleep 0.05
    done
    echo "FAIL $step: server did not start" >&2
    cat "$tmp/server.log" >&2
    fail=1
}

stop_server() {
    kill "$server" 2>/dev/null
    wait "$server" 2>/dev/null
}

run() { # run TRANSPORT HOST
    local t=$1
    local host=$2
    local opt=(--transport "$t")

    start_server step7_duct_echo "${opt[@]}" --once
    timeout 60 "$dir/step7_duct_echo" client "$host" --port "$port" "${opt[@]}" hello lab > "$tmp/c.log" 2>&1
    wait "$server"
    expect "step 7 $t: echo" "$tmp/c.log" '^echo: lab$'
    expect "step 7 $t: oversize send refused" "$tmp/c.log" '^send of 4097 bytes: Message too long$'
    expect "step 7 $t: receive timeout" "$tmp/c.log" '^receive with a 200 ms timeout: Connection timed out$'
    expect "step 7 $t: usable after both errors" "$tmp/c.log" '^echo: still connected$'
    expect "step 7 $t: server saw the close" "$tmp/server.log" '^client gone: connection closed by peer$'

    start_server step8_duct_flow "${opt[@]}"
    timeout 120 "$dir/step8_duct_flow" client "$host" --port "$port" "${opt[@]}" --mib 2 > "$tmp/c.log" 2>&1
    echo "exit $?" >> "$tmp/c.log"
    stop_server
    expect "step 8 $t: back-pressure reached" "$tmp/c.log" 'send space 0, non-blocking send: Connection timed out'
    expect "step 8 $t: every message delivered" "$tmp/c.log" '^  server received ([0-9]+) of \1 messages$'
    expect "step 8 $t: throughput measured" "$tmp/c.log" '65536 B  batch 16 +[0-9]+ msg/s'
    expect "step 8 $t: no loss" "$tmp/c.log" '^exit 0$'

    start_server step9_duct_event_loop "${opt[@]}" --clients 8
    timeout 120 "$dir/step9_duct_event_loop" client "$host" --port "$port" "${opt[@]}" --clients 8 --requests 500 \
        > "$tmp/c.log" 2>&1
    wait "$server"
    expect "step 9 $t: every request answered" "$tmp/server.log" '^served 4000 requests on 8 connections in one thread$'
    expect "step 9 $t: client finished" "$tmp/c.log" '^8 connections x 500 round trips: [0-9]+ requests/s'

    start_server step10_duct_zero_copy "${opt[@]}"
    timeout 300 "$dir/step10_duct_zero_copy" client "$host" --port "$port" "${opt[@]}" --mib 8 > "$tmp/c.log" 2>&1
    echo "exit $?" >> "$tmp/c.log"
    stop_server
    expect "step 10 $t: zero-copy 1 MiB" "$tmp/c.log" '1048576 B  zero-copy +[0-9]+ MiB/s'
    expect "step 10 $t: no corrupt message" "$tmp/c.log" '^exit 0$'

    start_server step11_duct_one_sided "${opt[@]}" --clients 2
    timeout 120 "$dir/step11_duct_one_sided" client "$host" --port "$port" "${opt[@]}" --ops 500 > "$tmp/c.log" 2>&1
    wait "$server"
    expect "step 11 $t: write and read back" "$tmp/c.log" '^client 1 read back: "written by client 1 with a one-sided write"$'
    expect "step 11 $t: counters" "$tmp/c.log" '^fetch-and-add counter 1000, lock-protected counter 1000$'
    expect "step 11 $t: server view" "$tmp/server.log" '^counter 1000, lock-protected counter 1000'

    start_server step12_duct_kv "${opt[@]}"
    local kv=("$dir/step12_duct_kv" client "$host" --port "$port" "${opt[@]}")
    {
        timeout 30 "${kv[@]}" put city Yerevan
        timeout 30 "${kv[@]}" get city
        timeout 30 "${kv[@]}" rpcget city
        timeout 30 "${kv[@]}" del city
        timeout 30 "${kv[@]}" get city
        timeout 120 "${kv[@]}" bench 1000
    } > "$tmp/c.log" 2>&1
    stop_server
    expect "step 12 $t: one-sided GET" "$tmp/c.log" '^Yerevan$'
    expect "step 12 $t: GET after DEL" "$tmp/c.log" '^\(not found\)$'
    expect "step 12 $t: bench" "$tmp/c.log" '^1000 keys: one-sided GET [0-9.]+ us, message GET [0-9.]+ us \(mean\), [0-9]+ torn reads retried$'
}

run tcp 127.0.0.1
if [ -z "${DUCT_TEST_ADDR:-}" ]; then
    echo "skip rdma: DUCT_TEST_ADDR not set"
else
    # A build without RDMA support, or a host without a device, cannot listen on RDMA.
    "$dir/step7_duct_echo" server --transport rdma --port 0 --once > "$tmp/probe.log" 2>&1 &
    probe=$!
    sleep 0.5
    kill "$probe" 2>/dev/null
    wait "$probe" 2>/dev/null
    if grep -q "listening on port" "$tmp/probe.log"; then
        run rdma "$DUCT_TEST_ADDR"
    else
        echo "skip rdma: $(head -1 "$tmp/probe.log")"
    fi
fi
exit $fail
