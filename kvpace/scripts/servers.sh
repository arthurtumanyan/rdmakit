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
#   APPENDFSYNC  always|everysec|no: valkey and kvstore keep an append-only log with this fsync policy in
#                DATA_DIR/<server> (emptied at start; default DATA_DIR /var/tmp/pace-data, which must be on the
#                disk to measure, not tmpfs). Unset: no log.
#   REPLICA_CPUS start a replica of valkey or kvstore pinned to this CPU set: valkey on PORT + 100 (a second
#                container, pace-<server>-replica), kvstore with its native listener on PORT + 100 following
#                PORT. Stopped together with the primary.
#   TLS_DIR      directory with server.crt and server.key (PEM). valkey: TLS on PORT (plain port off);
#                kvstore: the native listener encrypted (--native-tls); kvstore-resp: RESP over TLS on PORT.
#                The arenakvd of the TLS build (build/<arch>-tls) is used for every kvstore id when TLS_DIR or
#                AKV_TLS_BUILD=1 is set, so that plain and encrypted runs use the same binary.
#
# kvstore-resp serves RESP2 on PORT (native protocol on PORT + 1), for comparisons with valkey over one protocol.
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
    docker rm -f "pace-$1" "pace-$1-replica" >/dev/null 2>&1 || true
    local pids=()
    for f in "/tmp/pace-$1.pid" "/tmp/pace-$1-replica.pid"; do
        if [ -f "$f" ]; then
            pids+=("$(cat "$f")")
            kill "${pids[-1]}" 2>/dev/null || true
            rm -f "$f"
        fi
    done
    # Waits (at most 10 s) until the processes have exited, so that the next start finds the ports free.
    for _ in $(seq 1 100); do
        local alive=0
        for p in "${pids[@]}"; do
            kill -0 "$p" 2>/dev/null && alive=1
        done
        [ "$alive" = 0 ] && break
        sleep 0.1
    done
}

# Prints an empty data directory for server $1 (the append-only log lives there).
data_dir() {
    local d="${DATA_DIR:-/var/tmp/pace-data}/$1"
    rm -rf "$d"
    mkdir -p "$d"
    echo "$d"
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
        vol=()
        persist=(--appendonly no)
        if [ -n "${APPENDFSYNC:-}" ]; then
            d=$(data_dir valkey)
            # As the invoking user, so that the log files stay removable by the next data_dir call.
            vol=(-v "$d:/data" --user "$(id -u):$(id -g)")
            persist=(--appendonly yes --appendfsync "$APPENDFSYNC" --dir /data)
        fi
        listen=(--port "$port")
        if [ -n "${TLS_DIR:-}" ]; then
            vol+=(-v "$TLS_DIR:/tls:ro")
            listen=(--port 0 --tls-port "$port" --tls-cert-file /tls/server.crt --tls-key-file /tls/server.key
                --tls-auth-clients no)
        fi
        # shellcheck disable=SC2086
        docker run -d --rm --name pace-valkey --network host --cpuset-cpus "$cpus" "${vol[@]}" valkey/valkey:latest \
            valkey-server "${listen[@]}" --save "" "${persist[@]}" --protected-mode no --io-threads "$ncpu" \
            --maxmemory "${cap_mb}mb" --maxmemory-policy allkeys-lru ${EXTRA_ARGS:-} >/dev/null
        if [ -n "${REPLICA_CPUS:-}" ]; then
            docker run -d --rm --name pace-valkey-replica --network host --cpuset-cpus "$REPLICA_CPUS" \
                valkey/valkey:latest valkey-server --port $((port + 100)) --save "" --appendonly no \
                --protected-mode no --replicaof 127.0.0.1 "$port" --maxmemory "${cap_mb}mb" \
                --maxmemory-policy allkeys-lru >/dev/null
        fi
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
    kvstore | kvstore-rdma | kvstore-mc | kvstore-resp)
        variant=""
        if [ -n "${TLS_DIR:-}" ] || [ "${AKV_TLS_BUILD:-0}" = 1 ]; then
            variant=-tls
        fi
        bin="$here/../../arenakv/build/$(uname -m)$variant/arenakvd"
        listen="--port $port"
        if [ "$name" = kvstore-rdma ]; then
            transport="rdma --addr ${RDMA_ADDR:?set RDMA_ADDR to the address of the RDMA interface}"
        else
            transport=tcp
        fi
        if [ "$name" = kvstore-mc ]; then
            listen="--port $((port + 1)) --memcached-port $port"
        fi
        if [ "$name" = kvstore-resp ]; then
            listen="--port $((port + 1)) --resp-port $port"
        fi
        persist=""
        if [ -n "${APPENDFSYNC:-}" ]; then
            persist="--appendonly $(data_dir "$name") --appendfsync $APPENDFSYNC"
        fi
        tls=""
        if [ -n "${TLS_DIR:-}" ]; then
            tls="--tls-cert $TLS_DIR/server.crt --tls-key $TLS_DIR/server.key"
            if [ "$name" = kvstore-resp ]; then
                listen="--port $((port + 1)) --tls-resp-port $port"
            else
                tls="$tls --native-tls"
            fi
        fi
        # shellcheck disable=SC2086
        taskset -c "$cpus" "$bin" --transport $transport $listen --threads "$ncpu" --capacity "${cap_mb}M" \
            $persist $tls ${EXTRA_ARGS:-} >"/tmp/pace-$name.log" 2>&1 &
        echo $! >"/tmp/pace-$name.pid"
        if [ -n "${REPLICA_CPUS:-}" ]; then
            nrep=$(echo "$REPLICA_CPUS" | tr ',' '\n' | wc -l)
            primary_port=$port
            if [ "$name" = kvstore-mc ] || [ "$name" = kvstore-resp ]; then
                primary_port=$((port + 1))
            fi
            # shellcheck disable=SC2086
            taskset -c "$REPLICA_CPUS" "$bin" --transport tcp --port $((port + 100)) --threads "$nrep" \
                --capacity "${cap_mb}M" --replicaof "127.0.0.1:$primary_port" $tls \
                >"/tmp/pace-$name-replica.log" 2>&1 &
            echo $! >"/tmp/pace-$name-replica.pid"
        fi
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
    for n in valkey dragonfly memcached kvstore kvstore-rdma kvstore-mc kvstore-resp; do
        stop_one "$n"
    done
    ;;
pid)
    case "$name" in
    valkey) comm=valkey-server ;;
    dragonfly) comm=dragonfly ;;
    memcached) comm=memcached ;;
    kvstore | kvstore-rdma | kvstore-mc | kvstore-resp)
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
