"""Machine-checkable statements made in the documentation.

Each claim has an identifier, the statement as written in the documents, and a function that evaluates it
on the summary produced by bench_data.summarize(). Documents cite a claim as [C<number>]. verify.py fails
when a cited claim does not hold, when a document cites an unknown claim, or when a claim is not cited, so
prose and data cannot drift apart silently. Claims are written after looking at the data and are phrased so
that the condition below is exactly what the sentence says. "Mean" refers to the mean over repetitions;
"ranges do not overlap" means the smallest repetition of one system is larger than the largest repetition of
the other. A claim listed in _RUN is evaluated on the summary of that earlier run directory (a sibling
of the verified run) instead; such claims describe how an earlier run differed and name the run in their text.
"""

TCP_SYSTEMS = ["kvstore", "kvstore-mc", "valkey", "dragonfly", "memcached"]
ALL_SYSTEMS = ["kvstore", "kvstore-rdma", "kvstore-mc", "valkey", "dragonfly", "memcached"]
SWEEP_SYSTEMS = ["kvstore", "valkey", "dragonfly", "memcached"]
WORKLOADS = ["small", "block", "kvcache"]


def cell(s, system, cpus, wl, metric):
    return s["matrix"]["%s|%d|%s" % (system, cpus, wl)][metric]


def mean(s, system, cpus, wl, metric="throughput"):
    return cell(s, system, cpus, wl, metric)["mean"]


def above(a, b):
    """Ranges do not overlap and a is higher."""
    return a["min"] > b["max"]


def sweep(s, system, rate, metric):
    return s["sweep"]["%s|%d" % (system, rate)][metric]


def cap(s, system, c, metric):
    return s["capacity"]["%s|%s" % (system, c)][metric]


def loop(s, section, system, x, metric="throughput"):
    return s[section]["%s|%d" % (system, x)][metric]


def c1(s):
    return all(max(ALL_SYSTEMS, key=lambda x: mean(s, x, cpus, wl)) in ("kvstore", "kvstore-mc")
               for cpus in (1, 4) for wl in WORKLOADS)


def c2(s):
    return all(above(cell(s, "kvstore", 4, wl, "throughput"), cell(s, other, 4, wl, "throughput"))
               for wl in WORKLOADS for other in ("valkey", "dragonfly"))


def c3(s):
    return all(above(cell(s, "kvstore-mc", 1, wl, "throughput"), cell(s, "memcached", 1, wl, "throughput"))
               for wl in WORKLOADS)


def c4(s):
    def lowest(cpus, wl):
        return min(TCP_SYSTEMS, key=lambda x: mean(s, x, cpus, wl, "server_cpu_us_per_op"))
    rows = [(cpus, wl) for cpus in (1, 4) for wl in WORKLOADS if (cpus, wl) != (4, "kvcache")]
    last = mean(s, "kvstore", 4, "kvcache", "server_cpu_us_per_op") / mean(s, "memcached", 4, "kvcache", "server_cpu_us_per_op")
    return (all(lowest(cpus, wl) in ("kvstore", "kvstore-mc") for cpus, wl in rows)
            and lowest(4, "kvcache") == "memcached" and last < 1.02)


def c5(s):
    return all(mean(s, "kvstore", 4, wl, "server_cpu_us_per_op") < mean(s, other, 4, wl, "server_cpu_us_per_op")
               for wl in WORKLOADS for other in ("valkey", "dragonfly"))


def c6(s):
    return all(mean(s, "kvstore-rdma", cpus, wl) < mean(s, "kvstore", cpus, wl) for cpus in (1, 4) for wl in WORKLOADS)


def c7(s):
    mine = sweep(s, "kvstore", 260000, "achieved_ops")
    return (mine["min"] > 0.99 * 260000
            and all(sweep(s, other, 260000, "achieved_ops")["max"] < 240000 for other in ("valkey", "dragonfly", "memcached")))


def c8(s):
    return all(abs(sweep(s, system, rate, "achieved_ops")["mean"] - rate) / rate < 0.01
               for system in SWEEP_SYSTEMS for rate in (20000, 60000, 100000, 140000))


def c9(s):
    return all(sweep(s, system, 260000, "p99_us")["mean"] > 1000 * sweep(s, system, 260000, "service_p99_us")["mean"]
               for system in ("valkey", "dragonfly", "memcached"))


def c10(s):
    cells = [sweep(s, system, rate, "p99_us")["cv"] for system in SWEEP_SYSTEMS for rate in (20000, 60000, 100000, 140000)]
    return len(cells) == 16 and sum(c > 0.20 for c in cells) >= len(cells) / 2


def c11(s):
    return all(above(cap(s, "kvstore", c, "reuse_rate"), cap(s, "valkey", c, "reuse_rate")) for c in ("16mb", "64mb", "256mb"))


def c12(s):
    def diff(c):
        return cap(s, "memcached", c, "reuse_rate")["mean"] - cap(s, "kvstore", c, "reuse_rate")["mean"]
    return all(diff(c) > 0 for c in ("16mb", "64mb", "256mb", "1gb")) and all(diff(c) < 0.03 for c in ("256mb", "1gb"))


def c13(s):
    vals = [cap(s, system, "4gb", "reuse_rate")["mean"] for system in ("kvstore", "valkey", "dragonfly", "dragonfly-evict", "memcached")]
    return max(vals) - min(vals) < 0.005


def c14(s):
    return (cap(s, "dragonfly", "256mb", "error_rate")["mean"] > 0.90 and cap(s, "dragonfly", "1gb", "error_rate")["mean"] > 0.80
            and all(cap(s, "dragonfly-evict", c, "error_rate")["max"] == 0 for c in ("256mb", "1gb", "4gb")))


def c15(s):
    caps = ("16mb", "64mb", "256mb", "1gb", "4gb")
    return (all(cap(s, "memcached", c, "error_rate")["max"] < 0.001 for c in caps)
            and all(cap(s, x, c, "error_rate")["max"] == 0 for x in ("kvstore", "valkey") for c in caps))


def c16(s):
    mine = [mean(s, "kvstore", cpus, "small", "server_rss_mib") for cpus in (1, 4)]
    others = [mean(s, x, cpus, "small", "server_rss_mib") for x in ("valkey", "dragonfly", "memcached") for cpus in (1, 4)]
    return max(mine) < 20 and max(mine) < min(others)


def c17(s):
    def ops(vs, th):
        return s["engine"]["scale"]["%d|%d" % (vs, th)]["ops_per_sec"]["mean"]
    return (4 < ops(64, 8) / ops(64, 1) < 6 and ops(4096, 8) / ops(4096, 1) < 4 and ops(4096, 16) / ops(4096, 8) < 1.15)


def c18(s):
    return s["engine"]["micro"]["match16"]["speedup"]["mean"] >= 10


def c19(s):
    def rtt(label, size):
        return s["transport"]["loopback|%s|%d" % (label, size)]["rtt_p50_us"]["mean"]
    small = all(rtt("rdma-poll", n) < rtt("tcp-poll", n) for n in (64, 256, 1024))
    large = all(rtt("tcp-poll", n) < min(rtt("rdma-poll", n), rtt("rdma-event", n)) for n in (16384, 65536, 262144, 1048576))
    return small and large


def c20(s):
    def stream(label, size):
        return s["transport"]["loopback|%s|%d" % (label, size)]["stream_mb_s"]["mean"]
    sizes = sorted({int(k.split("|")[2]) for k in s["transport"]})
    return len(sizes) == 8 and all(stream("tcp-poll", n) >= 4 * max(stream("rdma-poll", n), stream("rdma-event", n)) for n in sizes)


def c21(s):
    v = s["validation"]

    def ratio(server, tool, conns):
        return v["%s|%s|%d" % (server, tool, conns)]["throughput"]["mean"] / v["%s|memtier|%d" % (server, conns)]["throughput"]["mean"]
    servers = ("valkey", "kvstore-mc")
    return all(abs(ratio(x, "kvpace", 16) - 1) < 0.03 and ratio(x, "kvpace", 1) >= 1.25 for x in servers)


def c22(s):
    return all(cell(s, x, cpus, wl, "error_rate")["max"] == 0 for x in ALL_SYSTEMS for cpus in (1, 4) for wl in WORKLOADS)


def c23(s):
    v = s["validation"]
    r = [v["%s|kvpace-threads|16" % x]["throughput"]["mean"] / v["%s|memtier|16" % x]["throughput"]["mean"]
         for x in ("valkey", "kvstore-mc")]
    return all(0.85 <= x <= 0.95 for x in r)


def c24(s):
    native = loop(s, "pipeline", "kvstore", 32)
    others = [loop(s, "pipeline", x, 32) for x in ("kvstore-mc", "valkey", "dragonfly", "memcached")]
    return (all(above(native, o) for o in others)
            and all(native["mean"] >= 2 * loop(s, "pipeline", x, 32)["mean"] for x in ("valkey", "dragonfly", "memcached")))


def c25(s):
    def gain(x):
        return loop(s, "pipeline", x, 32)["mean"] / loop(s, "pipeline", x, 1)["mean"]
    return (gain("kvstore") > 15 and all(9 < gain(x) < 13 for x in ("kvstore-mc", "valkey"))
            and all(gain(x) < 6.5 for x in ("dragonfly", "memcached")))


def c26(s):
    def keep(x):
        return loop(s, "connections", x, 1024)["mean"] / loop(s, "connections", x, 16)["mean"]
    return all(keep(x) > 0.95 for x in ("kvstore", "kvstore-mc", "memcached", "valkey")) and keep("dragonfly") < 0.85


def c27(s):
    return min(TCP_SYSTEMS, key=lambda x: loop(s, "connections", x, 1024, "p99_us")["mean"]) == "kvstore-mc"


def c28(s):
    def share(x):
        return loop(s, "pipeline", x, 1, "server_system_share")
    return share("kvstore")["min"] > 0.80 and all(share(x)["mean"] > 0.70 for x in TCP_SYSTEMS)


def c29(s):
    return all(abs(mean(s, "kvstore", cpus, wl, "server_cpu_us_per_op") / mean(s, "kvstore-mc", cpus, wl, "server_cpu_us_per_op") - 1)
               < 0.05 for cpus in (1, 4) for wl in WORKLOADS)


def c30(s):
    return all(above(loop(s, "pipeline", x, 1, "server_system_share"), loop(s, "pipeline", x, 32, "server_system_share"))
               for x in TCP_SYSTEMS)


def c31(s):
    def lead(cpus):
        best = max(mean(s, x, cpus, "kvcache") for x in ("kvstore", "kvstore-mc"))
        return best / mean(s, "memcached", cpus, "kvcache")
    return all(1 < lead(cpus) < 1.15 for cpus in (1, 4))


def c32(s):
    return all(mean(s, "kvstore", cpus, wl, "server_cpu_us_per_op") > mean(s, "kvstore-mc", cpus, wl, "server_cpu_us_per_op")
               for cpus in (1, 4) for wl in WORKLOADS)


def c33(s):
    return all(mean(s, "kvstore", cpus, "small", "server_rss_mib") > 8 * mean(s, x, cpus, "small", "server_rss_mib")
               for cpus in (1, 4) for x in ("valkey", "dragonfly", "memcached"))


def c34(s):
    t = s["transport"]
    return all(t["loopback|tcp-poll|%d" % n]["rtt_p50_us"]["mean"] > t["loopback|rdma-poll|%d" % n]["rtt_p50_us"]["mean"]
               for n in (64, 256, 1024))


def c35(s):
    v = s["validation"]
    servers = ("valkey", "kvstore-mc")
    return all(7 <= v["%s|kvpace|1" % x]["p50_us"]["mean"] <= 9 and v["%s|memtier|1" % x]["p50_us"]["mean"] == 15
               for x in servers)


def c36(s):
    def falls(x):
        p = [sweep(s, x, r, "p99_us")["mean"] for r in (100000, 140000, 180000, 220000)]
        return any(b < a for a, b in zip(p, p[1:]))
    return all(falls(x) for x in SWEEP_SYSTEMS)


def c37(s):
    return all(cap(s, "dragonfly", c, "error_rate")["min"] > 0.8 for c in ("256mb", "1gb"))


def c38(s):
    return max(loop(s, "pipeline", x, 32)["min"] for x in TCP_SYSTEMS) > 3_000_000


_TEXT = {
    1: "arenakv over TCP, with its native protocol or with the memcached protocol, has the highest mean throughput in all six matrix rows",
    2: "at four server CPUs the throughput of arenakv (TCP, native protocol) is higher than that of Valkey and of Dragonfly in all three workloads, with non-overlapping ranges",
    3: "at one server CPU arenakv over the memcached protocol has higher throughput than memcached in all three workloads, with non-overlapping ranges",
    4: "one of the two arenakv TCP rows (native protocol or memcached protocol) has the lowest mean server CPU time per operation of the five TCP servers in five of the six matrix rows; in the KV-cache row at four server CPUs memcached is lowest and native arenakv is within 2% of it",
    5: "at four server CPUs arenakv (TCP) uses less server CPU time per operation than Valkey and Dragonfly in all three workloads (means)",
    6: "arenakv over Soft-RoCE has lower mean throughput than arenakv over TCP in all six matrix rows",
    7: "at 260,000 offered operations per second arenakv (TCP) achieves the offered rate within 1% in every repetition, while Valkey, Dragonfly and memcached achieve less than 240,000 operations per second in every repetition",
    8: "up to 140,000 offered operations per second all four servers of the sweep achieve the offered rate within 1%",
    9: "at 260,000 offered operations per second the p99 latency from the intended send time exceeds the p99 service time by more than a factor of 1,000 for Valkey, Dragonfly and memcached",
    10: "below saturation (up to 140,000 offered operations per second) the p99 latency has a coefficient of variation above 20% between repetitions in at least half of the 16 cells (four servers, four rates)",
    11: "at 16, 64 and 256 MB configured capacity the reuse rate of arenakv (TCP) is higher than that of Valkey, with non-overlapping ranges",
    12: "memcached has a higher mean reuse rate than arenakv (TCP) at every configured capacity from 16 MB to 1 GB, and the difference is below 0.03 at 256 MB and 1 GB",
    13: "at 4 GB configured capacity the mean reuse rates of all systems that ran differ by less than 0.005",
    14: "with default flags Dragonfly in cache mode failed more than 90% of requests at 256 MB and more than 80% at 1 GB; with --max_eviction_per_heartbeat raised no request failed",
    15: "memcached failed fewer than 0.1% of requests in every capacity configuration; arenakv and Valkey failed none",
    16: "with 64-byte values arenakv has a lower mean resident memory than each of Valkey, Dragonfly and memcached, below 20 MiB, at one and at four server CPUs",
    17: "engine throughput with 64-byte values grows by a factor between 4 and 6 from one to eight threads, with 4 KiB values by a factor below 4, and 4 KiB throughput grows by less than 15% from 8 to 16 threads",
    18: "the vector tag comparison is at least 10 times faster than the scalar reference",
    19: "for messages up to 1 KiB the median round trip of Soft-RoCE with polling is lower than that of TCP; for messages of 16 KiB and larger TCP is lower than both Soft-RoCE modes",
    20: "TCP loopback streaming throughput is at least four times that of both Soft-RoCE modes at every measured message size",
    21: "with the client model of memtier_benchmark kvpace measures a throughput within 3% of memtier_benchmark with 16 connections, and at least 25% higher with one connection, on both validation servers",
    22: "no operation failed in any matrix run",
    23: "with one blocking thread per connection kvpace measures between 5% and 15% less throughput than memtier_benchmark with 16 connections on both validation servers",
    24: "at pipeline depth 32 native arenakv has the highest throughput of the five servers, with non-overlapping ranges, and at least twice the mean throughput of Valkey, Dragonfly and memcached",
    25: "from pipeline depth 1 to 32 the throughput of native arenakv grows by a factor above 15, that of arenakv over the memcached protocol and of Valkey by a factor between 9 and 13, and that of Dragonfly and memcached by less than 6.5",
    26: "from 16 to 1024 connections the throughput of native arenakv, arenakv over the memcached protocol, memcached and Valkey falls by less than 5%, that of Dragonfly by more than 15%",
    27: "at 1024 connections arenakv over the memcached protocol has the lowest mean GET p99 latency of the five servers",
    28: "at pipeline depth 1 kernel mode accounts for more than 80% of the server CPU time of native arenakv in every repetition, and for more than 70% of the server CPU time of each of the five TCP servers (means)",
    29: "in every matrix row the mean server CPU time per operation of native arenakv and of arenakv over the memcached protocol differ by less than 5%",
    30: "for each of the five TCP servers the kernel-mode share of the server CPU time is lower at pipeline depth 32 than at depth 1, with non-overlapping ranges",
    31: "in the KV-cache rows the higher of the two arenakv TCP means exceeds the mean throughput of memcached by less than 15%, at one and at four server CPUs",
    32: "in run 2026-10-21-run2 the mean server CPU time per operation of native arenakv is higher than that of arenakv over the memcached protocol in every matrix row",
    33: "in run 2026-10-20-run1 the mean resident memory of arenakv with 64-byte values is more than eight times that of each of Valkey, Dragonfly and memcached, at one and at four server CPUs",
    34: "in run 2026-10-20-run1 the median loopback round trip of TCP is higher than that of Soft-RoCE with polling for messages of 64, 256 and 1024 bytes",
    35: "with one connection kvpace measures a mean p50 latency between 7 and 9 microseconds and memtier_benchmark reports 15 microseconds, on both validation servers",
    36: "for each of the four servers of the sweep the mean p99 latency decreases at least once between consecutive offered rates from 100,000 to 220,000 operations per second",
    37: "in run 2026-10-20-run1 Dragonfly with default flags failed more than 80% of requests in every repetition at 256 MB and at 1 GB",
    38: "with 32 requests in flight per connection the four client CPUs drive more than 3 million operations per second in every repetition of at least one server of the pipeline suite",
}
# Claims about an earlier run: claim number -> run directory name under benchmarks/data.
_RUN = {32: "2026-10-21-run2", 33: "2026-10-20-run1", 34: "2026-10-20-run1", 37: "2026-10-20-run1"}
_CHECK = {n: globals()["c%d" % n] for n in _TEXT}
CLAIMS = [{"id": "C%d" % n, "text": _TEXT[n], "check": _CHECK[n], "run": _RUN.get(n)} for n in sorted(_TEXT)]
