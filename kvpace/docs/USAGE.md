# kvpace usage

`kvpace` generates load against a key-value server and measures throughput and latency. It drives RESP servers
(Redis, Valkey, Dragonfly), memcached text servers, and `arenakvd` over its native protocol, with the same workload
and measurement code for all of them. This guide covers installation, connecting to each kind of server, workloads,
load control, output, and the procedure for comparing servers. The full benchmark suite of this repository, which
runs `kvpace` against several servers, is described in [`../../benchmarks/RUNNING.md`](../../benchmarks/RUNNING.md).

Example outputs below were produced by the commands shown on one machine; the numbers in them are illustrations of
the format, not measurement results.

## Contents

1. [Build and install](#1-build-and-install)
2. [Quick start](#2-quick-start)
3. [Connecting to a server](#3-connecting-to-a-server)
4. [Choosing a mode](#4-choosing-a-mode)
5. [Workloads](#5-workloads)
6. [Load control](#6-load-control)
7. [Reading the text output](#7-reading-the-text-output)
8. [Result files](#8-result-files)
9. [Several load generators](#9-several-load-generators)
10. [Pinning and CPU assignment](#10-pinning-and-cpu-assignment)
11. [Comparing servers](#11-comparing-servers)
12. [Option reference](#12-option-reference)
13. [Exit status and troubleshooting](#13-exit-status-and-troubleshooting)

## 1. Build and install

```
make -C kvpace                    # build/<arch>/kvpace, with the native driver for arenakv
make -C kvpace TLS=1              # TLS for the RESP and memcached drivers: build/<arch>-tls/
make -C kvpace NATIVE=0           # without the native driver (no dependency on ../arenakv and ../rdmaduct)
make -C kvpace test
sudo make -C kvpace install       # /usr/local/bin/kvpace; PREFIX= and DESTDIR= as usual
kvpace --version                  # source revision of the build
kvpace --help                     # every option
```

## 2. Quick start

Against `arenakvd` over its native protocol (TCP), with the server's CPU time recorded:

```
arenakvd --transport tcp --port 7600 --capacity 1G &
kvpace --driver native --driver-opt transport=tcp --port 7600 --threads 4 --duration 10 \
    --server-pid $(pidof arenakvd) --out native.json
```

```
driver=native threads=4 conns/thread=1 pipeline=1 value=64B keys=100000 dist=uniform
ops=4076129 (gets=3668466 puts=407663 misses=0 errors=0) in 10.00 s -> 407610 ops/s
server: 22.640 CPU s during run (5.55 us/op), RSS 16.9 MiB
get latency us: mean 9.8  p50 8.3  p90 17.3  p99 18.9  p99.9 30.1  p99.99 130.0  max 2028.6
set latency us: mean 9.9  p50 8.3  p90 17.4  p99 19.2  p99.9 30.2  p99.99 123.4  max 978.7
service latency us: mean 9.8  p50 8.3  p90 17.3  p99 18.9  p99.9 30.1  p99.99 128.5  max 2028.6
```

Against Valkey and memcached in containers:

```
docker run -d --rm --name valkey --network host valkey/valkey:latest valkey-server --port 6379 --save ''
kvpace --driver resp --port 6379 --threads 2 --conns-per-thread 4 --pipeline 8 --duration 10

docker run -d --rm --name memcached --network host memcached:latest memcached -p 11211 -m 1024
kvpace --driver memcached --port 11211 --threads 4 --rate 50000 --arrival poisson --duration 10
```

Check that the port is free before starting a server (`ss -ltnp 'sport = :6379'`) and that the server is running
(`docker ps`, or the server's start-up line): a server that cannot bind its port exits, and `kvpace` then loads
whatever else listens on that port, writing its key space there.

Before the measurement, `kvpace` writes every key of the key space once (preload), so that GETs hit; then it runs a
warm-up (`--warmup`, default 2 s) whose operations are not counted, then the measurement (`--duration`, default
10 s).

## 3. Connecting to a server

| Server | Driver | Typical options |
|---|---|---|
| Redis, Valkey, Dragonfly, arenakvd `--resp-port` | `resp` | `--port 6379` |
| a Redis Cluster (Valkey, Redis, arenakvd with `--cluster-config`) | `resp-cluster` | `--port` of any node; requests go to the primary of each key's slot, `MOVED` reloads the slot map; one connection per thread (not with `--conns-per-thread` or `--pipeline`) |
| memcached, Dragonfly `--memcached_port`, arenakvd `--memcached-port` | `memcached` | `--port 11211` |
| arenakvd native listener over TCP | `native` | `--driver-opt transport=tcp --port 7600` |
| arenakvd native listener over RDMA | `native` | `--driver-opt transport=rdma --host <RDMA interface address> --port 7600` |

`--host` takes a host name or an IPv4 address (default 127.0.0.1); `--port` defaults to 6379.

**Native driver options** (`--driver-opt KEY=VALUE`, repeatable):

| Option | Default | Meaning |
|---|---|---|
| `transport=tcp\|rdma\|auto` | tcp | must match the server's transport |
| `wait=poll\|event` | event | waiting strategy for RDMA completions; `poll` spins on a core per thread |
| `max-msg=N` | 65536 | 64 to 67108864: message size limit; values above about 64 KiB need a larger limit here and `--max-msg` on the server |
| `depth=N` | 8 | 2 to 4096: buffers per connection; with `--pipeline` above `depth - 1` the depth is raised automatically unless given |

**Authentication.** `--auth SECRET` authenticates every connection: RESP `AUTH SECRET` (or `AUTH user password` for
`user:password`), memcached ASCII authentication (`set` of the credentials as in `memcached -Y`), and the native
`AUTH` request. `--db N` sends `SELECT N` on RESP connections.

**TLS.** TLS needs a build with `make TLS=1` (OpenSSL 1.1.1 or newer; the binary is in `build/<arch>-tls/`). `--tls`
enables it for the `resp` and `memcached` drivers. The server certificate is verified against `--cacert FILE` (or
the system store), and its name or IP address must match `--sni NAME` or the host. `--cert` and `--key` present a
client certificate. `--tls-skip-verify` disables verification for test setups. With the native driver the same
options select an encrypted rdmaduct endpoint (server started with `--native-tls`, over TCP or RDMA); this needs
kvpace built with `TLS=1`, which links the `TLS=1` builds of arenakv and rdmaduct, and `--cacert` is required unless
`--tls-skip-verify` is given (the system store is not used).

```
kvpace --driver resp --host cache.example.net --port 6380 --tls --cacert ca.crt --auth app:secret
```

## 4. Choosing a mode

| Goal | Mode | Command pattern |
|---|---|---|
| Maximum throughput of a server | closed loop with enough connections | `--threads 16` (no `--rate`) |
| The client model of `memtier_benchmark -t T -c C --pipeline P` | multiplexed closed loop | `--threads T --conns-per-thread C --pipeline P` |
| Latency under a given load | open loop | `--rate N --threads 16` |
| A YCSB core workload | preset | `--workload ycsb-b` (E is not available: it needs range scans) |
| A fixed amount of work instead of a fixed time | count mode | `--requests N` (per connection, no warm-up) |
| Saturation behavior, latency versus load | a series of open-loop runs at increasing rates | repeat with `--rate` |
| Effectiveness of a cache for prefix-sharing requests | KV-cache scenario | `--scenario kvcache` |

In a closed loop the offered load adapts to the server, so latency percentiles describe queueing at that
concurrency, not at a given arrival rate. For latency as a function of load use the open loop. A connection can
offer at most `pipeline / service time` operations per second: use enough connections for the target rate (the
output reports the largest schedule lag; a lag of many milliseconds means the generator or the server cannot keep
up).

With `--conns-per-thread 1 --pipeline 1` (the default) each thread calls the driver synchronously. Any other setting
selects the multiplexed runner: each thread serves its connections from one epoll loop and keeps up to `--pipeline`
requests in flight on each. Requests on one connection complete in order, so a slow response delays the responses
queued behind it; this delay is part of the measured latency, as it is for an application that pipelines. In the
KV-cache scenario every pipeline slot runs its own sequence of requests with one block operation in flight, so
`--threads T --conns-per-thread C --pipeline P` makes the requests of `T x C x P` synchronous threads.

## 5. Workloads

| Option | Effect |
|---|---|
| `--keys N` | key space size; keys are the prefix followed by 15 digits |
| `--read-ratio X` | GET with probability X, SET otherwise (the default model, X = 0.9) |
| `--ratio S:G` | SET and GET weights, as in `memtier_benchmark --ratio` |
| `--mix get=W,set=W,del=W,incr=W,mget=W,rmw=W,insert=W` | any weighted mix; unspecified operations have weight 0 |
| `--multi-get N` | keys per MGET (RESP `MGET`, memcached `get k1 k2 ...`, native batched GET) |
| `--dist uniform\|zipf\|sequential\|gaussian\|latest` | key distribution |
| `--theta X` | Zipf skew, 0 <= X < 1 (default 0.99) |
| `--gauss-sd X` | Gaussian standard deviation as a fraction of the key space (default 1/6) |
| `--value-size N`, `A-B`, `S:W,...` | fixed size, uniform range, or sizes with weights (for example `64:8,4096:2`) |
| `--key-prefix TEXT` | key prefix (default `k`, at most 32 bytes, no whitespace) |
| `--workload ycsb-a\|b\|c\|d\|f` | the YCSB core workload mix and distribution (table below); values default to 1000 bytes (10 fields of 100 bytes) unless `--value-size` is given; `--dist`, `--theta`, `--mix`, `--read-ratio` and `--ratio` given with it override the preset |
| `--seed N` | random seed; the same seed and options generate the same sequence of operations and keys in each thread |
| `--no-preload` | skip writing the key space (for a second generator, or a server already filled) |

INCR operates on separate counter keys (the prefix followed by `c`), so that counters never meet the binary values
written by SET. RMW is a GET followed by a SET of the same key; its latency covers both. INSERT writes a key beyond
the current key space and grows it; the `latest` distribution favours the most recently inserted keys.

| YCSB preset | Mix | Distribution |
|---|---|---|
| `ycsb-a` | 50% read, 50% update | Zipf, theta 0.99 |
| `ycsb-b` | 95% read, 5% update | Zipf, theta 0.99 |
| `ycsb-c` | 100% read | Zipf, theta 0.99 |
| `ycsb-d` | 95% read, 5% insert | latest |
| `ycsb-f` | 50% read, 50% read-modify-write | Zipf, theta 0.99 |

**KV-cache scenario.** `--scenario kvcache` replaces single-key operations by requests that model the prefix chains
of an inference KV-cache store: each request reads a chain of blocks until the first miss and writes the missing
tail. Parameters (defaults): `--block-size` (16384 bytes), `--groups` (16 prefix groups), `--shared-blocks` (32
template blocks), `--turn-blocks` (8 blocks per conversation turn), `--active-conv` (256 conversations per thread),
`--max-turns` (16), `--new-conv-prob` (0.05), `--theta` (Zipf skew of groups and conversations). The output reports
the reuse rate, the fraction of needed blocks served from the cache. Model and metrics:
[`kvcache-scenario.md`](kvcache-scenario.md).

```
kvpace --driver native --driver-opt transport=tcp --port 7600 --scenario kvcache --threads 4 --duration 10
```

```
driver=native threads=4 scenario=kvcache block=16384B groups=16 shared-blocks=32 turn-blocks=8 active-conv=256 max-turns=16
ops=2187419 (gets=1883527 puts=303892 misses=24499 errors=0) in 10.00 s -> 218741 ops/s
kvcache: 24499 requests, reuse 85.9% of needed blocks, probe hit rate 98.7% (1859028 of 1883527), 303892 blocks recomputed and written, read 3045.8 MB/s, write 497.9 MB/s, corrupt 0
service latency us: mean 1632.5  p50 1466.4  p90 2687.0  p99 4358.1  p99.9 5046.3  p99.99 5931.0  max 6190.5
request latency us: mean 1632.5  p50 1466.4  p90 2687.0  p99 4358.1  p99.9 5046.3  p99.99 5931.0  max 6190.5
```

**Trace replay.** `--trace FILE` takes the request chains of the KV-cache scenario from a trace instead of the
model: JSON lines in the layout of the Mooncake traces, one request per line with its arrival `timestamp` in
milliseconds and the `hash_ids` of its prompt blocks in order (other fields are ignored; `src/trace.h`). Requests are
started in trace order at their trace time divided by `--trace-speed` (default 1; `0` starts each request as soon
as a request machine is free), probe their blocks until the first miss and write the rest, as modeled requests
do. The run ends with the trace: there is no warm-up, and `--rate`, `--requests` and `--duration` do not apply.
`--block-size` sets the bytes per block id. The JSON result names the trace, its request and block counts and the
speed (`kvcache.trace`).

```
kvpace --driver resp --port 6379 --scenario kvcache --trace conversation_trace.jsonl --block-size 16384 \
       --threads 4 --conns-per-thread 4 --pipeline 4
```

## 6. Load control

| Option | Effect |
|---|---|
| `--threads N` | client threads (1 to 4096) |
| `--conns-per-thread N`, `--pipeline N` | connections per thread and requests in flight per connection |
| `--duration SEC`, `--warmup SEC` | measurement and warm-up length (fractions allowed); the per-second series covers at most 3600 s |
| `--requests N` | requests per connection instead of a duration, without warm-up |
| `--rate N` | open loop: aggregate operations per second over all connections; 0 is a closed loop |
| `--arrival constant\|poisson` | open-loop inter-arrival times |
| `--spin` | open loop: busy-wait until each intended send time instead of sleeping (more accurate schedule, one busy core per thread) |
| `--start-at UNIXTIME` | after the preload, wait until this wall-clock time (synchronized start on several hosts) |
| `--pin LIST` | pin thread i to CPU `LIST[i mod length]` |

In the open loop each operation has an intended send time derived from the rate. Latency is measured from that
time, so a server or generator that falls behind schedule shows the backlog in the latency, and the largest delay
of an actual send behind its intended time is reported as the schedule lag.

## 7. Reading the text output

`kvpace --driver resp --port 16400 --threads 2 --conns-per-thread 4 --pipeline 8 --duration 2` against Valkey 9.1
listening on port 16400:

```
driver=resp threads=2 conns/thread=4 pipeline=8 value=64B keys=100000 dist=uniform
ops=2221880 (gets=1999219 puts=222661 misses=0 errors=0) in 2.00 s -> 1110888 ops/s
get latency us: mean 56.3  p50 49.4  p90 74.2  p99 84.0  p99.9 144.4  p99.99 374.8  max 1163.6
set latency us: mean 56.4  p50 49.4  p90 74.2  p99 84.5  p99.9 139.3  p99.99 378.9  max 1007.9
service latency us: mean 56.1  p50 49.2  p90 73.7  p99 84.0  p99.9 143.4  p99.99 374.8  max 1163.4
```

The first line repeats the configuration. `misses` counts GETs of absent keys; with a preloaded key space and no DEL
in the mix it must be 0 unless the server evicts. `errors` counts failed operations; the exit status is then 3.
`service` is the latency from the actual send; the per-operation lines are from the intended send time in the open
loop (in the closed loop both coincide). An open-loop run adds a line such as
`open loop: offered 50000 ops/s (poisson arrivals), max lag 508.5 us`. With `--server-pid`, a line such as
`server: 22.640 CPU s during run (5.55 us/op), RSS 16.9 MiB` gives the server's CPU time during the measurement, per
operation, and its resident memory. `--percentiles` selects the reported percentiles.

## 8. Result files

Written with `--out FILE`. Schema version 2. Fields (times in nanoseconds unless the name says otherwise):

| Path | Content |
|---|---|
| `schema` | 2 |
| `tool.name`, `tool.commit`, `tool.timestamp` | `kvpace`, git description of the build, UTC time; merged files add `tool.inputs`, `tool.merged_from` (input paths), `tool.input_timestamps` and `tool.threads_total` |
| `environment` | `cpu`, `logical_cpus`, `kernel`, `machine`, `compiler` |
| `config` | all options, including `conns_per_thread`, `pipeline`, `mix`, `value_sizes`, `key_prefix`, `tls`, `auth` (a flag, never the secret), `label`, `argv` |
| `results.seconds`, `ops`, `gets`, `puts`, `misses`, `errors`, `ops_per_sec` | counters of the measurement phase |
| `results.ops_by_type` | completed operations per type: `get`, `set`, `del`, `incr`, `mget`, `rmw`, `insert` |
| `results.mget_keys`, `mget_hits` | keys requested and found by MGET |
| `results.get_latency_ns`, `put_latency_ns`, `service_time_ns` | `count`, `min`, `mean`, `p50`, `p90`, `p99`, `p999`, `max`, and `percentiles` (the values of `--percentiles`) |
| `results.latency_ns.<op>` | the same summary for every operation type with samples |
| `results.max_lag_ns` | largest delay of an actual send behind its intended time |
| `results.series` | completed operations per second of the measurement phase |
| `results.server` | present with `--server-pid`: `cpu_seconds`, `system_cpu_seconds` (kernel-mode part, `stime` of `/proc/<pid>/stat`), `cpu_us_per_op`, `rss_bytes` |
| `results.kvcache` | present for the KV-cache scenario: parameters, `requests`, `block_gets`, `block_hits`, `hit_rate`, `reuse_rate`, `block_puts`, `bytes_get`, `bytes_put`, `corrupt`, `request_latency_ns` |
| `results.histograms` | non-empty buckets of every histogram (`service`, `request`, one per operation) as `[index, count]` pairs of the log-linear histogram described in [`ARCHITECTURE.md`](ARCHITECTURE.md#histogram) |

`get_latency_ns` and `put_latency_ns` keep their schema-1 meaning (GET and SET latency), so readers of schema 1
continue to work. In the KV-cache scenario `gets`, `puts` and `misses` count block operations and the series counts
requests.

`--csv FILE` writes one row per operation type and a `service` row (all operations): `driver`, `operation`,
`count`, `ops_per_sec`, `mean_us`, one column per percentile, `max_us`.

`kvpace --driver resp --port 16400 --workload ycsb-b --keys 10000 --threads 4 --requests 20000 --csv ycsb-b.csv`
against the same server:

```
driver,operation,count,ops_per_sec,mean_us,p50_us,p90_us,p99_us,p99.9_us,p99.99_us,max_us
resp,get,75907,193508.7,19.421,19.199,23.167,27.519,43.263,262.143,456.182
resp,set,4093,10434.2,19.897,19.967,23.423,28.159,47.615,131.268,131.268
resp,service,80000,203942.9,19.445,19.199,23.167,27.519,43.263,227.327,456.182
```

`--hdr-prefix PREFIX` writes `PREFIX_<op>.hgrm` for every operation type with samples, in the percentile
distribution format of HdrHistogram (values in milliseconds, as `memtier_benchmark --hdr-file-prefix` writes them).
The files can be plotted with the HdrHistogram plotter. Values are the bucket upper bounds of the kvpace histogram,
so they carry its relative error of at most 0.8%.

**Server CPU and memory.** `--server-pid PID` samples `/proc/PID/stat` and `/proc/PID/statm` before and after the
measurement. PID is the host process id: `pidof arenakvd`, `docker inspect -f '{{.State.Pid}}' <container>`, or
`kvpace/scripts/servers.sh pid <server>` for servers started with that script. The server must run on the same
host as `kvpace`.

## 9. Several load generators

One client host can be the bottleneck. To load a server from several hosts:

1. start kvpace on every host with the same options, its own `--seed` and `--out`, and the same
   `--start-at UNIXTIME` (a time after the preload completes on all hosts; the clocks must be synchronized, for
   example with NTP or PTP). Use `--no-preload` on all hosts but one;
2. combine the result files: `kvpace merge --out all.json a.json b.json ...`.

```
$ kvpace merge --out all.json a.json b.json
merged 2 results: 450722 ops in 1.00 s -> 450722 ops/s
```

`kvpace merge` sums the counters and the per-second series element by element, takes the longest measurement time,
merges the histograms bucket by bucket and recomputes all percentiles from the merged histograms. The result is a
schema-2 file that `kvpace merge` accepts again. Files of schema 1 have no histograms and are refused. The merge
does not check that the inputs overlapped in time; the `tool.timestamp` of every input is kept in
`tool.input_timestamps` for that check.

## 10. Pinning and CPU assignment

`--pin LIST` pins thread `i` to entry `i mod length`. Pin the generator and the server to disjoint physical cores
and leave hyper-thread siblings of used cores idle (`lscpu -e` lists which logical CPUs share a core); the benchmark
suite documents its assignment in [`../../benchmarks/METHODOLOGY.md`](../../benchmarks/METHODOLOGY.md). Start servers
with `scripts/servers.sh`, which pins containers with `--cpuset-cpus` and native processes with `taskset`:

```
kvpace/scripts/servers.sh start valkey 6379 0,2       # SERVER PORT CPUSET
kvpace/scripts/servers.sh pid valkey
kvpace/scripts/servers.sh stop valkey
```

`servers.sh` knows `valkey`, `dragonfly`, `memcached` (containers on the host network, persistence off), and
`kvstore`, `kvstore-rdma`, `kvstore-mc` (arenakvd over TCP, over RDMA at `RDMA_ADDR`, and serving the memcached
protocol). `CAPACITY` (for example `4gb`) sets the cache size of each, `EXTRA_ARGS` appends server arguments.

## 11. Comparing servers

Use the same command line, CPU assignment and number of repetitions for every server, restart the server for every
run, and compare distributions, not single runs. `benchmarks/tools/run.py` implements this procedure
([`../../benchmarks/RUNNING.md`](../../benchmarks/RUNNING.md)). Results of different client models (synchronous
threads versus multiplexed connections) must not be compared with each other ([validation](validation.md)).

## 12. Option reference

| Option | Default | Accepted values |
|---|---|---|
| `--driver` | resp | `resp`, `memcached`, `native` |
| `--driver-opt K=V` | | native: `transport`, `wait`, `max-msg`, `depth` (section 3) |
| `--host`, `--port` | 127.0.0.1, 6379 | |
| `--threads` | 1 | 1 to 4096 |
| `--conns-per-thread` | 1 | 1 to 65536 |
| `--pipeline` | 1 | 1 to 4096 |
| `--auth` | | `password` or `user:password` |
| `--db` | 0 | RESP database number |
| `--tls`, `--cacert`, `--cert`, `--key`, `--sni`, `--tls-skip-verify` | off | TLS builds only |
| `--keys` | 100000 | 1 or more |
| `--key-prefix` | k | at most 32 bytes, no whitespace |
| `--dist` | uniform | `uniform`, `zipf`, `sequential`, `gaussian`, `latest` |
| `--theta` | 0.99 | 0 <= X < 1 |
| `--gauss-sd` | 1/6 | greater than 0 |
| `--value-size` | 64 | `N`, `A-B`, `S:W,...` |
| `--read-ratio` | 0.9 | 0 to 1 |
| `--ratio` | | `SET:GET` |
| `--mix` | | `op=weight,...` with `get`, `set`, `del`, `incr`, `mget`, `rmw`, `insert` |
| `--multi-get` | 1 | 1 to 4096 |
| `--workload` | | `ycsb-a`, `ycsb-b`, `ycsb-c`, `ycsb-d`, `ycsb-f` |
| `--seed` | 1 | unsigned 64-bit integer |
| `--no-preload` | | |
| `--scenario` | kv | `kv`, `kvcache` |
| `--block-size` | 16384 | 1 to 67108864 |
| `--groups` | 16 | 1 or more |
| `--shared-blocks` | 32 | 0 to 1048576 |
| `--turn-blocks`, `--active-conv`, `--max-turns` | 8, 256, 16 | 1 to 1048576 |
| `--new-conv-prob` | 0.05 | 0 to 1 |
| `--trace` | | file of JSON lines (`timestamp`, `hash_ids`) |
| `--trace-speed` | 1 | 0 or more (0: back to back) |
| `--duration` | 10 | seconds, greater than 0 |
| `--warmup` | 2 | seconds, 0 or more |
| `--requests` | | 1 or more, per connection |
| `--start-at` | | Unix time in seconds |
| `--rate` | 0 | operations per second, 0 or more |
| `--arrival` | constant | `constant`, `poisson` |
| `--spin` | | |
| `--pin` | | comma-separated CPU numbers |
| `--server-pid` | | host PID |
| `--label` | unlabeled | free text stored in the result |
| `--out`, `--csv`, `--hdr-prefix` | | output files |
| `--percentiles` | 50,90,99,99.9,99.99 | comma-separated percentiles |

Invalid values are rejected before any connection is opened, with a message such as
`kvpace: invalid value '0' for --threads (expected a number of threads from 1 to 4096)`.

## 13. Exit status and troubleshooting

| Exit status | Meaning |
|---|---|
| 0 | the run completed without failed operations |
| 1 | setup failure: connection, authentication, preload or TLS setup failed, or an output file cannot be written |
| 2 | invalid options |
| 3 | some operations failed; the result is still written |

| Symptom | Cause | Action |
|---|---|---|
| `worker 0: connect to 127.0.0.1:6379 failed` | no server on that address and port, or a firewall | check `--host`, `--port` and the server's listener |
| results look like another server's | a different process owns the port (the intended server failed to start) | `ss -ltnp 'sport = :PORT'`; the RESP `INFO` or memcached `version` reply names the server |
| `worker 0: preload of key 0 failed`, exit status 1 | the server refused the first SET: it requires authentication (add `--auth`), is full without eviction, or rejects the value size | the server's reply is in its log and statistics; `--no-preload` skips the preload |
| `errors=` above 0 | the server refused operations: full without eviction, entries too large, timeouts | the server's log and statistics name the reason |
| `misses=` above 0 without DEL | the server evicted or expired keys (cache smaller than the key space times the value size) | enlarge the cache or shrink `--keys` |
| large `max lag` in the open loop | not enough connections for the rate, or saturated client CPUs | add `--threads` or connections; check `results.max_lag_ns` |
| native driver: values larger than about 64 KiB fail | message limit | `--driver-opt max-msg=N` and the server's `--max-msg` |
| native driver over RDMA cannot connect | wrong address or transport | use the server's RDMA interface address and `transport=rdma` |
