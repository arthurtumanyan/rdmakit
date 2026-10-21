#!/bin/bash
# Prints a JSON document describing the machine, toolchain and software versions of a benchmark run.
#   env-info.sh [REPO_ROOT]
# Everything is read from the running system; nothing is assumed. Missing tools are reported as null.
set -u
root=${1:-$(cd "$(dirname "$0")/../.." && pwd)}

json_str() { python3 -c 'import json,sys; print(json.dumps(sys.stdin.read().strip()))'; }
first() { "$@" 2>/dev/null | head -1 | json_str; }

cpu_model=$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | json_str)
# All distinct governors of the online CPUs, comma-separated (one value when every CPU uses the same).
governor=$(cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort -u | paste -sd, | json_str)
mem_kb=$(awk '/MemTotal/ {print $2}' /proc/meminfo)
nic=${BENCH_NIC:-}
nic_speed=null
if [ -n "$nic" ] && command -v ethtool >/dev/null; then
    s=$(ethtool "$nic" 2>/dev/null | awk '/Speed:/ {print $2}' | tr -d 'Mb/s')
    [ -n "$s" ] && nic_speed=$s
fi
git_commit=$(git -C "$root" rev-parse HEAD 2>/dev/null | json_str)
git_dirty=$(git -C "$root" status --porcelain 2>/dev/null | wc -l)
docker_ver=$(docker version --format '{{.Server.Version}}' 2>/dev/null | json_str)
# Containers already running when the run starts compete with the measured processes for CPU and memory.
containers=$(docker ps -q 2>/dev/null | wc -l)
loadavg=$(cut -d' ' -f1 /proc/loadavg)

image_id() {
    docker image inspect "$1" --format '{{index .RepoDigests 0}}' 2>/dev/null | json_str
}

cat <<JSON
{
  "timestamp_utc": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "hostname_hash": "$(hostname | sha256sum | cut -c1-12)",
  "cpu_model": $cpu_model,
  "logical_cpus": $(nproc --all),
  "cpu_governor": $governor,
  "memory_kib": $mem_kb,
  "kernel": $(uname -r | json_str),
  "machine": "$(uname -m)",
  "os": $(. /etc/os-release 2>/dev/null && echo "$PRETTY_NAME" | json_str),
  "compiler": $(first cc --version),
  "libibverbs": $(first dpkg-query -W -f='${Version}' libibverbs1),
  "nic": "${nic}",
  "nic_speed_mbit": $nic_speed,
  "rdma_devices": $(rdma link show 2>/dev/null | json_str),
  "docker": $docker_ver,
  "running_containers": $containers,
  "load_avg_1min": $loadavg,
  "images": {
    "valkey": $(image_id valkey/valkey:latest),
    "dragonfly": $(image_id docker.dragonflydb.io/dragonflydb/dragonfly:latest),
    "memcached": $(image_id memcached:latest),
    "memtier": $(image_id redislabs/memtier_benchmark:latest)
  },
  "valkey_version": $(docker run --rm valkey/valkey:latest valkey-server --version 2>/dev/null | json_str),
  "memcached_version": $(docker run --rm memcached:latest memcached --version 2>/dev/null | json_str),
  "dragonfly_version": $(docker run --rm docker.dragonflydb.io/dragonflydb/dragonfly:latest dragonfly --version 2>&1 | head -1 | sed "s/\x1b\[[0-9;]*m//g" | json_str),
  "git_commit": $git_commit,
  "git_uncommitted_files": $git_dirty
}
JSON
