"""Machine-checkable statements made in the documentation.

Each claim has an identifier, the statement as written in the documents, and a function that evaluates it
on the summary produced by bench_data.summarize(). Documents cite a claim as [C<number>]. verify.py fails
when a cited claim does not hold, when a document cites an unknown claim, or when a claim is not cited, so
prose and data cannot drift apart silently. Claims are written after looking at the data and are phrased so
that the condition below is exactly what the sentence says. "Mean" refers to the mean over repetitions;
"ranges do not overlap" means the smallest repetition of one system is larger than the largest repetition of
the other.
"""

TCP_SYSTEMS = ["kvstore", "valkey", "dragonfly", "memcached"]
ALL_SYSTEMS = ["kvstore", "kvstore-rdma", "valkey", "dragonfly", "memcached"]
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


def c1(s):
    for cpus in (1, 4):
        for wl in WORKLOADS:
            best = max(ALL_SYSTEMS, key=lambda x: mean(s, x, cpus, wl))
            expect = "kvstore" if (cpus, wl) == (4, "kvcache") else "memcached"
            if best != expect:
                return False
    return True


def c2(s):
    return all(above(cell(s, "kvstore", 4, wl, "throughput"), cell(s, other, 4, wl, "throughput"))
               for wl in WORKLOADS for other in ("valkey", "dragonfly"))


def c3(s):
    return all(above(cell(s, other, 1, "small", "throughput"), cell(s, "kvstore", 1, "small", "throughput"))
               for other in ("valkey", "dragonfly", "memcached"))


def c4(s):
    return all(min(TCP_SYSTEMS, key=lambda x: mean(s, x, cpus, wl, "server_cpu_us_per_op")) == "memcached"
               for cpus in (1, 4) for wl in WORKLOADS)


def c5(s):
    return all(mean(s, "kvstore", 4, wl, "server_cpu_us_per_op") < mean(s, other, 4, wl, "server_cpu_us_per_op")
               for wl in WORKLOADS for other in ("valkey", "dragonfly"))


def c6(s):
    lower = [(cpus, wl) for cpus in (1, 4) for wl in WORKLOADS
             if mean(s, "kvstore-rdma", cpus, wl) < mean(s, "kvstore", cpus, wl)]
    return len(lower) == 5 and (1, "small") not in lower


def c7(s):
    return all(above(sweep(s, other, 260000, "achieved_ops"), sweep(s, "kvstore", 260000, "achieved_ops"))
               for other in ("valkey", "dragonfly", "memcached"))


def c8(s):
    return all(abs(sweep(s, system, rate, "achieved_ops")["mean"] - rate) / rate < 0.01
               for system in TCP_SYSTEMS for rate in (20000, 60000, 100000, 140000))


def c9(s):
    return all(sweep(s, system, 260000, "p99_us")["mean"] > 1000 * sweep(s, system, 260000, "service_p99_us")["mean"]
               for system in TCP_SYSTEMS)


def c10(s):
    cells = [sweep(s, system, rate, "p99_us")["cv"] for system in TCP_SYSTEMS for rate in (20000, 60000, 100000, 140000)]
    return len(cells) == 16 and sum(c > 0.20 for c in cells) >= len(cells) / 2


def c11(s):
    return all(above(cap(s, "kvstore", c, "reuse_rate"), cap(s, "valkey", c, "reuse_rate")) for c in ("16mb", "64mb", "256mb"))


def c12(s):
    higher = all(cap(s, "memcached", c, "reuse_rate")["mean"] > cap(s, "kvstore", c, "reuse_rate")["mean"] for c in ("16mb", "64mb"))
    close = all(abs(cap(s, "memcached", c, "reuse_rate")["mean"] - cap(s, "kvstore", c, "reuse_rate")["mean"]) < 0.02
                for c in ("256mb", "1gb", "4gb"))
    return higher and close


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
    others = [mean(s, x, cpus, "small", "server_rss_mib") for x in ("valkey", "dragonfly", "memcached") for cpus in (1, 4)]
    mine = [mean(s, "kvstore", cpus, "small", "server_rss_mib") for cpus in (1, 4)]
    return max(others) < 40 and min(mine) > 8 * max(others)


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
    big = all(stream("tcp-poll", n) >= 5 * max(stream("rdma-poll", n), stream("rdma-event", n)) for n in (65536, 262144, 1048576))
    small = all(stream("rdma-poll", n) > stream("tcp-poll", n) and stream("rdma-event", n) > stream("tcp-poll", n) for n in (64, 256, 1024))
    return big and small


def c21(s):
    # The raw data identifies kvpace by its development name "kvbench".
    v = s["validation"]
    r1 = v["kvbench|1"]["throughput"]["mean"] / v["memtier|1"]["throughput"]["mean"]
    r16 = v["kvbench|16"]["throughput"]["mean"] / v["memtier|16"]["throughput"]["mean"]
    return r1 >= 1.2 and 0.85 <= r16 <= 0.95


def c22(s):
    return all(cell(s, x, cpus, wl, "error_rate")["max"] == 0 for x in ALL_SYSTEMS for cpus in (1, 4) for wl in WORKLOADS)


_TEXT = {
    1: "memcached has the highest mean throughput in five of the six matrix rows; arenakv (TCP) has the highest in the sixth (KV-cache requests, four server CPUs)",
    2: "at four server CPUs the throughput of arenakv (TCP) is higher than that of Valkey and of Dragonfly in all three workloads, with non-overlapping ranges",
    3: "at one server CPU arenakv (TCP) has lower throughput than Valkey, Dragonfly and memcached on 64-byte values, with non-overlapping ranges",
    4: "memcached has the lowest mean server CPU time per operation of the four TCP servers in all six matrix rows",
    5: "at four server CPUs arenakv (TCP) uses less server CPU time per operation than Valkey and Dragonfly in all three workloads (means)",
    6: "arenakv over Soft-RoCE has lower mean throughput than arenakv over TCP in five of the six matrix rows; the exception is 64-byte values with one server CPU",
    7: "at 260,000 offered operations per second arenakv (TCP) achieves a lower rate than Valkey, Dragonfly and memcached, with non-overlapping ranges",
    8: "up to 140,000 offered operations per second all four TCP servers achieve the offered rate within 1%",
    9: "at 260,000 offered operations per second the p99 latency from the intended send time exceeds the p99 service time by more than a factor of 1,000 for every TCP server",
    10: "below saturation (up to 140,000 offered operations per second) the p99 latency has a coefficient of variation above 20% between repetitions in at least half of the 16 cells (four servers, four rates)",
    11: "at 16, 64 and 256 MB configured capacity the reuse rate of arenakv (TCP) is higher than that of Valkey, with non-overlapping ranges",
    12: "memcached has a higher mean reuse rate than arenakv (TCP) at 16 and 64 MB and differs from it by less than 0.02 at 256 MB, 1 GB and 4 GB",
    13: "at 4 GB configured capacity the mean reuse rates of all systems that ran differ by less than 0.005",
    14: "with default flags Dragonfly in cache mode failed more than 90% of requests at 256 MB and more than 80% at 1 GB; with --max_eviction_per_heartbeat raised no request failed",
    15: "memcached failed fewer than 0.1% of requests in every capacity configuration; arenakv and Valkey failed none",
    16: "with 64-byte values the resident memory of arenakv is more than eight times that of each of Valkey, Dragonfly and memcached, each of which stays below 40 MiB",
    17: "engine throughput with 64-byte values grows by a factor between 4 and 6 from one to eight threads, with 4 KiB values by a factor below 4, and 4 KiB throughput grows by less than 15% from 8 to 16 threads",
    18: "the vector tag comparison is at least 10 times faster than the scalar reference",
    19: "for messages up to 1 KiB the median round trip of Soft-RoCE with polling is lower than that of TCP; for messages of 16 KiB and larger TCP is lower than both Soft-RoCE modes",
    20: "TCP loopback streaming throughput is at least five times that of Soft-RoCE for messages of 64 KiB and larger, and lower than both Soft-RoCE modes for messages up to 1 KiB",
    21: "with one connection kvpace measures at least 20% higher throughput than memtier_benchmark; with 16 connections it measures between 5% and 15% lower",
    22: "no operation failed in any matrix run",
}
_CHECK = {1: c1, 2: c2, 3: c3, 4: c4, 5: c5, 6: c6, 7: c7, 8: c8, 9: c9, 10: c10, 11: c11, 12: c12, 13: c13, 14: c14,
          15: c15, 16: c16, 17: c17, 18: c18, 19: c19, 20: c20, 21: c21, 22: c22}
CLAIMS = [{"id": "C%d" % n, "text": _TEXT[n], "check": _CHECK[n]} for n in sorted(_TEXT)]
