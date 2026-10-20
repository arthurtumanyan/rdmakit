# kvpace usage

## Choosing a mode

| Goal | Mode | Command pattern |
|---|---|---|
| Maximum throughput of a server | closed loop with enough connections | `--threads 16` (no `--rate`) |
| Latency under a given load | open loop | `--rate N --threads 16` |
| Saturation behavior, latency versus load | a series of open-loop runs at increasing rates | repeat with `--rate` |
| Effectiveness of a cache for prefix-sharing requests | KV-cache scenario | `--scenario kvcache` |

In a closed loop the offered load adapts to the server, so latency percentiles describe queueing at that
concurrency, not at a given arrival rate. For latency as a function of load use the open loop. A connection can
offer at most `1 / service time` operations per second: use enough threads for the target rate (the output
reports the largest schedule lag; a lag of many milliseconds means the generator or the server cannot keep up).

## Reading the text output

```
driver=resp threads=16 value=64B read_ratio=0.90 keys=100000 dist=uniform
ops=1613671 (gets=1452514 puts=161157 misses=0 errors=0) in 8.00 s -> 201706 ops/s
server: 8.000 CPU s during run (4.96 us/op), RSS 23.0 MiB
get latency us: mean 79.3  p50 75.8  p90 92.2  p99 145.4  p99.9 245.8  max 3872.4
put latency us: ...
service latency us: ...
```

`misses` counts GETs of absent keys; with a preloaded key space it must be 0 unless the server evicts. `errors`
counts failed operations; the exit status is then 3. `service` is the latency from the actual send, `get` and
`put` are from the intended send time in the open loop.

## Result files

Written with `--out FILE`. Schema version 1. Fields (times in nanoseconds unless the name says otherwise):

| Path | Content |
|---|---|
| `schema` | 1 |
| `tool.commit`, `tool.timestamp` | git description of the build, UTC time |
| `environment` | `cpu`, `logical_cpus`, `kernel`, `machine`, `compiler` |
| `config` | all options, `label`, `argv` |
| `results.seconds`, `ops`, `gets`, `puts`, `misses`, `errors`, `ops_per_sec` | counters of the measurement phase |
| `results.get_latency_ns`, `put_latency_ns`, `service_time_ns` | `count`, `min`, `mean`, `p50`, `p90`, `p99`, `p999`, `max` |
| `results.max_lag_ns` | largest delay of an actual send behind its intended time |
| `results.series` | completed operations per second of the measurement phase |
| `results.server` | present with `--server-pid`: `cpu_seconds`, `cpu_us_per_op`, `rss_bytes` |
| `results.kvcache` | present for the KV-cache scenario: parameters, `requests`, `block_gets`, `block_hits`, `hit_rate`, `reuse_rate`, `block_puts`, `bytes_get`, `bytes_put`, `corrupt`, `request_latency_ns` |

In the KV-cache scenario `gets`, `puts` and `misses` count block operations and the series counts requests.

## Pinning and CPU assignment

`--pin LIST` pins thread `i` to entry `i mod length`. Pin the generator and the server to disjoint physical cores
and leave hyper-thread siblings of used cores idle; the benchmark suite documents its assignment in
[`../benchmarks/METHODOLOGY.md`](../../benchmarks/METHODOLOGY.md). Start servers with `scripts/servers.sh`, which pins
containers with `--cpuset-cpus` and native processes with `taskset`.

## Comparing servers

Use the same command line, CPU assignment and number of repetitions for every server, restart the server for every
run, and compare distributions, not single runs. `benchmarks/tools/run.py` implements this procedure.
