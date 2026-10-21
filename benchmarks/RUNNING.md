# Running the benchmark suite

How to produce a new run directory like `data/2026-10-21-run3`, turn it into figures and tables, and check it. The
method behind the suites (definitions, CPU assignment, threats to validity) is in [`METHODOLOGY.md`](METHODOLOGY.md);
the published results are in [`README.md`](README.md). Running `kvpace` by hand against one server is described in
[`../kvpace/docs/USAGE.md`](../kvpace/docs/USAGE.md).

## Contents

1. [Requirements](#1-requirements)
2. [Preparing the machine](#2-preparing-the-machine)
3. [Running](#3-running)
4. [Suites](#4-suites)
5. [Output](#5-output)
6. [Reports and verification](#6-reports-and-verification)
7. [Publishing a run](#7-publishing-a-run)
8. [Troubleshooting](#8-troubleshooting)

## 1. Requirements

| Item | Needed for | Check |
|---|---|---|
| Linux x86-64 or aarch64 with at least 16 logical CPUs, or CPU sets chosen for the machine ([section 2](#cpu-sets)) | all suites | `nproc`, `lscpu -e` |
| the projects built: `make` at the repository root | all suites | `kvpace/build/$(uname -m)/kvpace --version` |
| Docker, with the images `valkey/valkey:latest`, `docker.dragonflydb.io/dragonflydb/dragonfly:latest`, `memcached:latest`, `redislabs/memtier_benchmark:latest` | matrix, sweep, pipeline, connections, capacity, validation | `docker pull` each image once |
| an RDMA device (Soft-RoCE suffices) and its interface address in `RDMA_ADDR` | the Soft-RoCE rows of the matrix, the transport suite | `rdma link show` |
| `BENCH_NIC`: the network interface name, recorded with its speed | environment record | `ip link` |
| Python 3 with matplotlib | reports and verification | `python3 -c 'import matplotlib'` |
| free ports 7100 and 7101 on the loopback interface | all servers listen there | `ss -ltn 'sport = :7100'` |

Soft-RoCE on an Ethernet interface:

```
sudo modprobe rdma_rxe
sudo rdma link add rxe0 type rxe netdev enp4s0
rdma link show
ip -4 addr show dev enp4s0          # its address is RDMA_ADDR
ulimit -l unlimited                  # memory registration in this shell
```

## 2. Preparing the machine

**Clean tree.** Commit or stash changes first. The run records the source commit and the number of uncommitted
files (`env.json`: `git_commit`, `git_uncommitted_files`), and `tools/verify.py` warns about a run made from a tree
with uncommitted files.

**Other load.** Stop containers and services that are not part of the benchmark and that use CPU. The run records
the number of running containers and the 1-minute load average at its start (`running_containers`,
`load_avg_1min`). Restart the stopped containers afterwards:

```
docker ps --format '{{.Names}}' > stopped-containers.txt
xargs -r docker stop < stopped-containers.txt
# ... run ...
xargs -r docker start < stopped-containers.txt
```

**CPU frequency.** The `performance` governor removes frequency ramp-up from the measurements; run3 used it, run1
and run2 used `powersave`. The run records the distinct governors of all CPUs (`cpu_governor`).

```
sudo cpupower frequency-set -g performance
cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor | sort | uniq -c
# afterwards: sudo cpupower frequency-set -g powersave
```

<a id="cpu-sets"></a>**CPU sets.** The server and the load generator run on distinct physical cores, one logical CPU
per core. The defaults are those of the measured machine (Intel Core i9-12900K, where logical CPUs 2k and 2k+1 share
a P-core): server on CPU 0 (one-CPU rows) or CPUs 0, 2, 4, 6 (four-CPU rows), load generator on CPUs 8, 10, 12, 14.
On another machine choose sets from `lscpu -e` (column CORE) and set:

```
export BENCH_SERVER_CPU=0              # one-CPU configurations
export BENCH_SERVER_CPUS4=0,2,4,6      # four-CPU configurations
export BENCH_CLIENT_CPUS=8,10,12,14    # load generator (four threads)
```

The sets in effect appear in every recorded command line of the manifest.

## 3. Running

```
export RDMA_ADDR=192.0.2.10 BENCH_NIC=enp4s0       # the RDMA interface address and its interface
python3 benchmarks/tools/run.py --suite all --reps 5 --out benchmarks/data/<date>-run<N>
python3 benchmarks/tools/run.py --suite capacity-evict --reps 5 --out benchmarks/data/<date>-run<N>
```

`--suite` may be repeated to run a subset (`--suite pipeline --suite connections`). `--quick` runs every
configuration once with short durations to check the setup; its output is not meant for publication. An existing
output directory is extended: `env.json` is kept and new runs are appended to `manifest.json`, which is how
`capacity-evict` is added to the run of `all`.

Repetitions are interleaved: repetition 1 of every configuration runs before repetition 2 of any configuration, in a
shuffled order, so that slow drift of the machine does not correlate with one configuration. Every run starts a
fresh server. Progress is printed per run (`[matrix] valkey-1cpu-kvcache-r1 ok`), and the last line summarizes the
statuses:

```
done: 690 runs, 0 failed, 20 with operation errors, 10 skipped
```

**Duration.** Run3 (five repetitions) recorded the following run times in its manifest (sum of `wall_s`); server
starts and stops add to them, and the whole procedure took about 2.5 hours.

| Suite | Runs | Minutes |
|---|---|---|
| matrix | 180 | 38.9 |
| sweep | 140 | 23.2 |
| pipeline | 75 | 12.3 |
| connections | 100 | 16.7 |
| capacity (including capacity-evict) | 115 | 25.4 |
| engine | 15 | 4.5 |
| transport | 15 | 0.5 |
| validation | 50 | 10.3 |

Do not build, test or run other benchmarks on the machine while a run is in progress.

## 4. Suites

| Suite | What it measures | Systems |
|---|---|---|
| `matrix` | closed-loop throughput, latency, server CPU and memory; {1, 4} server CPUs x {64-byte values, 16 KiB values, KV-cache} | Valkey, Dragonfly, memcached, arenakv over TCP, over Soft-RoCE, and serving the memcached protocol |
| `sweep` | open-loop latency from the intended send time versus offered load, 20,000 to 260,000 operations per second | Valkey, Dragonfly, memcached, arenakv (TCP) |
| `pipeline` | 16 connections with 1, 8 or 32 requests in flight | the five TCP servers |
| `connections` | 16 to 1024 connections, one request in flight each | the five TCP servers |
| `capacity` | KV-cache reuse rate versus configured capacity, 16 MB to 4 GB | Valkey, Dragonfly, memcached, arenakv |
| `capacity-evict` | the capacity suite for Dragonfly with `--max_eviction_per_heartbeat` raised (not part of `all`) | Dragonfly |
| `engine` | in-process engine throughput by thread count, and micro-benchmarks (`akv_bench`) | arenakv engine |
| `transport` | rdmaduct ping-pong latency and streaming throughput on loopback, TCP and Soft-RoCE (`duct_bench`) | rdmaduct |
| `validation` | kvpace against memtier_benchmark on the same servers and settings | Valkey, arenakv (memcached protocol) |

The workloads, server arguments and client options of every suite are defined in
[`tools/run.py`](tools/run.py) and [`../kvpace/scripts/servers.sh`](../kvpace/scripts/servers.sh); the
manifest records the exact command line of every run.

## 5. Output

```
DIR/env.json            machine, kernel, compiler, governors, NIC, RDMA devices, image digests, server versions,
                        git commit and uncommitted files, running containers, load average
DIR/manifest.json       one entry per run: suite, parameters, command line, status, wall time, result file
DIR/<suite>/*.json      raw results, one file per run (kvpace, akv_bench, duct_bench or memtier format)
DIR/<suite>/*.err       standard error of runs that failed
```

| Status in the manifest | Meaning |
|---|---|
| `ok` | the run completed and every operation succeeded |
| `errors` | the run completed, some operations failed (for example a full cache that refuses stores); the result is kept and the error rate is reported |
| `failed` | no result: the server did not start or the tool exited with a setup failure; see the `.err` file |
| `skipped` | the configuration cannot run (Dragonfly refuses capacities below its minimum memory per thread); the reason is recorded |

The address in `RDMA_ADDR` is replaced by the documentation address 192.0.2.10 (RFC 5737) in every recorded
command and result file. Raw files are never edited afterwards.

## 6. Reports and verification

```
python3 benchmarks/tools/report.py benchmarks/data/<run>                 # figures/ and generated/ only
python3 benchmarks/tools/report.py benchmarks/data/<run> --update-docs   # also the tables in the Markdown files
python3 benchmarks/tools/verify.py benchmarks/data/<run>                 # check everything against the raw data
make verify                                                              # verify.py on the run named in LATEST
```

`report.py` reads only the run directory. It writes the figures (light and dark), the tables in `generated/`, and
`generated/summary.json` with the aggregated numbers and a digest of the inputs; with `--update-docs` it replaces
every block between `<!-- BEGIN GENERATED:name -->` and `<!-- END GENERATED:name -->` in the repository's Markdown
files. `verify.py` recomputes the summary from the raw data, redraws every figure and compares it byte for byte,
compares every generated table, checks that every image referenced from Markdown exists, and evaluates every claim
in [`tools/claims.py`](tools/claims.py) that the documents cite as `[C<n>]`.

## 7. Publishing a run

1. Write the run name into [`LATEST`](LATEST) and run `report.py ... --update-docs`.
2. Run `make verify`. Claims that no longer hold for the new data are reported as `FAIL: claim C<n> does not hold`.
   Rewrite each such statement and its condition in `claims.py` from the new data, in the same commit as the
   documents that cite it; a claim is never adjusted to fit a number it does not describe.
3. Describe the provenance of the run in [`README.md`](README.md#environment-of-the-run): commit, governor,
   what was stopped, how it differs from earlier runs.
4. Commit the run directory, the figures, the generated tables, `LATEST`, `claims.py` and the documents together.
   Earlier run directories stay unchanged.

## 8. Troubleshooting

| Symptom | Cause | Action |
|---|---|---|
| `missing .../kvpace (build the projects first ...)` | the projects are not built | `make` at the repository root |
| `RDMA_ADDR must name the IPv4 address of the RDMA interface` | a Soft-RoCE configuration without `RDMA_ADDR` | set it, or run suites without RDMA rows |
| `server ... did not open port 7100` | the server failed to start, or the port is taken | `ss -ltnp 'sport = :7100'`; `/tmp/pace-<server>.log` holds the log of native servers, `docker logs pace-<server>` of containers while they run |
| many `failed` runs of one system | a missing image, or Docker not reachable | `docker pull` the image; `docker info` |
| `errors` in capacity runs of memcached or Dragonfly | full caches refusing stores at small capacities | expected; reported as the error rate (see [`README.md`](README.md)) |
| `skipped` runs of Dragonfly | capacity below Dragonfly's minimum memory per thread | expected |
| `verify.py` reports a changed figure | matplotlib differs from the version that drew the figures | use the matplotlib version of CI (3.6.3) |
| high run-to-run variation (`cv`) | other load, frequency scaling, a shared core | section 2 |
