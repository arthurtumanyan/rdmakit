#!/usr/bin/env python3
"""Generates figures, tables and documentation blocks from a benchmark run directory.

    report.py RUN_DIR [--out-figures DIR] [--out-generated DIR] [--update-docs]

Inputs are read only from RUN_DIR (raw results, env.json, manifest.json) through bench_data.summarize().
Outputs:
    figures/<name>.svg, figures/<name>-dark.svg     charts (light and dark surface)
    generated/summary.json                          aggregated numbers, input hashes, summary digest
    generated/<name>.md                             tables (the table view of every chart)
With --update-docs the generated tables are inserted into every Markdown file of the repository between
`<!-- BEGIN GENERATED:name -->` and `<!-- END GENERATED:name -->` markers.

Chart conventions: one hue per system in fixed order (blue, orange, aqua, yellow; validated for color-vision
separation), the Soft-RoCE variant of the store is the same blue with a diagonal hatch and the memcached-protocol
variant the same blue with a dotted hatch (dashed lines with hollow markers in line charts), bars show the mean over
repetitions with a whisker from the minimum to the maximum repetition, values are written at bar tips, the
legend is always present, grid lines are hairlines.
"""
import argparse
import json
import pathlib
import re
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.patches import Patch  # noqa: E402
from matplotlib.lines import Line2D  # noqa: E402

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import bench_data  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[2]
PREVIEW_DIR = None  # set by --preview-png: also writes PNG files for visual inspection

THEMES = {
    "light": {"suffix": "", "surface": "#fcfcfb", "text": "#0b0b0b", "text2": "#52514e", "muted": "#8a8984",
              "grid": "#e8e7e3", "series": ["#2a78d6", "#eb6834", "#1baf7a", "#eda100"]},
    "dark": {"suffix": "-dark", "surface": "#1a1a19", "text": "#f0efec", "text2": "#c3c2b7", "muted": "#8f8e87",
             "grid": "#2c2c2a", "series": ["#3987e5", "#d95926", "#199e70", "#c98500"]},
}

# Fixed entity -> slot mapping (color follows the entity, never its rank).
SYSTEM_SLOT = {"kvstore": 0, "valkey": 1, "dragonfly": 2, "memcached": 3, "kvstore-rdma": 0, "kvstore-mc": 0,
               "dragonfly-evict": 2}
SYSTEM_LABEL = {"kvstore": "arenakv (TCP)", "valkey": "Valkey", "dragonfly": "Dragonfly", "memcached": "memcached",
                "kvstore-rdma": "arenakv (Soft-RoCE)", "kvstore-mc": "arenakv (memcached protocol)",
                "dragonfly-evict": "Dragonfly (eviction flag raised)"}
HATCH = {"kvstore-rdma": "////", "kvstore-mc": "...."}
# Variants drawn dashed with hollow markers in line charts.
VARIANT = {"kvstore-mc", "dragonfly-evict"}
BAR_ORDER = ["kvstore", "kvstore-rdma", "kvstore-mc", "valkey", "dragonfly", "memcached"]
LINE_ORDER = ["kvstore", "valkey", "dragonfly", "memcached"]
LOOP_ORDER = ["kvstore", "kvstore-mc", "valkey", "dragonfly", "memcached"]
CAPACITY_ORDER = ["kvstore", "valkey", "dragonfly", "dragonfly-evict", "memcached"]
WORKLOAD_TITLE = {"small": "64-byte values", "block": "16 KiB values", "kvcache": "KV-cache requests"}
TRANSPORT_SLOT = {"rdma-poll": 0, "tcp-poll": 1, "rdma-event": 2}
TRANSPORT_LABEL = {"tcp-poll": "TCP", "rdma-poll": "Soft-RoCE, polling", "rdma-event": "Soft-RoCE, event-driven"}


def compact(v):
    """Compact number formatting with three significant digits (1.95k, 607k, 12.4M)."""
    if v is None:
        return ""
    a = abs(v)
    if a >= 1e6:
        return "%.3gM" % (v / 1e6)
    if a >= 1e3:
        return "%.3gk" % (v / 1e3)
    return "%.3g" % v


def size_label(n):
    """Binary-unit label of a byte count."""
    for unit, div in (("MiB", 1 << 20), ("KiB", 1 << 10)):
        if n >= div:
            return "%d %s" % (n // div, unit)
    return "%d B" % n


def plain_axis(ax, axis="y", sizes=None):
    """Plain (non-scientific) tick labels on a logarithmic or linear axis; no minor tick labels."""
    ax_ = ax.yaxis if axis == "y" else ax.xaxis
    if sizes is not None:
        ax.set_xticks(sizes)
        ax.set_xticklabels([size_label(v) for v in sizes], fontsize=7)
        ax.minorticks_off()
        return
    ax_.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: compact(v)))
    ax_.set_minor_formatter(matplotlib.ticker.NullFormatter())


END_LABEL = {"dragonfly-evict": "Dragonfly (flag raised)", "kvstore-mc": "arenakv (mc protocol)"}


def end_labels(ax, theme, items):
    """Direct labels right of the plot area. items: (y_data, label, color). Labels keep a minimum spacing."""
    t = THEMES[theme]
    if not items:
        return
    trans = matplotlib.transforms.blended_transform_factory(ax.transAxes, ax.transData)
    inv = ax.transData.inverted()
    fig = ax.figure
    fig.canvas.draw()
    # positions in axes fraction (0..1) along y
    frac = []
    for y, label, color in items:
        frac.append([ax.transAxes.inverted().transform(ax.transData.transform((0, y)))[1], label, color, y])
    frac.sort(key=lambda r: r[0])
    gap = 0.075
    for i in range(1, len(frac)):
        if frac[i][0] - frac[i - 1][0] < gap:
            frac[i][0] = frac[i - 1][0] + gap
    for f, label, color, y in frac:
        ax.plot([1.012], [f], marker="o", markersize=5, color=color, transform=ax.transAxes, clip_on=False,
                markeredgecolor=t["surface"], markeredgewidth=1.2, linestyle="none")
        ax.text(1.03, f, label, transform=ax.transAxes, va="center", ha="left", fontsize=7.5, color=t["text2"])


def style(theme):
    t = THEMES[theme]
    plt.rcParams.update({
        "figure.facecolor": t["surface"], "axes.facecolor": t["surface"], "savefig.facecolor": t["surface"],
        "text.color": t["text"], "axes.labelcolor": t["text2"], "xtick.color": t["text2"], "ytick.color": t["text2"],
        "axes.edgecolor": t["grid"], "axes.linewidth": 0.8, "grid.color": t["grid"], "grid.linewidth": 0.6,
        "font.family": "DejaVu Sans", "font.size": 8, "axes.titlesize": 9, "axes.titleweight": "regular",
        "axes.spines.top": False, "axes.spines.right": False, "svg.fonttype": "none", "svg.hashsalt": "arenakv",
        "hatch.linewidth": 1.2, "legend.frameon": False, "legend.fontsize": 8, "legend.labelcolor": t["text2"],
    })
    return t


def finish(fig, path, theme, digest, title, subtitle):
    t = THEMES[theme]
    fig.text(0.01, 0.985, title, ha="left", va="top", fontsize=11, color=t["text"])
    fig.text(0.01, 0.945, subtitle, ha="left", va="top", fontsize=8, color=t["text2"])
    fig.savefig(path, format="svg", metadata={"Creator": None, "Date": None,
                                               "Description": "data-sha256=%s" % digest, "Title": title})
    if PREVIEW_DIR is not None:
        fig.savefig(PREVIEW_DIR / (pathlib.Path(path).stem + ".png"), format="png", dpi=110)
    plt.close(fig)


def get(summary, section, key, metric):
    row = summary[section].get(key)
    if row is None or metric not in row:
        return None
    return row[metric]


def bar_panel(ax, theme, series, values, ylabel, title, log=False):
    """series: list of (system, hatch pattern or None); values: list of stats dicts or None."""
    t = THEMES[theme]
    n = len(series)
    for i, ((system, hatch), st) in enumerate(zip(series, values)):
        if st is None:
            continue
        color = t["series"][SYSTEM_SLOT[system]]
        ax.bar(i, st["mean"], width=0.46, color=color, hatch=hatch, edgecolor=t["surface"],
               linewidth=0, zorder=3)
        ax.vlines(i, st["min"], st["max"], color=t["text2"], linewidth=1.0, zorder=4)
        ax.text(i, st["max"], compact(st["mean"]), ha="center", va="bottom", fontsize=6.5, color=t["text"],
                zorder=5)
    ax.set_xlim(-0.7, n - 0.3)
    ax.set_xticks([])
    ax.set_title(title, loc="left", color=t["text"])
    ax.set_ylabel(ylabel)
    ymax = max([v["max"] for v in values if v] or [1])
    ax.set_ylim(0, ymax * 1.18)
    ax.yaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: compact(v)))
    ax.grid(axis="y", zorder=0)
    ax.spines["left"].set_visible(False)
    ax.tick_params(axis="y", length=0)


def system_legend(fig, theme, entries, loc_y=0.0):
    t = THEMES[theme]
    handles = []
    for system, hatch in entries:
        handles.append(Patch(facecolor=t["series"][SYSTEM_SLOT[system]], hatch=hatch,
                             edgecolor=t["surface"], linewidth=0, label=SYSTEM_LABEL[system]))
    fig.legend(handles=handles, loc="lower center", ncol=len(handles), bbox_to_anchor=(0.5, loc_y))


def fig_matrix(summary, theme, path, digest, metric, ylabel, title, subtitle):
    t = style(theme)
    fig, axes = plt.subplots(2, 3, figsize=(10.5, 6.2), gridspec_kw={"hspace": 0.38, "wspace": 0.32})
    series = [(s, HATCH.get(s)) for s in BAR_ORDER]
    for r, cpus in enumerate((1, 4)):
        for c, wl in enumerate(("small", "block", "kvcache")):
            vals = [get(summary, "matrix", "%s|%d|%s" % (s, cpus, wl), metric) for s, _ in series]
            unit = ylabel
            if metric == "throughput" and wl == "kvcache":
                unit = "requests / s"
            bar_panel(axes[r][c], theme, series, vals, unit if c == 0 else "",
                      "%d server CPU%s, %s" % (cpus, "" if cpus == 1 else "s", WORKLOAD_TITLE[wl]))
    system_legend(fig, theme, series, 0.0)
    fig.subplots_adjust(top=0.86, bottom=0.11, left=0.07, right=0.99)
    finish(fig, path, theme, digest, title, subtitle)


def fig_sweep(summary, theme, path, digest):
    t = style(theme)
    fig, axes = plt.subplots(1, 2, figsize=(10.5, 4.4), gridspec_kw={"wspace": 0.22})
    rates = sorted({int(k.split("|")[1]) for k in summary["sweep"]})
    for ax, metric, ylabel, log in ((axes[0], "achieved_ops", "achieved operations / s", False),
                                    (axes[1], "p99_us", "p99 latency from intended send time (us)", True)):
        for system in LINE_ORDER:
            xs, ys, lo, hi = [], [], [], []
            for rate in rates:
                st = get(summary, "sweep", "%s|%d" % (system, rate), metric)
                if st:
                    xs.append(rate)
                    ys.append(st["mean"])
                    lo.append(max(0.0, st["mean"] - st["min"]))
                    hi.append(max(0.0, st["max"] - st["mean"]))
            if not xs:
                continue
            color = t["series"][SYSTEM_SLOT[system]]
            ax.errorbar(xs, ys, yerr=[lo, hi], color=color, linewidth=2, marker="o", markersize=5,
                        markeredgecolor=t["surface"], markeredgewidth=1.5, ecolor=color, elinewidth=1,
                        capsize=0, zorder=3)
        if metric == "achieved_ops":
            ax.plot([min(rates), max(rates)], [min(rates), max(rates)], color=t["muted"], linewidth=1,
                    linestyle=(0, (1, 2)), zorder=1)
            ax.text(min(rates) + (max(rates) - min(rates)) * 0.55, min(rates) + (max(rates) - min(rates)) * 0.55,
                    "  ideal: achieved = offered", color=t["muted"], fontsize=7, va="top", ha="left", rotation=0)
        if log:
            ax.set_yscale("log")
        ax.set_xlabel("offered operations / s")
        ax.set_ylabel(ylabel)
        ax.xaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: compact(v)))
        plain_axis(ax, "y")
        ax.grid(True, zorder=0)
        ax.spines["left"].set_visible(False)
        ax.tick_params(length=0)
        if log:
            ends = []
            for system in LINE_ORDER:
                last = [get(summary, "sweep", "%s|%d" % (system, r), metric) for r in rates]
                last = [x for x in last if x]
                if last:
                    ends.append((last[-1]["mean"], SYSTEM_LABEL[system], t["series"][SYSTEM_SLOT[system]]))
            fig.canvas.draw()
            end_labels(ax, theme, ends)
    handles = [Line2D([0], [0], color=t["series"][SYSTEM_SLOT[s]], linewidth=2, marker="o", markersize=5,
                      markeredgecolor=t["surface"], label=SYSTEM_LABEL[s]) for s in LINE_ORDER]
    fig.legend(handles=handles, loc="lower center", ncol=4, bbox_to_anchor=(0.5, 0.0))
    fig.subplots_adjust(top=0.84, bottom=0.2, left=0.07, right=0.9)
    finish(fig, path, theme, digest, "Open-loop load sweep, one server CPU, 64-byte values",
           "16 connections, constant arrivals, 90% GET. Points: mean of repetitions; whiskers: minimum to maximum.")


def fig_coordinated(summary, theme, path, digest):
    """Latency measured from the intended send time versus pure service time (same runs)."""
    t = style(theme)
    fig, axes = plt.subplots(1, 2, figsize=(10.5, 4.2), gridspec_kw={"wspace": 0.22})
    rates = sorted({int(k.split("|")[1]) for k in summary["sweep"]})
    for ax, system in zip(axes, ("valkey", "memcached")):
        for metric, label, color in (("p99_us", "from intended send time", t["series"][SYSTEM_SLOT[system]]),
                                     ("service_p99_us", "service time only", t["muted"])):
            xs = [r for r in rates if get(summary, "sweep", "%s|%d" % (system, r), metric)]
            ys = [summary["sweep"]["%s|%d" % (system, r)][metric]["mean"] for r in xs]
            if xs:
                ax.plot(xs, ys, color=color, linewidth=2, marker="o", markersize=5,
                        markeredgecolor=t["surface"], markeredgewidth=1.5, label=label, zorder=3)
        ax.set_yscale("log")
        ax.set_title(SYSTEM_LABEL[system], loc="left", color=t["text"])
        ax.set_xlabel("offered operations / s")
        ax.set_ylabel("p99 latency (us)")
        ax.xaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: compact(v)))
        plain_axis(ax, "y")
        ax.grid(True, zorder=0)
        ax.spines["left"].set_visible(False)
        ax.tick_params(length=0)
        ax.legend(loc="upper left")
    fig.subplots_adjust(top=0.82, bottom=0.14, left=0.07, right=0.98)
    finish(fig, path, theme, digest, "Why latency is measured from the intended send time",
           "Above saturation the latency from the intended send time grows with the backlog; the service time does not.")


def fig_capacity(summary, theme, path, digest):
    t = style(theme)
    fig, axes = plt.subplots(1, 2, figsize=(10.5, 4.4), gridspec_kw={"wspace": 0.22})
    caps = ["16mb", "64mb", "256mb", "1gb", "4gb"]
    labels = {"16mb": "16 MB", "64mb": "64 MB", "256mb": "256 MB", "1gb": "1 GB", "4gb": "4 GB"}
    for ax, metric, ylabel in ((axes[0], "reuse_rate", "reuse rate (fraction of needed blocks served from cache)"),
                               (axes[1], "throughput", "requests / s")):
        for system in CAPACITY_ORDER:
            pts = [(i, get(summary, "capacity", "%s|%s" % (system, c), metric)) for i, c in enumerate(caps)]
            pts = [(i, st) for i, st in pts if st]
            if not pts:
                continue
            color = t["series"][SYSTEM_SLOT[system]]
            variant = system == "dragonfly-evict"
            ax.errorbar([i for i, _ in pts], [st["mean"] for _, st in pts],
                        yerr=[[max(0.0, st["mean"] - st["min"]) for _, st in pts], [max(0.0, st["max"] - st["mean"]) for _, st in pts]],
                        color=color, linewidth=2, linestyle=(0, (4, 2)) if variant else "-", marker="o", markersize=5,
                        markerfacecolor=t["surface"] if variant else color, markeredgecolor=color if variant else t["surface"],
                        markeredgewidth=1.5, ecolor=color, elinewidth=1, capsize=0, zorder=3)
        ax.set_xticks(range(len(caps)))
        ax.set_xticklabels([labels[c] for c in caps])
        ax.set_xlabel("configured cache capacity")
        ax.set_ylabel(ylabel)
        ax.grid(True, zorder=0)
        ax.spines["left"].set_visible(False)
        ax.tick_params(length=0)
        if metric == "throughput":
            ax.yaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: compact(v)))
        else:
            ax.set_ylim(0, 1)
        if metric == "throughput":
            ax.set_ylim(0, None)
            ends = []
            for system in CAPACITY_ORDER:
                pts = [get(summary, "capacity", "%s|%s" % (system, c), metric) for c in caps]
                pts = [x for x in pts if x]
                if pts:
                    ends.append((pts[-1]["mean"], END_LABEL.get(system, SYSTEM_LABEL[system]), t["series"][SYSTEM_SLOT[system]]))
            fig.canvas.draw()
            end_labels(ax, theme, ends)
    handles = [Line2D([0], [0], color=t["series"][SYSTEM_SLOT[s]], linewidth=2,
                      linestyle=(0, (4, 2)) if s == "dragonfly-evict" else "-", marker="o", markersize=5,
                      markerfacecolor=t["surface"] if s == "dragonfly-evict" else t["series"][SYSTEM_SLOT[s]],
                      markeredgecolor=t["series"][SYSTEM_SLOT[s]] if s == "dragonfly-evict" else t["surface"],
                      label=SYSTEM_LABEL[s]) for s in CAPACITY_ORDER if any(k.startswith(s + "|") for k in summary["capacity"])]
    fig.legend(handles=handles, loc="lower center", ncol=5, bbox_to_anchor=(0.5, 0.0))
    fig.subplots_adjust(top=0.84, bottom=0.2, left=0.08, right=0.84, wspace=0.3)
    finish(fig, path, theme, digest, "KV-cache workload: reuse rate versus configured capacity",
           "One server CPU, 8 connections, LRU eviction. Dragonfly needs at least 256 MiB per thread. Dashed: Dragonfly with --max_eviction_per_heartbeat raised.")


def fig_transport(summary, theme, path, digest):
    t = style(theme)
    fig, axes = plt.subplots(1, 2, figsize=(10.5, 4.4), gridspec_kw={"wspace": 0.22})
    sizes = sorted({int(k.split("|")[2]) for k in summary["transport"]})
    for ax, metric, ylabel in ((axes[0], "rtt_p50_us", "median round trip (us)"),
                               (axes[1], "stream_mb_s", "streaming throughput (MB / s)")):
        for label in ("tcp-poll", "rdma-poll", "rdma-event"):
            pts = [(s, get(summary, "transport", "loopback|%s|%d" % (label, s), metric)) for s in sizes]
            pts = [(s, st) for s, st in pts if st]
            if not pts:
                continue
            color = t["series"][TRANSPORT_SLOT[label]]
            ax.errorbar([s for s, _ in pts], [st["mean"] for _, st in pts],
                        yerr=[[max(0.0, st["mean"] - st["min"]) for _, st in pts], [max(0.0, st["max"] - st["mean"]) for _, st in pts]],
                        color=color, linewidth=2, marker="o", markersize=5, markeredgecolor=t["surface"],
                        markeredgewidth=1.5, ecolor=color, elinewidth=1, capsize=0, zorder=3)
        ax.set_xscale("log", base=2)
        ax.set_yscale("log")
        ax.set_xlabel("message size")
        ax.set_ylabel(ylabel)
        plain_axis(ax, "x", sizes=[v for v in sizes if v in (64, 1024, 16384, 262144, 1048576)])
        plain_axis(ax, "y")
        ax.grid(True, zorder=0)
        ax.spines["left"].set_visible(False)
        ax.tick_params(length=0)
        if metric == "stream_mb_s":
            ends = []
            for label in ("tcp-poll", "rdma-poll", "rdma-event"):
                pts = [get(summary, "transport", "loopback|%s|%d" % (label, sz), metric) for sz in sizes]
                pts = [x for x in pts if x]
                if pts:
                    ends.append((pts[-1]["mean"], TRANSPORT_LABEL[label], t["series"][TRANSPORT_SLOT[label]]))
            fig.canvas.draw()
            end_labels(ax, theme, ends)
    handles = [Line2D([0], [0], color=t["series"][TRANSPORT_SLOT[k]], linewidth=2, marker="o", markersize=5,
                      markeredgecolor=t["surface"], label=TRANSPORT_LABEL[k]) for k in ("tcp-poll", "rdma-poll", "rdma-event")]
    fig.legend(handles=handles, loc="lower center", ncol=3, bbox_to_anchor=(0.5, 0.0))
    fig.subplots_adjust(top=0.84, bottom=0.2, left=0.08, right=0.83, wspace=0.28)
    finish(fig, path, theme, digest, "rdmaduct: round trip and streaming throughput on one host (loopback)",
           "Both endpoints on the same machine; Soft-RoCE is a software implementation of RDMA, not RDMA hardware.")


def fig_engine(summary, theme, path, digest):
    t = style(theme)
    fig, axes = plt.subplots(1, 3, figsize=(10.5, 4.2), gridspec_kw={"wspace": 0.36, "width_ratios": [1.3, 1, 1.1]})
    ax = axes[0]
    threads = sorted({int(k.split("|")[1]) for k in summary["engine"]["scale"]})
    for vs, slot, label in ((64, 0, "64-byte values"), (4096, 1, "4 KiB values")):
        pts = [(th, summary["engine"]["scale"].get("%d|%d" % (vs, th))) for th in threads]
        pts = [(th, r["ops_per_sec"]) for th, r in pts if r]
        if not pts:
            continue
        color = t["series"][slot]
        ax.errorbar([th for th, _ in pts], [st["mean"] for _, st in pts],
                    yerr=[[max(0.0, st["mean"] - st["min"]) for _, st in pts], [max(0.0, st["max"] - st["mean"]) for _, st in pts]],
                    color=color, linewidth=2, marker="o", markersize=5, markeredgecolor=t["surface"],
                    markeredgewidth=1.5, ecolor=color, elinewidth=1, capsize=0, label=label, zorder=3)
    if max(threads) > 8:
        ax.axvspan(8.5, max(threads) + 0.5, color=t["grid"], alpha=0.6, zorder=0, linewidth=0)
        ax.text(8.7, 0.03, "hyper-thread siblings", fontsize=7, color=t["muted"], va="bottom",
                transform=matplotlib.transforms.blended_transform_factory(ax.transData, ax.transAxes))
    ax.set_xlabel("threads")
    ax.set_ylabel("operations / s (90% GET)")
    ax.set_xticks(threads)
    ax.yaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: compact(v)))
    ax.set_title("engine scaling", loc="left", color=t["text"])
    ax.legend(loc="upper left")
    ax.grid(True, zorder=0)
    ax.spines["left"].set_visible(False)
    ax.tick_params(length=0)

    ax = axes[1]
    m = summary["engine"]["micro"].get("match16")
    if m:
        for i, (key, label, slot) in enumerate((("vector_ns_per_call", "vector", 0), ("scalar_ns_per_call", "scalar", 1))):
            st = m[key]
            ax.bar(i, st["mean"], width=0.46, color=t["series"][slot], linewidth=0, zorder=3)
            ax.vlines(i, st["min"], st["max"], color=t["text2"], linewidth=1.0, zorder=4)
            ax.text(i, st["max"], "%.2f ns" % st["mean"], ha="center", va="bottom", fontsize=7, color=t["text"])
        ax.set_xticks([0, 1])
        ax.set_xticklabels(["%s\n(%s)" % ("vector", summary["engine"]["micro"].get("simd", "")), "scalar\nreference"])
        ax.set_ylim(0, max(m["scalar_ns_per_call"]["max"], 1) * 1.2)
    ax.set_ylabel("ns per 16-lane tag comparison")
    ax.set_title("tag matching", loc="left", color=t["text"])
    ax.grid(axis="y", zorder=0)
    ax.spines["left"].set_visible(False)
    ax.tick_params(length=0)

    ax = axes[2]
    lens = sorted(int(k.split("|")[1]) for k in summary["engine"]["micro"] if k.startswith("hash|"))
    pts = [(ln, summary["engine"]["micro"]["hash|%d" % ln]["gb_per_s"]) for ln in lens]
    if pts:
        ax.errorbar([ln for ln, _ in pts], [st["mean"] for _, st in pts],
                    yerr=[[max(0.0, st["mean"] - st["min"]) for _, st in pts], [max(0.0, st["max"] - st["mean"]) for _, st in pts]],
                    color=t["series"][0], linewidth=2, marker="o", markersize=5, markeredgecolor=t["surface"],
                    markeredgewidth=1.5, ecolor=t["series"][0], elinewidth=1, capsize=0, zorder=3)
    ax.set_xscale("log", base=2)
    ax.set_xticks([8, 64, 512, 4096])
    ax.set_xticklabels(["8 B", "64 B", "512 B", "4 KiB"], fontsize=7)
    ax.minorticks_off()
    ax.set_xlabel("key length")
    ax.set_ylabel("hash throughput (GB / s)")
    ax.set_title("key hash", loc="left", color=t["text"])
    ax.grid(True, zorder=0)
    ax.spines["left"].set_visible(False)
    ax.tick_params(length=0)
    fig.subplots_adjust(top=0.84, bottom=0.14, left=0.07, right=0.99)
    finish(fig, path, theme, digest, "arenakv engine micro-benchmarks (no network)",
           "Points and bars: mean of repetitions; whiskers: minimum to maximum. Hash: consecutive calls are data-dependent (latency bound).")


def line_series(ax, theme, system, xs, stats_list):
    """One system's line with min-max whiskers; variants are dashed with hollow markers."""
    t = THEMES[theme]
    pts = [(x, st) for x, st in zip(xs, stats_list) if st]
    if not pts:
        return
    color = t["series"][SYSTEM_SLOT[system]]
    variant = system in VARIANT
    ax.errorbar([x for x, _ in pts], [st["mean"] for _, st in pts],
                yerr=[[max(0.0, st["mean"] - st["min"]) for _, st in pts], [max(0.0, st["max"] - st["mean"]) for _, st in pts]],
                color=color, linewidth=2, linestyle=(0, (4, 2)) if variant else "-", marker="o", markersize=5,
                markerfacecolor=t["surface"] if variant else color, markeredgecolor=color if variant else t["surface"],
                markeredgewidth=1.5, ecolor=color, elinewidth=1, capsize=0, zorder=3)


def line_legend(fig, theme, systems):
    t = THEMES[theme]
    handles = [Line2D([0], [0], color=t["series"][SYSTEM_SLOT[s]], linewidth=2,
                      linestyle=(0, (4, 2)) if s in VARIANT else "-", marker="o", markersize=5,
                      markerfacecolor=t["surface"] if s in VARIANT else t["series"][SYSTEM_SLOT[s]],
                      markeredgecolor=t["series"][SYSTEM_SLOT[s]] if s in VARIANT else t["surface"],
                      label=SYSTEM_LABEL[s]) for s in systems]
    if handles:
        fig.legend(handles=handles, loc="lower center", ncol=len(handles), bbox_to_anchor=(0.5, 0.0))


def fig_concurrency(summary, theme, path, digest):
    """Throughput versus pipeline depth and versus connection count, p99 versus connection count."""
    t = style(theme)
    fig, axes = plt.subplots(1, 3, figsize=(10.5, 4.3), gridspec_kw={"wspace": 0.3})
    depths = sorted({int(k.split("|")[1]) for k in summary["pipeline"]})
    conns = sorted({int(k.split("|")[1]) for k in summary["connections"]})
    panels = ((axes[0], "pipeline", depths, "throughput", "operations / s", "requests in flight per connection",
               "16 connections, pipelined", False),
              (axes[1], "connections", conns, "throughput", "operations / s", "connections", "no pipelining", False),
              (axes[2], "connections", conns, "p99_us", "GET p99 latency (us)", "connections", "no pipelining", True))
    for ax, section, xs, metric, ylabel, xlabel, title, log in panels:
        for system in LOOP_ORDER:
            line_series(ax, theme, system, xs, [get(summary, section, "%s|%d" % (system, x), metric) for x in xs])
        if xs:
            ax.set_xscale("log", base=2)
            ax.set_xticks(xs)
            ax.set_xticklabels([str(x) for x in xs])
            ax.minorticks_off()
        if log and xs:
            ax.set_yscale("log")
            plain_axis(ax, "y")
        else:
            ax.set_ylim(0, None)
            ax.yaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: compact(v)))
        ax.set_xlabel(xlabel)
        ax.set_ylabel(ylabel)
        ax.set_title(title, loc="left", color=t["text"])
        ax.grid(True, zorder=0)
        ax.spines["left"].set_visible(False)
        ax.tick_params(length=0)
    line_legend(fig, theme, [s for s in LOOP_ORDER if any(k.startswith(s + "|") for k in summary["connections"])])
    fig.subplots_adjust(top=0.82, bottom=0.22, left=0.07, right=0.99)
    finish(fig, path, theme, digest, "Pipelining and connection count, one server CPU, 64-byte values",
           "Closed loop, 4 client threads, 90% GET. Points: mean of repetitions; whiskers: minimum to maximum.")

def fig_cpu_split(summary, theme, path, digest):
    """Server CPU time per operation split into kernel and user mode, pipeline depths 1 and 32."""
    t = style(theme)
    depths = [d for d in (1, 32) if any(k.endswith("|%d" % d) for k in summary["pipeline"])]
    fig, axes = plt.subplots(1, max(1, len(depths)), figsize=(10.5, 4.3), gridspec_kw={"wspace": 0.25})
    axes = list(axes) if hasattr(axes, "__len__") else [axes]
    drawn = False
    for ax, depth in zip(axes, depths):
        ymax = 0.0
        for i, system in enumerate(LOOP_ORDER):
            k = "%s|%d" % (system, depth)
            ker = get(summary, "pipeline", k, "server_system_us_per_op")
            usr = get(summary, "pipeline", k, "server_user_us_per_op")
            tot = get(summary, "pipeline", k, "server_cpu_us_per_op")
            share = get(summary, "pipeline", k, "server_system_share")
            if ker is None or usr is None or tot is None or share is None:
                continue
            drawn = True
            color = t["series"][SYSTEM_SLOT[system]]
            hatch = HATCH.get(system)
            ax.bar(i, ker["mean"], width=0.5, color=color, hatch=hatch, edgecolor=t["surface"], linewidth=0, zorder=3)
            ax.bar(i, usr["mean"], width=0.5, bottom=ker["mean"], color=color, alpha=0.35, hatch=hatch,
                   edgecolor=t["surface"], linewidth=0, zorder=3)
            ax.vlines(i, tot["min"], tot["max"], color=t["text2"], linewidth=1.0, zorder=4)
            ax.text(i, tot["max"], "%.2f us\nkernel %.0f%%" % (tot["mean"], share["mean"] * 100), ha="center",
                    va="bottom", fontsize=6.5, color=t["text"], zorder=5)
            ymax = max(ymax, tot["max"])
        ax.set_xlim(-0.7, len(LOOP_ORDER) - 0.3)
        ax.set_xticks([])
        ax.set_ylim(0, (ymax or 1) * 1.28)
        ax.set_ylabel("server CPU (us / op)")
        ax.set_title("%d request%s in flight per connection" % (depth, "" if depth == 1 else "s"), loc="left",
                     color=t["text"])
        ax.grid(axis="y", zorder=0)
        ax.spines["left"].set_visible(False)
        ax.tick_params(axis="y", length=0)
    if not drawn:
        axes[0].text(0.5, 0.5, "The kernel and user split is not recorded in this run.", ha="center", va="center",
                     transform=axes[0].transAxes, color=t["text2"])
    handles = [Patch(facecolor=t["series"][SYSTEM_SLOT[s]], hatch=HATCH.get(s), edgecolor=t["surface"], linewidth=0,
                     label=SYSTEM_LABEL[s]) for s in LOOP_ORDER]
    handles += [Patch(facecolor=t["muted"], label="kernel mode (solid)"),
                Patch(facecolor=t["muted"], alpha=0.35, label="user mode (faded)")]
    fig.legend(handles=handles, loc="lower center", ncol=len(handles), bbox_to_anchor=(0.5, 0.0), fontsize=7)
    fig.subplots_adjust(top=0.78, bottom=0.14, left=0.07, right=0.99)
    finish(fig, path, theme, digest, "Where the server CPU time per operation goes: kernel and user mode",
           "One server CPU, 16 connections over loopback TCP, 64-byte values, 90% GET. Bars: mean of repetitions; whiskers: "
           "minimum to maximum of the total.\nSolid: kernel mode, i.e. system calls and the TCP stack (stime of "
           "/proc/<pid>/stat); faded: user mode.")

# ---- tables ---------------------------------------------------------------------------------------------

def fmt_stat(st, digits=0, unit=""):
    if not st:
        return "n/a"
    mean = st["mean"]
    s = ("{:,.%df}" % digits).format(mean)
    return "%s%s (cv %.1f%%)" % (s, unit, st["cv"] * 100)


def fmt_pct(st):
    """Mean of a fraction as a percentage, with its coefficient of variation."""
    if not st:
        return "n/a"
    return "%.0f%% (cv %.1f%%)" % (st["mean"] * 100, st["cv"] * 100)


def md_table(header, rows):
    out = ["| " + " | ".join(header) + " |", "|" + "|".join("---" for _ in header) + "|"]
    for r in rows:
        out.append("| " + " | ".join(str(c) for c in r) + " |")
    return "\n".join(out) + "\n"


def provenance(summary, digest):
    env = summary["env"]
    m = summary["manifest"]
    return ("Data: `benchmarks/data/%s` (%d runs: %d ok, %d with operation errors, %d failed, %d skipped), commit `%s`, "
            "summary digest `%s`. Mean over repetitions; `cv` is the coefficient of variation.\n"
            % (summary["run"], m["runs"], m["ok"], m["with_errors"], m["failed"], len(m["skipped"]),
               env["git_commit"][:12], digest[:16]))


def tables(summary, digest):
    out = {}
    pv = provenance(summary, digest)
    env = summary["env"]
    out["environment"] = md_table(["Item", "Value"], [
        ["Run", summary["run"]],
        ["Date (UTC)", env["timestamp_utc"]],
        ["CPU", env["cpu_model"]],
        ["Logical CPUs", env["logical_cpus"]],
        ["CPU governor", env["cpu_governor"]],
        ["Memory", "%.1f GiB" % (env["memory_kib"] / 1048576)],
        ["Kernel", env["kernel"]],
        ["OS", env["os"]],
        ["Compiler", env["compiler"]],
        ["Network interface", "%s, %s Mbit/s" % (env["nic"], env["nic_speed_mbit"])],
        ["RDMA devices", env["rdma_devices"]],
        ["Valkey", env["valkey_version"]],
        ["Dragonfly", env["dragonfly_version"]],
        ["memcached", env["memcached_version"]],
        ["Container runtime", "Docker %s" % env["docker"]],
        ["Source commit", env["git_commit"][:12]],
        ["Uncommitted files at run start", env["git_uncommitted_files"]],
    ] + [[label, env[key]] for label, key in (("Containers running at run start", "running_containers"),
                                              ("Load average (1 min) at run start", "load_avg_1min")) if key in env])

    def matrix_table(metric, digits, title_unit):
        rows = []
        for wl in ("small", "block", "kvcache"):
            for cpus in (1, 4):
                row = [WORKLOAD_TITLE[wl], cpus]
                for s in BAR_ORDER:
                    row.append(fmt_stat(get(summary, "matrix", "%s|%d|%s" % (s, cpus, wl), metric), digits))
                rows.append(row)
        return md_table(["Workload", "Server CPUs"] + [SYSTEM_LABEL[s] for s in BAR_ORDER], rows)

    out["matrix-throughput"] = pv + "\nOperations per second (KV-cache rows: requests per second).\n\n" + matrix_table("throughput", 0, "")
    out["matrix-cpu"] = pv + "\nServer CPU time per operation in microseconds (KV-cache rows: per block GET or PUT).\n\n" + matrix_table("server_cpu_us_per_op", 2, "")
    out["matrix-p99"] = pv + "\nGET latency p99 in microseconds in the closed loop (KV-cache rows: request latency).\n\n" + matrix_table("p99_us", 0, "")
    out["matrix-rss"] = pv + "\nServer resident memory at the end of the run in MiB.\n\n" + matrix_table("server_rss_mib", 0, "")

    rates = sorted({int(k.split("|")[1]) for k in summary["sweep"]})
    rows = []
    for s in LINE_ORDER:
        for r in rates:
            k = "%s|%d" % (s, r)
            if k in summary["sweep"]:
                rows.append([SYSTEM_LABEL[s], "{:,}".format(r), fmt_stat(get(summary, "sweep", k, "achieved_ops"), 0),
                             fmt_stat(get(summary, "sweep", k, "p50_us"), 0), fmt_stat(get(summary, "sweep", k, "p99_us"), 0),
                             fmt_stat(get(summary, "sweep", k, "service_p99_us"), 0),
                             fmt_stat(get(summary, "sweep", k, "max_lag_ms"), 1)])
    out["sweep"] = pv + "\nOpen loop, one server CPU, 64-byte values, 16 connections. Latencies in microseconds, measured from the intended send time unless stated.\n\n" + md_table(
        ["System", "Offered ops/s", "Achieved ops/s", "p50", "p99", "p99 service time", "Max schedule lag (ms)"], rows)

    caps = ["16mb", "64mb", "256mb", "1gb", "4gb"]
    rows = []
    for s in CAPACITY_ORDER:
        row = [SYSTEM_LABEL[s]]
        for c in caps:
            row.append(fmt_stat(get(summary, "capacity", "%s|%s" % (s, c), "reuse_rate"), 3))
        rows.append(row)
    out["capacity-reuse"] = pv + "\nKV-cache scenario, reuse rate (fraction of needed blocks served from the cache) by configured capacity.\n\n" + md_table(
        ["System"] + ["16 MB", "64 MB", "256 MB", "1 GB", "4 GB"], rows)
    rows = []
    for s in CAPACITY_ORDER:
        row = [SYSTEM_LABEL[s]]
        for c in caps:
            row.append(fmt_stat(get(summary, "capacity", "%s|%s" % (s, c), "throughput"), 0))
        rows.append(row)
    out["capacity-throughput"] = pv + "\nKV-cache scenario, requests per second by configured capacity.\n\n" + md_table(
        ["System"] + ["16 MB", "64 MB", "256 MB", "1 GB", "4 GB"], rows)
    rows = []
    for s in CAPACITY_ORDER:
        row = [SYSTEM_LABEL[s]]
        for c in caps:
            st = get(summary, "capacity", "%s|%s" % (s, c), "error_rate")
            row.append("n/a" if not st else "%.2f%%" % (st["mean"] * 100))
        rows.append(row)
    out["capacity-errors"] = pv + "\nShare of KV-cache requests that failed (an operation was rejected by the server, for example with an out-of-memory error), by configured capacity.\n\n" + md_table(
        ["System"] + ["16 MB", "64 MB", "256 MB", "1 GB", "4 GB"], rows)
    if summary["manifest"]["skipped"]:
        skipped = sorted({(x["system"], x["capacity"], x["reason"]) for x in summary["manifest"]["skipped"]})
        out["capacity-skipped"] = "Skipped configurations: " + "; ".join("%s at %s (%s)" % x for x in skipped) + ".\n"

    threads = sorted({int(k.split("|")[1]) for k in summary["engine"]["scale"]})
    rows = []
    for vs in (64, 4096):
        row = ["%d-byte values" % vs]
        for th in threads:
            r = summary["engine"]["scale"].get("%d|%d" % (vs, th))
            row.append(fmt_stat(r["ops_per_sec"], 0) if r else "n/a")
        rows.append(row)
    out["engine-scale"] = pv + "\nEngine operations per second (90% GET, uniform keys, key space preloaded, no eviction).\n\n" + md_table(
        ["Value size"] + ["%d threads" % th for th in threads], rows)
    mm = summary["engine"]["micro"].get("match16")
    rows = []
    if mm:
        rows.append(["16-lane tag comparison, vector (%s)" % summary["engine"]["micro"].get("simd", ""), fmt_stat(mm["vector_ns_per_call"], 3, " ns")])
        rows.append(["16-lane tag comparison, scalar reference", fmt_stat(mm["scalar_ns_per_call"], 3, " ns")])
        rows.append(["Speedup of the vector version", fmt_stat(mm["speedup"], 1, "x")])
    for k in sorted((k for k in summary["engine"]["micro"] if k.startswith("hash|")), key=lambda x: int(x.split("|")[1])):
        rows.append(["Key hash, %s-byte key" % k.split("|")[1], fmt_stat(summary["engine"]["micro"][k]["ns_per_hash"], 2, " ns")])
    out["engine-micro"] = pv + "\n" + md_table(["Measurement", "Result"], rows)

    for section, xname, xlabel, text in (
            ("pipeline", "Depth", "requests in flight per connection",
             "Closed loop, one server CPU, 64-byte values, 90% GET, 16 connections from 4 client threads, with the given number of requests in flight per connection."),
            ("connections", "Connections", "connections",
             "Closed loop, one server CPU, 64-byte values, 90% GET, connections spread over 4 client threads, one request in flight per connection.")):
        xs = sorted({int(k.split("|")[1]) for k in summary[section]})
        rows = []
        for sname in LOOP_ORDER:
            for x in xs:
                k = "%s|%d" % (sname, x)
                if k in summary[section]:
                    rows.append([SYSTEM_LABEL[sname], "{:,}".format(x), fmt_stat(get(summary, section, k, "throughput"), 0),
                                 fmt_stat(get(summary, section, k, "p50_us"), 0), fmt_stat(get(summary, section, k, "p99_us"), 0),
                                 fmt_stat(get(summary, section, k, "server_cpu_us_per_op"), 2),
                                 fmt_pct(get(summary, section, k, "server_system_share")),
                                 fmt_stat(get(summary, section, k, "server_rss_mib"), 0)])
        out[section] = pv + "\n" + text + (" Latencies are GET latencies in microseconds. Server kernel share: kernel-mode part"
                                          " of the server CPU time (stime in /proc/<pid>/stat), that is system calls and the"
                                          " TCP stack run on the server's behalf.\n\n") + md_table(
            ["System", xname, "ops/s", "p50", "p99", "Server CPU µs/op", "Server kernel share", "Server RSS MiB"], rows)

    rows = []
    vlabel = {"valkey": "Valkey (RESP)", "kvstore-mc": "arenakv (memcached protocol)"}
    for server in ("valkey", "kvstore-mc"):
        for conns in (1, 16):
            mt = summary["validation"].get("%s|memtier|%d" % (server, conns))
            for tool in ("kvpace", "kvpace-threads"):
                kb = summary["validation"].get("%s|%s|%d" % (server, tool, conns))
                if not (kb and mt):
                    continue
                desc = "1 thread, 1 connection" if conns == 1 else (
                    "4 threads x 4 connections" if tool == "kvpace" else "16 threads x 1 connection")
                rows.append([vlabel[server], conns, desc, fmt_stat(kb["throughput"], 0), fmt_stat(mt["throughput"], 0),
                             "%.2f" % (kb["throughput"]["mean"] / mt["throughput"]["mean"]),
                             fmt_stat(kb["p50_us"], 1), fmt_stat(mt["p50_us"], 1),
                             fmt_stat(kb["p99_us"], 0), fmt_stat(mt["p99_us"], 0)])
    out["validation"] = pv + "\nOne server CPU, 64-byte values, 90% GET. memtier_benchmark runs `-t 1 -c 1` for one connection and `-t 4 -c 4` for 16. Latencies are GET latencies in microseconds; memtier reports milliseconds with three decimals.\n\n" + md_table(
        ["Server", "Connections", "kvpace client model", "kvpace ops/s", "memtier ops/s", "Ratio", "kvpace p50", "memtier p50", "kvpace p99", "memtier p99"], rows)

    sizes = sorted({int(k.split("|")[2]) for k in summary["transport"]})
    for metric, digits, name, text in (("rtt_p50_us", 1, "transport-rtt", "Median round-trip time in microseconds (ping-pong, one message in flight)."),
                                       ("rtt_p99_us", 1, "transport-rtt-p99", "p99 round-trip time in microseconds."),
                                       ("stream_mb_s", 0, "transport-stream", "One-way streaming throughput in MB/s (batched sends, 64 buffers per direction).")):
        rows = []
        for sz in sizes:
            row = [sz]
            for label in ("tcp-poll", "rdma-poll", "rdma-event"):
                row.append(fmt_stat(get(summary, "transport", "loopback|%s|%d" % (label, sz), metric), digits))
            rows.append(row)
        out[name] = pv + "\n" + text + " Loopback on one host.\n\n" + md_table(
            ["Message bytes"] + [TRANSPORT_LABEL[k] for k in ("tcp-poll", "rdma-poll", "rdma-event")], rows)
    return out


FIGURES = {
    "throughput": lambda s, th, p, d: fig_matrix(s, th, p, d, "throughput", "operations / s",
                                                 "Throughput by system, workload and server CPUs",
                                                 "Closed loop, 16 connections. Bars: mean of repetitions; whiskers: minimum to maximum. Diagonal hatch: arenakv over Soft-RoCE (software RDMA); dotted: arenakv over the memcached protocol."),
    "cpu-per-op": lambda s, th, p, d: fig_matrix(s, th, p, d, "server_cpu_us_per_op", "server CPU (us / op)",
                                                 "Server CPU time per operation",
                                                 "Same runs as the throughput figure. KV-cache panels: per block GET or PUT. Whiskers: minimum to maximum."),
    "sweep": fig_sweep,
    "concurrency": fig_concurrency,
    "cpu-split": fig_cpu_split,
    "coordinated-omission": fig_coordinated,
    "capacity": fig_capacity,
    "transport": fig_transport,
    "engine": fig_engine,
}


def update_docs(blocks):
    pat = re.compile(r"(<!-- BEGIN GENERATED:(?P<name>[\w-]+) -->\n)(.*?)(<!-- END GENERATED:(?P=name) -->)", re.S)
    changed = []
    for md in sorted(ROOT.rglob("*.md")):
        if "/build/" in str(md) or "/data/" in str(md):
            continue
        text = md.read_text()

        def repl(m):
            name = m.group("name")
            if name not in blocks:
                return m.group(0)
            return m.group(1) + blocks[name] + m.group(4)

        new = pat.sub(repl, text)
        if new != text:
            md.write_text(new)
            changed.append(str(md.relative_to(ROOT)))
    return changed


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run_dir")
    ap.add_argument("--out-figures", default=str(ROOT / "benchmarks" / "figures"))
    ap.add_argument("--out-generated", default=str(ROOT / "benchmarks" / "generated"))
    ap.add_argument("--update-docs", action="store_true")
    ap.add_argument("--preview-png", help="also write PNG renderings of the figures to this directory")
    args = ap.parse_args()
    global PREVIEW_DIR
    if args.preview_png:
        PREVIEW_DIR = pathlib.Path(args.preview_png)
        PREVIEW_DIR.mkdir(parents=True, exist_ok=True)
    summary = bench_data.summarize(args.run_dir)
    digest = bench_data.summary_digest(summary)
    figdir = pathlib.Path(args.out_figures)
    gendir = pathlib.Path(args.out_generated)
    figdir.mkdir(parents=True, exist_ok=True)
    gendir.mkdir(parents=True, exist_ok=True)
    (gendir / "summary.json").write_text(bench_data.canonical(summary))
    (gendir / "summary.sha256").write_text(digest + "\n")
    for name, fn in FIGURES.items():
        for theme in ("light", "dark"):
            fn(summary, theme, figdir / ("%s%s.svg" % (name, THEMES[theme]["suffix"])), digest)
    blocks = tables(summary, digest)
    for name, text in blocks.items():
        (gendir / (name + ".md")).write_text(text)
    if args.update_docs:
        for f in update_docs(blocks):
            print("updated", f)
    print("summary digest", digest)


if __name__ == "__main__":
    main()
