#!/usr/bin/env python3
"""Verifies that published numbers, tables, figures and claims agree with the raw benchmark data.

    verify.py RUN_DIR

Checks (every failure is listed; the exit status is non-zero when any check fails):
  1. the manifest has no failed runs, every configuration has the same number of repetitions (at least 3),
     and every raw file listed in the manifest exists;
  2. the SHA-256 of every raw input file equals the one recorded in generated/summary.json;
  3. recomputing the summary from the raw files reproduces generated/summary.json exactly;
  4. every figure embeds the digest of that summary;
  5. every generated table equals the table recomputed from the data;
  6. every GENERATED block in the documentation equals the generated table of the same name;
  7. every claim cited in the documentation as [C<number>] exists in claims.py and holds for the data, and
     every claim in claims.py is cited at least once.
Environment problems (run made from a modified source tree, power-saving CPU governor) are reported as
warnings, not failures: they limit what the numbers mean and are stated in the documentation.
"""
import json
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import bench_data  # noqa: E402
import claims  # noqa: E402
import report  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[2]
GEN = ROOT / "benchmarks" / "generated"
FIG = ROOT / "benchmarks" / "figures"


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    run = pathlib.Path(sys.argv[1])
    problems, warnings = [], []

    manifest = json.loads((run / "manifest.json").read_text())
    failed = [m["file"] or str(m["params"]) for m in manifest if m["status"] == "failed"]
    if failed:
        problems.append("failed runs in the manifest: %s" % ", ".join(failed[:5]))
    for m in manifest:
        if m.get("file") and m["status"] in ("ok", "errors") and not (run / m["file"]).exists():
            problems.append("missing raw file %s" % m["file"])

    summary = bench_data.summarize(run)
    digest = bench_data.summary_digest(summary)

    # repetition counts per configuration and suite
    counts = {}
    for section in ("matrix", "sweep", "pipeline", "connections", "capacity"):
        for key, row in summary[section].items():
            counts.setdefault(section, set()).add(row["throughput"]["n"])
    for section, ns in counts.items():
        if len(ns) > 1:
            problems.append("%s: configurations have different repetition counts %s" % (section, sorted(ns)))
        if min(ns) < 3:
            problems.append("%s: fewer than 3 repetitions in some configuration" % section)

    committed = GEN / "summary.json"
    if not committed.exists():
        problems.append("generated/summary.json is missing; run report.py")
    else:
        old = json.loads(committed.read_text())
        for name, h in old.get("inputs", {}).items():
            path = run / name
            if not path.exists() or bench_data.sha256_file(path) != h:
                problems.append("input file changed or missing since the report was generated: %s" % name)
        if bench_data.canonical(summary) != committed.read_text():
            problems.append("summary recomputed from raw data differs from generated/summary.json")

    for svg in sorted(FIG.glob("*.svg")):
        if "data-sha256=%s" % digest not in svg.read_text():
            problems.append("figure %s was not generated from this data (digest mismatch)" % svg.name)
    if not list(FIG.glob("*.svg")):
        problems.append("no figures found")

    blocks = report.tables(summary, digest)
    for name, text in blocks.items():
        f = GEN / (name + ".md")
        if not f.exists() or f.read_text() != text:
            problems.append("generated table %s is missing or stale" % name)

    pat = re.compile(r"<!-- BEGIN GENERATED:(?P<name>[\w-]+) -->\n(?P<body>.*?)<!-- END GENERATED:(?P=name) -->", re.S)
    cited = set()
    for md in sorted(ROOT.rglob("*.md")):
        if "/build/" in str(md) or "/data/" in str(md):
            continue
        text = md.read_text()
        for m in pat.finditer(text):
            if m.group("name") not in blocks:
                problems.append("%s: unknown generated block %s" % (md.relative_to(ROOT), m.group("name")))
            elif m.group("body") != blocks[m.group("name")]:
                problems.append("%s: block %s differs from the data" % (md.relative_to(ROOT), m.group("name")))
        cited |= {f"C{n}" for n in re.findall(r"\[C(\d+)\]", text)}

    known = {c["id"]: c for c in claims.CLAIMS}
    for cid in sorted(cited - set(known)):
        problems.append("documentation cites %s which is not defined in claims.py" % cid)
    for cid, c in known.items():
        if cid not in cited:
            problems.append("claim %s is defined but not cited in any document" % cid)
            continue
        try:
            ok = bool(c["check"](summary))
        except Exception as e:  # noqa: BLE001
            ok = False
            problems.append("claim %s could not be evaluated: %r" % (cid, e))
            continue
        if not ok:
            problems.append("claim %s does not hold for the data: %s" % (cid, c["text"]))

    env = summary["env"]
    if env.get("git_uncommitted_files", 0):
        warnings.append("the run was made from a source tree with %d uncommitted files" % env["git_uncommitted_files"])
    if env.get("cpu_governor") not in ("performance",):
        warnings.append("CPU governor was %r (not 'performance'); latency tails are affected" % env.get("cpu_governor"))

    for w in warnings:
        print("warning:", w)
    for p in problems:
        print("FAIL:", p)
    print("verify: %d problem(s), %d warning(s); summary digest %s" % (len(problems), len(warnings), digest[:16]))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
