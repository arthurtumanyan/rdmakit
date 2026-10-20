#!/bin/bash
# Compatibility of arenakvd with third-party clients, run in containers on the host network:
#   valkey-cli         basic commands, expiry, INFO, pipelined input (valkey/valkey image)
#   valkey-benchmark   SET/GET/INCR/MSET with pipelining (-P 16) and 50 connections
#   memtier_benchmark  RESP and memcached text protocol, 4 threads x 8 connections, pipeline 4
#   redis-py           when the Python package `redis` is installed on the host
#
#   tests/compat.sh BUILD_DIR
#
# Exits 0 when every check passes, 1 on a failure, and 77 (skipped) when Docker is not available.
set -u
build=${1:?usage: compat.sh BUILD_DIR}
here=$(cd "$(dirname "$0")" && pwd)
if ! command -v docker >/dev/null 2>&1 || ! docker info >/dev/null 2>&1; then
    echo "compat: skipped (no docker)"
    exit 77
fi
VALKEY=${VALKEY_IMAGE:-valkey/valkey:latest}
MEMTIER=${MEMTIER_IMAGE:-redislabs/memtier_benchmark:latest}
port_resp=${COMPAT_RESP_PORT:-16379}
port_mc=${COMPAT_MC_PORT:-16211}
log=$(mktemp)
"$build/arenakvd" --port off --text-addr 127.0.0.1 --resp-port "$port_resp" --memcached-port "$port_mc" \
    --capacity 256M --threads 4 --log-level warn >"$log" 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null; wait $pid 2>/dev/null; rm -f "$log"' EXIT
for _ in $(seq 50); do
    (exec 3<>/dev/tcp/127.0.0.1/"$port_resp") 2>/dev/null && break
    sleep 0.1
done
fail=0
check() { # name, expected, actual
    if [ "$2" = "$3" ]; then
        echo "compat: ok   $1"
    else
        echo "compat: FAIL $1: expected [$2], got [$3]"
        fail=1
    fi
}
cli() { docker run --rm --network host "$VALKEY" valkey-cli -p "$port_resp" "$@" 2>&1; }

check "valkey-cli SET" "OK" "$(cli SET greeting hello)"
check "valkey-cli GET" "hello" "$(cli GET greeting)"
check "valkey-cli SET EX / TTL" "OK 100" "$(cli SET t v EX 100) $(cli TTL t)"
check "valkey-cli INCRBY" "15" "$(cli SET n 10 >/dev/null; cli INCRBY n 5)"
check "valkey-cli MGET" "hello 15" "$(cli MGET greeting n | tr '\n' ' ' | sed 's/ $//')"
check "valkey-cli DEL / EXISTS" "2 0" "$(cli DEL greeting n) $(cli EXISTS greeting)"
check "valkey-cli INFO keyspace" "1" "$(cli INFO | grep -c '^db0:keys=')"
check "valkey-cli pipe" "3" "$(printf 'SET p1 a\r\nSET p2 b\r\nSET p3 c\r\n' |
    docker run --rm -i --network host "$VALKEY" valkey-cli -p "$port_resp" --pipe 2>&1 |
    sed -n 's/.*replies: \([0-9]*\).*/\1/p')"

out=$(docker run --rm --network host "$VALKEY" valkey-benchmark -p "$port_resp" -t set,get,incr,mset -n 20000 -c 50 \
    -P 16 -q 2>&1)
check "valkey-benchmark set,get,incr,mset" "4" \
    "$(echo "$out" | tr '\r' '\n' | grep -oE '^ *(SET|GET|INCR|MSET \(10 keys\)): [0-9.]+ requests per second' |
        sed 's/:.*//' | sort -u | wc -l | tr -d ' ')"
check "valkey-benchmark reads the server configuration" "0" "$(echo "$out" | grep -c 'Could not fetch server CONFIG')"

for proto in redis memcache_text; do
    p=$port_resp
    [ "$proto" = memcache_text ] && p=$port_mc
    out=$(docker run --rm --network host "$MEMTIER" -s 127.0.0.1 -p "$p" --protocol="$proto" -t 4 -c 8 \
        --pipeline=4 -n 2000 --ratio=1:4 --data-size=512 --hide-histogram 2>&1)
    rc=$?
    check "memtier $proto exit status" "0" "$rc"
    check "memtier $proto totals line" "1" "$(echo "$out" | grep -c '^Totals')"
done

if python3 -c 'import redis' 2>/dev/null; then
    out=$(python3 "$here/compat_redis_py.py" "$port_resp" 2>&1)
    check "redis-py" "ok" "$out"
else
    echo "compat: skip redis-py (package not installed)"
fi
exit $fail
