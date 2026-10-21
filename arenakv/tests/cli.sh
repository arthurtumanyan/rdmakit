#!/bin/bash
# Command-line behavior of arenakvd and akv_cli:
#   arenakvd   --help and --version, rejection of invalid option values (exit status 2), start-up with port 0
#   akv_cli    every command over TCP against that server, exit statuses, binary values through standard input
#
#   tests/cli.sh BUILD_DIR
#
# Exits 0 when every check passes and 1 otherwise. Uses only loopback ports chosen by the kernel.
set -u
build=${1:?usage: cli.sh BUILD_DIR}
d=$build/arenakvd
c=$build/akv_cli
tmp=$(mktemp -d)
pid=
trap '[ -n "$pid" ] && kill "$pid" 2>/dev/null; wait 2>/dev/null; rm -rf "$tmp"' EXIT
fail=0
check() { # name, expected, actual
    if [ "$2" = "$3" ]; then
        echo "cli: ok   $1"
    else
        echo "cli: FAIL $1: expected [$2], got [$3]"
        fail=1
    fi
}

"$d" --help >"$tmp/help" 2>&1
check "arenakvd --help exit" 0 $?
check "arenakvd --help text" 1 "$(grep -c '^Usage: arenakvd' "$tmp/help")"
"$d" --version >"$tmp/version" 2>&1
check "arenakvd --version" 1 "$(grep -c '^arenakvd [0-9]' "$tmp/version")"
for bad in "--shards 3" "--capacity 10K" "--transport foo" "--threads abc" "--depth 1" "--max-msg 10" \
    "--port 70000" "--capacity 5X" "--log-level loud" "stray" "--port off" "--tls-resp-port 7000" \
    "--snapshot $tmp/missing/dir/x.snap"; do
    # shellcheck disable=SC2086
    timeout 5 "$d" $bad >/dev/null 2>&1
    check "arenakvd rejects '$bad'" 2 $?
done

"$d" --transport tcp --addr 127.0.0.1 --port 0 --capacity 64M --threads 2 >"$tmp/log" 2>&1 &
pid=$!
port=
for _ in $(seq 50); do
    port=$(sed -n 's/.*listening: native \([0-9]*\),.*/\1/p' "$tmp/log")
    [ -n "$port" ] && break
    sleep 0.1
done
check "server started" 1 "$([ -n "$port" ] && echo 1)"
a=(--port "$port")

"$c" "${a[@]}" ping >"$tmp/out" 2>&1
check "ping" "PONG 0" "$(cat "$tmp/out") $?"
"$c" "${a[@]}" get absent >/dev/null 2>&1
check "get absent exit" 1 $?
"$c" "${a[@]}" --ttl 60 --flags 3 set k1 hello
check "set exit" 0 $?
check "get" "hello" "$("$c" "${a[@]}" get k1)"
check "meta flags" "flags 3" "$("$c" "${a[@]}" meta k1 | grep '^flags')"
check "meta ttl set" 1 "$("$c" "${a[@]}" meta k1 | awk '/^ttl_ms/ {print ($2 > 0 && $2 <= 60000)}')"
"$c" "${a[@]}" add k1 x >/dev/null 2>&1
check "add present exit" 1 $?
"$c" "${a[@]}" replace absent x >/dev/null 2>&1
check "replace absent exit" 1 $?
"$c" "${a[@]}" append k1 " world"
"$c" "${a[@]}" prepend k1 ">"
check "append, prepend" ">hello world" "$("$c" "${a[@]}" get k1)"
check "incr" "5 3" "$("$c" "${a[@]}" incr ctr 5) $("$c" "${a[@]}" incr ctr -2)"
"$c" "${a[@]}" touch k1 0
check "touch removes expiry" "ttl_ms -1" "$("$c" "${a[@]}" meta k1 | grep '^ttl_ms')"
head -c 40000 /dev/urandom >"$tmp/blob"
"$c" "${a[@]}" set blob - <"$tmp/blob"
"$c" "${a[@]}" get blob >"$tmp/copy"
check "binary value round trip" 0 "$(cmp -s "$tmp/blob" "$tmp/copy"; echo $?)"
head -c 100000 /dev/urandom >"$tmp/big"
"$c" "${a[@]}" set big - <"$tmp/big" >/dev/null 2>&1
check "value above the message limit exit" 3 $?
"$c" "${a[@]}" exists k1
check "exists" 0 $?
"$c" "${a[@]}" del k1
"$c" "${a[@]}" del k1 >/dev/null 2>&1
check "del twice exit" 1 $?
check "stats entries" "entries 2" "$("$c" "${a[@]}" stats | grep '^entries ')"
"$c" "${a[@]}" flush
check "flush" "entries 0" "$("$c" "${a[@]}" stats | grep '^entries ')"
"$c" "${a[@]}" get >/dev/null 2>&1
check "usage error exit" 2 $?
"$c" --port 1 ping >/dev/null 2>&1
check "connection refused exit" 3 $?

kill "$pid"
wait "$pid"
check "arenakvd exit after SIGTERM" 0 $?
pid=
exit $fail
