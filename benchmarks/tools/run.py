#!/usr/bin/env python3
"""Benchmark orchestrator.

Runs measurement suites and stores every result as raw JSON together with the environment description
and a manifest of all runs.

    run.py --suite SUITE [--suite SUITE ...] --reps N [--out DIR] [--quick]

Suites:
    matrix     five systems x {1 CPU, 4 CPUs} x {small, block, kvcache} workloads (closed loop)
    sweep      open-loop latency versus offered load, one server CPU, small values
    capacity   KV-cache scenario: reuse rate versus configured cache capacity, one server CPU
    engine     in-process engine benchmarks: thread scaling and micro-benchmarks
    transport  rdmaduct ping-pong latency and streaming throughput (loopback)
    validation kvpace against memtier_benchmark on one Valkey server (checks the load generator)
    capacity-evict  capacity suite for Dragonfly with a raised eviction flag (not part of `all`)
    all        every suite above

Repetitions are interleaved: repetition 1 of every configuration runs before repetition 2 of any
configuration, in a shuffled order, so that slow drift of the machine does not correlate with a
configuration. Each run starts a fresh server. An existing output directory is extended: env.json is kept and
new runs are appended to manifest.json. Output layout:

    DIR/env.json            machine, toolchain, software versions, git state
    DIR/manifest.json       one entry per run (parameters, command, status, wall time)
    DIR/<suite>/*.json      raw results, one file per run
    DIR/<suite>/*.err       standard error of failed runs
"""
import argparse
import json
import os
import pathlib
import platform
import random
import socket
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
ARCH = platform.machine()
KVBENCH = ROOT / "kvpace" / "build" / ARCH / "kvpace"
SERVERS_SH = ROOT / "kvpace" / "scripts" / "servers.sh"
AKV_BENCH = ROOT / "arenakv" / "build" / ARCH / "akv_bench"
DUCT_BENCH = ROOT / "rdmaduct" / "build" / ARCH / "duct_bench"
ENV_SH = ROOT / "benchmarks" / "scripts" / "env-info.sh"

CLIENT_CPUS = "8,10,12,14"
SERVER_CPUS = {"1": "0", "4": "0,2,4,6"}
PORT = 7100

SYSTEMS = {
    "valkey": {"driver": "resp", "host": "127.0.0.1", "opts": [], "tcp": True},
    "dragonfly": {"driver": "resp", "host": "127.0.0.1", "opts": [], "tcp": True},
    "memcached": {"driver": "memcached", "host": "127.0.0.1", "opts": [], "tcp": True},
    # System ids "kvstore" and "kvstore-rdma" are the identifiers of arenakv in the raw data (its name during development).
    "kvstore": {"driver": "native", "host": "127.0.0.1", "opts": ["--driver-opt", "transport=tcp"], "tcp": True},
    "kvstore-rdma": {"driver": "native", "host": None, "opts": ["--driver-opt", "transport=rdma"], "tcp": False},
    # Dragonfly with one eviction flag raised: its default (100 evictions per heartbeat) rejects writes with
    # "Out of memory" under sustained 16 KiB writes at the configured capacities (see METHODOLOGY.md).
    "dragonfly-evict": {"driver": "resp", "host": "127.0.0.1", "opts": [], "tcp": True, "server": "dragonfly",
                        "extra": "--max_eviction_per_heartbeat=10000"},
}

WORKLOADS = {
    "small": ["--keys", "100000", "--value-size", "64", "--read-ratio", "0.9", "--dist", "uniform"],
    "block": ["--keys", "20000", "--value-size", "16384", "--read-ratio", "0.9", "--dist", "uniform"],
    "kvcache": ["--scenario", "kvcache", "--block-size", "16384", "--groups", "16", "--shared-blocks", "32",
                "--turn-blocks", "8", "--active-conv", "256", "--max-turns", "16", "--new-conv-prob", "0.05",
                "--theta", "0.9"],
}

# arenakv sizes its hash index from the expected mean entry size.
KVSTORE_AVG_ENTRY = {"small": "128", "block": "16384", "kvcache": "16384"}


def relative(text):
    """text with every occurrence of the repository root prefix removed."""
    return text.replace(str(ROOT) + "/", "")


class Recorder:
    """Writes raw results and keeps the manifest."""

    def __init__(self, out, quick):
        self.out = pathlib.Path(out)
        self.quick = quick
        self.out.mkdir(parents=True, exist_ok=True)
        mf = self.out / "manifest.json"
        self.manifest = json.loads(mf.read_text()) if mf.exists() else []

    def path(self, suite, name):
        d = self.out / suite
        d.mkdir(exist_ok=True)
        return d / name

    def add(self, entry):
        """Appends a manifest entry. Absolute paths below the repository root in the entry's command and in its
        result file are rewritten relative to the root, so results do not record the checkout location."""
        entry["command"] = [relative(c) for c in entry.get("command", [])]
        f = self.out / entry["file"] if entry.get("file") else None
        if f is not None and f.exists():
            f.write_text(relative(f.read_text()))
        self.manifest.append(entry)
        (self.out / "manifest.json").write_text(json.dumps(self.manifest, indent=1) + "\n")


def rdma_addr():
    addr = os.environ.get("RDMA_ADDR")
    if not addr:
        sys.exit("RDMA_ADDR must name the IPv4 address of the RDMA interface")
    return addr


def wait_port(host, port, timeout=15.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            with socket.create_connection((host, port), timeout=1):
                return True
        except OSError:
            time.sleep(0.2)
    return False


def server_start(name, cpus, capacity=None, extra=None):
    name = SYSTEMS.get(name, {}).get("server", name)
    env = dict(os.environ)
    if capacity:
        env["CAPACITY"] = capacity
    if extra:
        env["EXTRA_ARGS"] = extra
    if name == "kvstore-rdma":
        env["RDMA_ADDR"] = rdma_addr()
    subprocess.run([str(SERVERS_SH), "start", name, str(PORT), cpus], env=env, check=True,
                   stdout=subprocess.DEVNULL)
    if SYSTEMS[name]["tcp"]:
        if not wait_port("127.0.0.1", PORT):
            raise RuntimeError("server %s did not open port %d" % (name, PORT))
    else:
        time.sleep(2.0)
    time.sleep(0.8)
    pid = subprocess.run([str(SERVERS_SH), "pid", name], capture_output=True, text=True, check=True).stdout.strip()
    return pid


def server_stop(name):
    name = SYSTEMS.get(name, {}).get("server", name)
    subprocess.run([str(SERVERS_SH), "stop", name], check=False, stdout=subprocess.DEVNULL)
    time.sleep(0.3)


def run_kvpace(rec, suite, name, params, system, cpus, workload_args, extra_args, duration, warmup,
                capacity=None, threads=16):
    """Starts a server, runs kvpace once, stops the server, records the outcome."""
    out = rec.path(suite, name + ".json")
    sysdef = SYSTEMS[system]
    host = sysdef["host"] or rdma_addr()
    extra = sysdef.get("extra")
    if system.startswith("kvstore"):
        extra = "--avg-entry " + KVSTORE_AVG_ENTRY.get(params.get("workload", "small"), "128")
    started = time.time()
    status, err = "ok", ""
    cmd = []
    try:
        pid = server_start(system, cpus, capacity, extra)
        cmd = [str(KVBENCH), "--driver", sysdef["driver"], *sysdef["opts"], "--host", host, "--port", str(PORT),
               "--threads", str(threads), "--warmup", str(warmup), "--duration", str(duration),
               *workload_args, *extra_args, "--pin", CLIENT_CPUS, "--server-pid", pid,
               "--label", "%s %s" % (system, name), "--out", str(out)]
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=duration + warmup + 120)
        if p.returncode == 3 and out.exists():
            # kvpace exits with 3 when some operations failed; the result file is complete and records the count.
            status, err = "errors", p.stdout
        elif p.returncode != 0:
            status, err = "failed", p.stderr + p.stdout
    except Exception as e:  # noqa: BLE001 - any failure is recorded and the suite continues
        status, err = "failed", repr(e)
    finally:
        server_stop(system)
    if err:
        rec.path(suite, name + ".err").write_text(err)
    rec.add({"suite": suite, "file": "%s/%s.json" % (suite, name), "status": status, "params": params,
             "command": cmd, "wall_s": round(time.time() - started, 1),
             "server": {"system": system, "cpus": cpus, "capacity": capacity, "extra_args": extra}})
    print("[%s] %s %s" % (suite, name, status), flush=True)


DRAGONFLY_MIN_MB = 256


def cap_mb(cap):
    """Converts a size such as 256mb or 4gb to megabytes."""
    return int(cap[:-2]) * (1024 if cap.endswith("gb") else 1)


def interleave(configs, reps):
    """Yields (rep, config) with all configurations of one repetition before the next repetition."""
    for rep in range(1, reps + 1):
        order = list(configs)
        random.Random(rep).shuffle(order)
        for cfg in order:
            yield rep, cfg


def suite_matrix(rec, reps, quick):
    dur, warm = (3, 1) if quick else (8, 3)
    configs = [(s, n, w) for s in SYSTEMS for n in SERVER_CPUS for w in WORKLOADS]
    for rep, (system, n, wl) in interleave(configs, reps):
        run_kvpace(rec, "matrix", "%s-%scpu-%s-r%d" % (system, n, wl, rep),
                    {"system": system, "cpus": int(n), "workload": wl, "rep": rep}, system, SERVER_CPUS[n],
                    WORKLOADS[wl], [], dur, warm)


def suite_sweep(rec, reps, quick):
    dur, warm = (3, 1) if quick else (6, 2)
    rates = [20000, 60000, 100000, 140000, 180000, 220000, 260000]
    if quick:
        rates = [20000, 180000]
    systems = ["valkey", "dragonfly", "memcached", "kvstore"]
    configs = [(s, r) for s in systems for r in rates]
    for rep, (system, rate) in interleave(configs, reps):
        run_kvpace(rec, "sweep", "%s-%d-r%d" % (system, rate, rep),
                    {"system": system, "offered": rate, "workload": "small", "cpus": 1, "rep": rep}, system,
                    SERVER_CPUS["1"], WORKLOADS["small"], ["--rate", str(rate), "--arrival", "constant"], dur, warm)


def suite_capacity(rec, reps, quick):
    dur, warm = (4, 1) if quick else (10, 3)
    caps = ["16mb", "64mb", "256mb", "1gb", "4gb"]
    if quick:
        caps = ["64mb"]
    systems = ["valkey", "dragonfly", "memcached", "kvstore"]
    configs = [(s, c) for s in systems for c in caps]
    for rep, (system, cap) in interleave(configs, reps):
        if system == "dragonfly" and cap_mb(cap) < DRAGONFLY_MIN_MB:
            rec.add({"suite": "capacity", "file": None, "status": "skipped",
                     "params": {"system": system, "capacity": cap, "rep": rep},
                     "reason": "dragonfly refuses to start with less than %d MiB per thread" % DRAGONFLY_MIN_MB})
            continue
        run_kvpace(rec, "capacity", "%s-%s-r%d" % (system, cap, rep),
                    {"system": system, "capacity": cap, "workload": "kvcache", "cpus": 1, "rep": rep}, system,
                    SERVER_CPUS["1"], WORKLOADS["kvcache"], [], dur, warm, capacity=cap, threads=8)


def suite_capacity_evict(rec, reps, quick):
    """Capacity runs of Dragonfly with the raised eviction flag (the default-flag runs are in suite capacity)."""
    dur, warm = (4, 1) if quick else (10, 3)
    caps = ["256mb", "1gb", "4gb"]
    configs = [("dragonfly-evict", c) for c in caps]
    for rep, (system, cap) in interleave(configs, reps):
        run_kvpace(rec, "capacity", "%s-%s-r%d" % (system, cap, rep),
                    {"system": system, "capacity": cap, "workload": "kvcache", "cpus": 1, "rep": rep}, system,
                    SERVER_CPUS["1"], WORKLOADS["kvcache"], [], dur, warm, capacity=cap, threads=8)


def run_tool(rec, suite, name, params, cmd, timeout):
    out = rec.path(suite, name + ".json")
    started = time.time()
    status, err = "ok", ""
    try:
        p = subprocess.run([*cmd, "--out", str(out)], capture_output=True, text=True, timeout=timeout)
        if p.returncode != 0:
            status, err = "failed", p.stderr + p.stdout
    except Exception as e:  # noqa: BLE001
        status, err = "failed", repr(e)
    if err:
        rec.path(suite, name + ".err").write_text(err)
    rec.add({"suite": suite, "file": "%s/%s.json" % (suite, name), "status": status, "params": params,
             "command": cmd, "wall_s": round(time.time() - started, 1)})
    print("[%s] %s %s" % (suite, name, status), flush=True)


def suite_engine(rec, reps, quick):
    # Distinct physical P-cores first (even logical CPUs), then their hyper-thread siblings.
    cpus = "0,2,4,6,8,10,12,14,1,3,5,7,9,11,13,15"
    threads = "1,2" if quick else "1,2,4,8,12,16"
    cases = [("v64", 64, 1000000), ("v4096", 4096, 200000)]
    configs = [("scale", c) for c in cases] + [("micro", None)]
    for rep, (kind, case) in interleave(configs, reps):
        if kind == "micro":
            run_tool(rec, "engine", "micro-r%d" % rep, {"kind": "micro", "rep": rep},
                     [str(AKV_BENCH), "micro"], 600)
        else:
            label, vs, keys = case
            run_tool(rec, "engine", "scale-%s-r%d" % (label, rep),
                     {"kind": "scale", "value_size": vs, "keys": keys, "rep": rep},
                     [str(AKV_BENCH), "scale", "--threads", threads, "--cpus", cpus, "--value-size", str(vs),
                      "--read-ratio", "0.9", "--keys", str(keys), "--duration", "1" if quick else "3",
                      "--warmup", "0.5" if quick else "1"], 900)


def suite_transport(rec, reps, quick):
    sizes = "64,1024,16384,262144" if quick else "64,256,1024,4096,16384,65536,262144,1048576"
    pp, st = ("500", "1000") if quick else ("5000", "20000")
    addr = rdma_addr()
    modes = [("tcp", "poll", "127.0.0.1"), ("rdma", "poll", addr), ("rdma", "event", addr)]
    configs = [("loopback", m) for m in modes]
    for rep, (_, (tr, wait, host)) in interleave(configs, reps):
        run_tool(rec, "transport", "loopback-%s-%s-r%d" % (tr, wait, rep),
                 {"scope": "loopback", "transport": tr, "wait": wait, "rep": rep},
                 [str(DUCT_BENCH), "loopback", "--transport", tr, "--wait", wait, "--addr", host, "--sizes", sizes,
                  "--pingpong", pp, "--stream", st], 1800)


MEMTIER_IMAGE = "redislabs/memtier_benchmark:latest"


def memtier_command(rec, conns, out_name=None):
    """docker command line for memtier_benchmark with settings equivalent to kvpace's `small` workload."""
    threads, per_thread = (1, 1) if conns == 1 else (4, conns // 4)
    outdir = (rec.out / "validation").resolve()
    outdir.mkdir(exist_ok=True)
    base = ["docker", "run", "--rm", "--network", "host", "--cpuset-cpus", CLIENT_CPUS, "-v", "%s:/out" % outdir,
            MEMTIER_IMAGE, "-s", "127.0.0.1", "-p", str(PORT), "--protocol=redis", "-t", str(threads),
            "-c", str(per_thread), "--data-size=64", "--key-minimum=1", "--key-maximum=100000", "--hide-histogram"]
    preload = base + ["--key-pattern=P:P", "--ratio=1:0", "-n", "allkeys"]
    measure = base + ["--key-pattern=R:R", "--ratio=1:9", "--test-time=8", "--print-percentiles=50,99,99.9",
                      "--json-out-file=/out/%s.json" % out_name]
    return preload, measure


def run_memtier(rec, name, params, conns):
    started = time.time()
    status, err = "ok", ""
    preload, measure = memtier_command(rec, conns, name)
    try:
        server_start("valkey", SERVER_CPUS["1"])
        subprocess.run(preload, capture_output=True, text=True, timeout=300, check=True)
        p = subprocess.run(measure, capture_output=True, text=True, timeout=120)
        if p.returncode != 0:
            status, err = "failed", p.stderr + p.stdout
    except Exception as e:  # noqa: BLE001
        status, err = "failed", repr(e)
    finally:
        server_stop("valkey")
    if err:
        rec.path("validation", name + ".err").write_text(err)
    rec.add({"suite": "validation", "file": "validation/%s.json" % name, "status": status, "params": params,
             "command": measure, "wall_s": round(time.time() - started, 1)})
    print("[validation] %s %s" % (name, status), flush=True)


def suite_validation(rec, reps, quick):
    """kvpace against memtier_benchmark on the same server settings; order alternates between repetitions."""
    dur, warm = (3, 1) if quick else (8, 3)
    for rep in range(1, reps + 1):
        for conns in (1, 16):
            order = ["kvpace", "memtier"] if rep % 2 else ["memtier", "kvpace"]
            for tool in order:
                name = "%s-%dc-r%d" % (tool, conns, rep)
                params = {"tool": tool, "conns": conns, "rep": rep}
                if tool == "kvpace":
                    run_kvpace(rec, "validation", name, params, "valkey", SERVER_CPUS["1"], WORKLOADS["small"], [],
                                dur, warm, threads=conns)
                else:
                    run_memtier(rec, name, params, conns)


SUITES = {"matrix": suite_matrix, "sweep": suite_sweep, "capacity": suite_capacity, "engine": suite_engine,
          "transport": suite_transport, "validation": suite_validation,
          "capacity-evict": suite_capacity_evict}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--suite", action="append", required=True, choices=list(SUITES) + ["all"])
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--out", required=True)
    ap.add_argument("--quick", action="store_true", help="short runs and one repetition: checks the pipeline only")
    args = ap.parse_args()
    suites = [x for x in SUITES if x != "capacity-evict"] if "all" in args.suite else args.suite
    for tool in (KVBENCH, SERVERS_SH):
        if not tool.exists():
            sys.exit("missing %s (build the projects first: make -C %s)" % (tool, ROOT))
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    env = subprocess.run([str(ENV_SH), str(ROOT)], capture_output=True, text=True, check=True,
                         env={**os.environ, "BENCH_NIC": os.environ.get("BENCH_NIC", "")}).stdout
    if not (out / "env.json").exists():
        (out / "env.json").write_text(env)
    rec = Recorder(out, args.quick)
    reps = 1 if args.quick else args.reps
    subprocess.run([str(SERVERS_SH), "stop-all"], check=False, stdout=subprocess.DEVNULL)
    for s in suites:
        print("== suite %s (%d repetitions)" % (s, reps), flush=True)
        SUITES[s](rec, reps, args.quick)
    failed = [m for m in rec.manifest if m["status"] == "failed"]
    print("done: %d runs, %d failed, %d with operation errors, %d skipped" % (
        len(rec.manifest), len(failed), sum(m["status"] == "errors" for m in rec.manifest),
        sum(m["status"] == "skipped" for m in rec.manifest)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
