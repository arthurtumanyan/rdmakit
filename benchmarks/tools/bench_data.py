"""Loading and aggregation of raw benchmark results.

A run directory (see run.py) contains env.json, manifest.json and one raw JSON file per measurement.
`summarize()` turns it into a single deterministic dictionary: for every configuration the statistics of
each metric over the repetitions, plus the SHA-256 of every input file. The figure generator, the tables
and the verifier all read this dictionary, so a number has exactly one definition.

Statistics over repetitions: n, mean, sample standard deviation (n - 1), minimum, maximum and the
coefficient of variation (std / mean). Nothing is smoothed or filtered; failed runs are excluded from the
statistics and listed in the manifest summary.
"""
import hashlib
import json
import math
import pathlib


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def stats(values):
    """Descriptive statistics of a list of numbers."""
    n = len(values)
    if n == 0:
        return None
    mean = sum(values) / n
    std = math.sqrt(sum((v - mean) ** 2 for v in values) / (n - 1)) if n > 1 else 0.0
    return {"n": n, "mean": mean, "std": std, "min": min(values), "max": max(values),
            "cv": (std / mean) if mean else 0.0}


def _load(path):
    with open(path) as f:
        return json.load(f)


def _group(entries, keyfn):
    groups = {}
    for e in entries:
        groups.setdefault(keyfn(e), []).append(e)
    return groups


def _metrics_kv(result):
    """Metrics of one kvpace result file. KV-cache runs count requests, other runs count operations."""
    r = result["results"]
    seconds = r["seconds"]
    kc = r.get("kvcache")
    server = r.get("server", {})
    if kc:
        lat = kc["request_latency_ns"]
        throughput = kc["requests"] / seconds
    else:
        lat = r["get_latency_ns"]
        throughput = r["ops_per_sec"]
    m = {
        "throughput": throughput,
        "p50_us": lat["p50"] / 1e3,
        "p99_us": lat["p99"] / 1e3,
        "p999_us": lat["p999"] / 1e3,
        "errors": r["errors"],
        # KV-cache runs: server CPU per block GET or PUT, computed from the raw counters. kvpace up to commit
        # 46efd64 (the build of run2) wrote 0 for cpu_us_per_op in this scenario.
        "server_cpu_us_per_op": (server["cpu_seconds"] * 1e6 / (r["gets"] + r["puts"])
                                 if kc and "cpu_seconds" in server and r["gets"] + r["puts"]
                                 else server.get("cpu_us_per_op")),
        "server_rss_mib": server["rss_bytes"] / 1048576 if "rss_bytes" in server else None,
        # Kernel-mode share of the server CPU time (system calls and the network stack run on the server's
        # behalf); recorded by kvpace since run3.
        "server_system_share": (server["system_cpu_seconds"] / server["cpu_seconds"]
                                if server.get("system_cpu_seconds") is not None and server.get("cpu_seconds")
                                else None),
        "service_p99_us": r["service_time_ns"]["p99"] / 1e3,
        "max_lag_ms": r["max_lag_ns"] / 1e6,
        "achieved_ops": r["ops_per_sec"],
    }
    # Split of the server CPU time per operation into its kernel-mode and user-mode parts, per run.
    share, per_op = m["server_system_share"], m["server_cpu_us_per_op"]
    if share is not None and per_op is not None:
        m["server_system_us_per_op"] = per_op * share
        m["server_user_us_per_op"] = per_op * (1 - share)
    # Fraction of work units (requests for the KV-cache scenario, operations otherwise) that failed.
    units = kc["requests"] if kc else r["ops"]
    m["error_rate"] = r["errors"] / (units + r["errors"]) if (units + r["errors"]) else 0.0
    if kc:
        m["reuse_rate"] = kc["reuse_rate"]
        m["read_mb_s"] = kc["bytes_get"] / seconds / 1e6
    return m


def _agg(metric_dicts):
    """Statistics of every metric present in all dictionaries."""
    out = {}
    keys = set()
    for d in metric_dicts:
        keys.update(d)
    for k in sorted(keys):
        vals = [d[k] for d in metric_dicts if d.get(k) is not None]
        if vals:
            out[k] = stats(vals)
    return out


def summarize(run_dir):
    """Returns the deterministic summary of a run directory."""
    run_dir = pathlib.Path(run_dir)
    manifest = _load(run_dir / "manifest.json")
    env = _load(run_dir / "env.json")
    summary = {
        "run": run_dir.name,
        "env": env,
        "manifest": {
            "runs": len(manifest),
            "ok": sum(m["status"] == "ok" for m in manifest),
            "with_errors": sum(m["status"] == "errors" for m in manifest),
            "failed": sum(m["status"] == "failed" for m in manifest),
            "skipped": [m["params"] | {"reason": m.get("reason", "")} for m in manifest if m["status"] == "skipped"],
        },
        "inputs": {},
        "matrix": {}, "sweep": {}, "pipeline": {}, "connections": {}, "capacity": {},
        "engine": {"scale": {}, "micro": {}}, "transport": {}, "validation": {},
    }
    usable = [m for m in manifest if m["status"] in ("ok", "errors") and m.get("file")]
    for m in usable:
        summary["inputs"][m["file"]] = sha256_file(run_dir / m["file"])
    for name in ("env.json", "manifest.json"):
        summary["inputs"][name] = sha256_file(run_dir / name)

    def entries(suite):
        return [(m["params"], _load(run_dir / m["file"])) for m in usable if m["suite"] == suite]

    for key, grp in _group(entries("matrix"), lambda e: (e[0]["system"], e[0]["cpus"], e[0]["workload"])).items():
        summary["matrix"]["%s|%d|%s" % key] = _agg([_metrics_kv(r) for _, r in grp])
    for key, grp in _group(entries("sweep"), lambda e: (e[0]["system"], e[0]["offered"])).items():
        summary["sweep"]["%s|%d" % key] = _agg([_metrics_kv(r) for _, r in grp])
    for key, grp in _group(entries("pipeline"), lambda e: (e[0]["system"], e[0]["depth"])).items():
        summary["pipeline"]["%s|%d" % key] = _agg([_metrics_kv(r) for _, r in grp])
    for key, grp in _group(entries("connections"), lambda e: (e[0]["system"], e[0]["conns"])).items():
        summary["connections"]["%s|%d" % key] = _agg([_metrics_kv(r) for _, r in grp])
    for key, grp in _group(entries("capacity"), lambda e: (e[0]["system"], e[0]["capacity"])).items():
        summary["capacity"]["%s|%s" % key] = _agg([_metrics_kv(r) for _, r in grp])

    # Validation keys are server|tool|connections. Runs without a server parameter used Valkey; tool "kvbench"
    # (the development name of kvpace) used one blocking thread per connection, the model now named
    # "kvpace-threads".
    val = {}
    for params, r in entries("validation"):
        tool = "kvpace-threads" if params["tool"] == "kvbench" else params["tool"]
        if tool != "memtier":
            m = _metrics_kv(r)
            row = {"throughput": m["throughput"], "p50_us": m["p50_us"], "p99_us": m["p99_us"]}
        else:
            gets = r["ALL STATS"]["Gets"]
            row = {"throughput": r["ALL STATS"]["Totals"]["Ops/sec"],
                   "p50_us": gets["Percentile Latencies"]["p50.00"] * 1e3,
                   "p99_us": gets["Percentile Latencies"]["p99.00"] * 1e3}
        val.setdefault((params.get("server", "valkey"), tool, params["conns"]), []).append(row)
    for (server, tool, conns), rows in sorted(val.items()):
        summary["validation"]["%s|%s|%d" % (server, tool, conns)] = _agg(rows)

    scale = {}
    micro_match, micro_hash = [], {}
    for params, r in entries("engine"):
        if r["mode"] == "scale":
            for row in r["results"]:
                scale.setdefault((r["value_size"], row["threads"]), []).append(row["ops_per_sec"])
        else:
            micro_match.append(r["match16"])
            for row in r["hash"]:
                micro_hash.setdefault(row["key_len"], []).append(row)
    for (vs, th), vals in sorted(scale.items()):
        summary["engine"]["scale"]["%d|%d" % (vs, th)] = {"ops_per_sec": stats(vals)}
    if micro_match:
        summary["engine"]["micro"]["match16"] = _agg(micro_match)
        summary["engine"]["micro"]["simd"] = [r for p, r in entries("engine") if r["mode"] == "micro"][0]["simd"]
        for ln, rows in sorted(micro_hash.items()):
            summary["engine"]["micro"]["hash|%d" % ln] = _agg(rows)

    tr = {}
    for params, r in entries("transport"):
        label = "%s-%s" % (r["transport"], r["wait"])
        for row in r["results"]:
            tr.setdefault((params["scope"], label, row["size"]), []).append({
                "rtt_p50_us": row["rtt_us"]["p50"], "rtt_p99_us": row["rtt_us"]["p99"],
                "rtt_mean_us": row["rtt_us"]["mean"], "pingpong_mb_s": row["pingpong_mb_s"],
                "stream_mb_s": row["stream_mb_s"], "stream_msgs_per_s": row["stream_msgs_per_s"]})
    for (scope, label, size), rows in sorted(tr.items()):
        summary["transport"]["%s|%s|%d" % (scope, label, size)] = _agg(rows)
    return summary


def canonical(summary):
    """Canonical JSON text of a summary (stable key order, fixed float formatting)."""
    return json.dumps(summary, sort_keys=True, indent=1, ensure_ascii=True) + "\n"


def summary_digest(summary):
    return hashlib.sha256(canonical(summary).encode()).hexdigest()
