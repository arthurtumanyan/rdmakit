# kvpace usage

## Choosing a mode

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

With `--conns-per-thread 1 --pipeline 1` (the default) each thread calls the driver synchronously, as all results
of schema 1 did. Any other setting selects the multiplexed runner: each thread serves its connections from one
epoll loop and keeps up to `--pipeline` requests in flight on each. Requests on one connection complete in order,
so a slow response delays the responses queued behind it; this delay is part of the measured latency, as it is for
an application that pipelines. The KV-cache scenario always uses the synchronous runner.

## Workloads

| Option | Effect |
|---|---|
| `--read-ratio X` | GET with probability X, SET otherwise (the default model) |
| `--ratio S:G` | SET and GET weights, as in `memtier_benchmark --ratio` |
| `--mix get=W,set=W,del=W,incr=W,mget=W,rmw=W,insert=W` | any weighted mix; unspecified operations have weight 0 |
| `--multi-get N` | keys per MGET (RESP `MGET`, memcached `get k1 k2 ...`, native batched GET) |
| `--dist`, `--theta`, `--gauss-sd` | key distribution and its parameter |
| `--value-size N`, `A-B`, `S:W,...` | fixed size, uniform range, or sizes with weights |
| `--key-prefix TEXT` | keys are TEXT followed by 15 digits (at most 32 prefix bytes) |
| `--workload ycsb-a\|b\|c\|d\|f` | the YCSB core workload mix and distribution; values default to 1000 bytes (10 fields of 100 bytes) unless `--value-size` is given |

INCR operates on separate counter keys (the prefix followed by `c`), so that counters never meet the binary values
written by SET. RMW is a GET followed by a SET of the same key; its latency covers both. INSERT writes a key beyond
the current key space and grows it; the `latest` distribution favours the most recently inserted keys.

## Authentication and TLS

`--auth SECRET` authenticates every connection: RESP `AUTH SECRET` (or `AUTH user password` for `user:password`),
memcached ASCII authentication (`set` of the credentials as in `memcached -Y`), and the native `AUTH` request.
`--db N` sends `SELECT N` on RESP connections.

TLS needs a build with `make TLS=1` (OpenSSL 1.1.1 or newer; the binary is in `build/<arch>-tls/`). `--tls`
enables it for the `resp` and `memcached` drivers. The server certificate is verified against `--cacert FILE` (or
the system store), and its name or IP address must match `--sni NAME` or the host. `--cert` and `--key` present a
client certificate. `--tls-skip-verify` disables verification for test setups. The native driver does not support
TLS.

## Reading the text output

```
driver=resp threads=4 conns/thread=4 pipeline=4 value=64B keys=100000 dist=uniform
ops=2949360 (gets=2681079 puts=268281 misses=0 errors=0) in 1.00 s -> 2949179 ops/s
server: 6.080 CPU s during run (2.06 us/op), RSS 17.2 MiB
get latency us: mean 18.4  p50 17.2  p90 26.9  p99 41.7  p99.9 45.1  p99.99 55.6  max 1107.0
set latency us: ...
service latency us: ...
```

`misses` counts GETs of absent keys; with a preloaded key space and no DEL in the mix it must be 0 unless the
server evicts. `errors` counts failed operations; the exit status is then 3. `service` is the latency from the
actual send; the per-operation lines are from the intended send time in the open loop. `--percentiles` selects the
reported percentiles.

## Result files

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
| `results.server` | present with `--server-pid`: `cpu_seconds`, `cpu_us_per_op`, `rss_bytes` |
| `results.kvcache` | present for the KV-cache scenario: parameters, `requests`, `block_gets`, `block_hits`, `hit_rate`, `reuse_rate`, `block_puts`, `bytes_get`, `bytes_put`, `corrupt`, `request_latency_ns` |
| `results.histograms` | non-empty buckets of every histogram (`service`, `request`, one per operation) as `[index, count]` pairs of the log-linear histogram described in [`ARCHITECTURE.md`](ARCHITECTURE.md#histogram) |

`get_latency_ns` and `put_latency_ns` keep their schema-1 meaning (GET and SET latency), so readers of schema 1
continue to work. In the KV-cache scenario `gets`, `puts` and `misses` count block operations and the series counts
requests.

`--csv FILE` writes one row per operation type and a `service` row: `driver`, `operation`, `count`, `ops_per_sec`,
`mean_us`, one column per percentile, `max_us`.

`--hdr-prefix PREFIX` writes `PREFIX_<op>.hgrm` for every operation type with samples, in the percentile
distribution format of HdrHistogram (values in milliseconds, as `memtier_benchmark --hdr-file-prefix` writes them).
The files can be plotted with the HdrHistogram plotter. Values are the bucket upper bounds of the kvpace histogram,
so they carry its relative error of at most 0.8%.

## Several load generators

One client host can be the bottleneck. To load a server from several hosts:

1. start kvpace on every host with the same options, its own `--seed` and `--out`, and the same
   `--start-at UNIXTIME` (a time after the preload completes on all hosts; the clocks must be synchronized, for
   example with NTP or PTP). Use `--no-preload` on all hosts but one;
2. combine the result files: `kvpace merge --out all.json a.json b.json ...`.

`kvpace merge` sums the counters and the per-second series element by element, takes the longest measurement time,
merges the histograms bucket by bucket and recomputes all percentiles from the merged histograms. The result is a
schema-2 file that `kvpace merge` accepts again. Files of schema 1 have no histograms and are refused. The merge
does not check that the inputs overlapped in time; the `tool.timestamp` of every input is kept in
`tool.input_timestamps` for that check.

## Pinning and CPU assignment

`--pin LIST` pins thread `i` to entry `i mod length`. Pin the generator and the server to disjoint physical cores
and leave hyper-thread siblings of used cores idle; the benchmark suite documents its assignment in
[`../benchmarks/METHODOLOGY.md`](../../benchmarks/METHODOLOGY.md). Start servers with `scripts/servers.sh`, which pins
containers with `--cpuset-cpus` and native processes with `taskset`.

## Comparing servers

Use the same command line, CPU assignment and number of repetitions for every server, restart the server for every
run, and compare distributions, not single runs. `benchmarks/tools/run.py` implements this procedure.
