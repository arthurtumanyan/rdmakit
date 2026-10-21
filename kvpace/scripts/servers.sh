#!/bin/bash
# Starts and stops the systems under test.
#
#   servers.sh start SERVER PORT [CPUSET]    start SERVER on PORT, pinned to CPUSET (default SERVER_CPUS)
#   servers.sh stop SERVER                   stop SERVER
#   servers.sh stop-all                      stop every system under test
#   servers.sh pid SERVER                    print the host PID of the server process
#
# SERVER is one of: valkey, dragonfly, memcached (containers on the host network, persistence off),
# kvstore, kvstore-rdma, kvstore-mc (native arenakvd under taskset with one worker per CPU of CPUSET; kvstore-rdma
# listens on RDMA_ADDR; kvstore-mc serves the memcached text protocol on PORT and the native protocol on PORT + 1).
# The ids "kvstore", "kvstore-rdma" and "kvstore-mc" name arenakv in the benchmark data (its name during
# development).
#
# Environment:
#   CAPACITY     cache size limit with eviction, for example 256mb or 4gb. Unset: 8 GiB limit.
#                valkey: --maxmemory + allkeys-lru; dragonfly: --maxmemory + --cache_mode=true;
#                memcached: -m (megabytes); kvstore: --capacity.
#   EXTRA_ARGS   additional server arguments appended to the command line
#   RDMA_ADDR    IPv4 address of the RDMA interface (kvstore-rdma only)
set -euo pipefail
here=$(dirname "$0")
. "$here/env.sh"

action=${1:?usage: servers.sh start|stop|stop-all|pid SERVER [PORT] [CPUSET]}
name=${2:-}

# Converts sizes such as 256mb, 4gb, 512kb to megabytes.
to_mb() {
    local v=${1,,}
    case $v in
    *gb) echo $((${v%gb} * 1024)) ;;
    *mb) echo "${v%mb}" ;;
    *kb) echo $((${v%kb} / 1024)) ;;
    *) echo "$v" ;;
    esac
}

stop_one() {
    docker rm -f "pace-$1" >/dev/null 2>&1 || true
    if [ -f "/tmp/pace-$1.pid" ]; then
        kill "$(cat "/tmp/pace-$1.pid")" 2>/dev/null || true
        rm -f "/tmp/pace-$1.pid"
    fi
}

case "$action" in
start)
    port=${3:?port required}
    cpus=${4:-$SERVER_CPUS}
    ncpu=$(echo "$cpus" | tr ',' '\n' | wc -l)
    cap=${CAPACITY:-8gb}
    cap_mb=$(to_mb "$cap")
    stop_one "$name"
    case "$name" in
    valkey)
        # shellcheck disable=SC2086
        docker run -d --rm --name pace-valkey --network host --cpuset-cpus "$cpus" valkey/valkey:latest \
            valkey-server --port "$port" --save "" --appendonly no --protected-mode no --io-threads "$ncpu" \
            --maxmemory "${cap_mb}mb" --maxmemory-policy allkeys-lru ${EXTRA_ARGS:-} >/dev/null
        ;;
    dragonfly)
        # shellcheck disable=SC2086
        docker run -d --rm --name pace-dragonfly --network host --cpuset-cpus "$cpus" --ulimit memlock=-1 \
            docker.dragonflydb.io/dragonflydb/dragonfly:latest \
            --port "$port" --proactor_threads="$ncpu" --dbfilename= --maxmemory="${cap_mb}mb" --cache_mode=true \
            ${EXTRA_ARGS:-} >/dev/null
        ;;
    memcached)
        # shellcheck disable=SC2086
        docker run -d --rm --name pace-memcached --network host --cpuset-cpus "$cpus" memcached:latest \
            memcached -p "$port" -t "$ncpu" -m "$cap_mb" ${EXTRA_ARGS:-} >/dev/null
        ;;
    kvstore | kvstore-rdma | kvstore-mc)
        bin="$here/../../arenakv/build/$(uname -m)/arenakvd"
        listen="--port $port"
        if [ "$name" = kvstore-rdma ]; then
            transport="rdma --addr ${RDMA_ADDR:?set RDMA_ADDR to the address of the RDMA interface}"
        else
            transport=tcp
        fi
        if [ "$name" = kvstore-mc ]; then
            listen="--port $((port + 1)) --memcached-port $port"
        fi
        # shellcheck disable=SC2086
        taskset -c "$cpus" "$bin" --transport $transport $listen --threads "$ncpu" --capacity "${cap_mb}M" \
            ${EXTRA_ARGS:-} >"/tmp/pace-$name.log" 2>&1 &
        echo $! >"/tmp/pace-$name.pid"
        ;;
    *)
        echo "unknown server: $name" >&2
        exit 2
        ;;
    esac
    ;;
stop)
    stop_one "$name"
    ;;
stop-all)
    for n in valkey dragonfly memcached kvstore kvstore-rdma kvstore-mc; do
        stop_one "$n"
    done
    ;;
pid)
    case "$name" in
    valkey) comm=valkey-server ;;
    dragonfly) comm=dragonfly ;;
    memcached) comm=memcached ;;
    kvstore | kvstore-rdma | kvstore-mc)
        cat "/tmp/pace-$name.pid"
        exit 0
        ;;
    *)
        echo "unknown server: $name" >&2
        exit 2
        ;;
    esac
    # The container's init process is not the server; select the server binary by name.
    docker top "pace-$name" -eo pid,comm | awk -v c="$comm" '$2 == c { print $1; exit }'
    ;;
*)
    echo "unknown action: $action" >&2
    exit 2
    ;;
esac
