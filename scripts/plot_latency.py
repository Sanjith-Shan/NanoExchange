#!/usr/bin/env python3
"""Render benchmark charts for NanoExchange from Google Benchmark JSON.

The input is a Google Benchmark results file. The top level key benchmarks holds
a list. Each entry carries a name, a real_time or cpu_time value, a time_unit and
for aggregate rows an aggregate_name in the set mean, median, stddev. Some rows
also carry items_per_second. Benchmark names follow the pattern Family/arg1/arg2
where arg values are sizes or level counts.

The file is produced by a separate benchmarking process and may be missing or
partial. This script handles that gracefully, prints a clear message and still
exits 0 so it can be re run once the real results land.

Charts written into the output directory.
    pool_allocators.png       grouped median latency across the four pool strategies
    queue_spsc_vs_mutex.png   SPSC versus mutex queue median latency
    price_level_containers.png grouped median latency per level count for three containers
    throughput_scaling.png    items per second versus swept size on a log x axis
    orderbook_latency.png     median latency with a stddev band per order book operation
"""

import argparse
import json
import os
import sys
from collections import defaultdict

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np


# A small consistent palette. These read clearly in print and on screen and stay
# distinct for viewers with common color vision deficiencies.
PALETTE = ["#2f6db5", "#e07b39", "#3f9d5a", "#b0413e", "#7d5ba6", "#4c4c4c"]
GRID_KW = dict(axis="y", color="#d9d9d9", linewidth=0.8, zorder=0)


def apply_base_style():
    """Set a clean shared look for every figure."""
    plt.rcParams.update({
        "figure.facecolor": "white",
        "axes.facecolor": "white",
        "axes.edgecolor": "#666666",
        "axes.linewidth": 0.8,
        "axes.grid": False,
        "axes.titlesize": 13,
        "axes.titleweight": "bold",
        "axes.labelsize": 11,
        "xtick.labelsize": 10,
        "ytick.labelsize": 10,
        "legend.fontsize": 10,
        "legend.frameon": False,
        "font.family": "DejaVu Sans",
    })


# Canonical conversion of Google Benchmark time units to nanoseconds so charts
# share one latency scale.
UNIT_TO_NS = {"ns": 1.0, "us": 1000.0, "ms": 1000000.0, "s": 1000000000.0}


def load_benchmarks(path):
    """Load and parse the GBench JSON.

    Returns a list of normalized record dicts or None when the file is missing or
    cannot be parsed. Each record carries family, args, aggregate, time_ns and an
    optional items_per_second.
    """
    if not os.path.exists(path):
        print("Benchmark file not found at {}.".format(path))
        print("Run the benchmark process to produce it, then re run this script.")
        return None

    try:
        with open(path, "r") as fh:
            data = json.load(fh)
    except (ValueError, OSError) as exc:
        print("Could not read benchmark JSON at {}. Reason {}.".format(path, exc))
        return None

    raw = data.get("benchmarks")
    if not raw:
        print("No benchmarks list found in {}.".format(path))
        return None

    records = []
    for entry in raw:
        name = entry.get("name")
        if not name:
            continue

        # Aggregate rows carry aggregate_name. Non aggregate iteration rows are
        # skipped because the charts summarize mean, median and stddev only.
        aggregate = entry.get("aggregate_name")
        if aggregate is None:
            continue

        # Strip a trailing aggregate suffix such as _median from the name before
        # splitting so rows for the same case group together.
        base = name
        for suffix in ("_mean", "_median", "_stddev", "_cv"):
            if base.endswith(suffix):
                base = base[: -len(suffix)]
                break

        parts = base.split("/")
        family = parts[0]
        args = parts[1:]

        unit = entry.get("time_unit", "ns")
        scale = UNIT_TO_NS.get(unit, 1.0)
        # Prefer real_time. Fall back to cpu_time when real_time is absent.
        t = entry.get("real_time")
        if t is None:
            t = entry.get("cpu_time")
        time_ns = None if t is None else float(t) * scale

        records.append({
            "family": family,
            "args": args,
            "aggregate": aggregate,
            "time_ns": time_ns,
            "items_per_second": entry.get("items_per_second"),
        })

    if not records:
        print("The benchmark file held no aggregate rows to chart.")
        return None
    return records


def index_by_case(records):
    """Group records by family plus arg tuple, keyed to their aggregate stat.

    Returns a dict mapping (family, args_tuple) to a dict of aggregate name to
    the record. This lets a chart pull the median and stddev of one case cleanly.
    """
    cases = defaultdict(dict)
    for rec in records:
        key = (rec["family"], tuple(rec["args"]))
        cases[key][rec["aggregate"]] = rec
    return cases


def families_present(records):
    return {rec["family"] for rec in records}


def _median_ns(case):
    med = case.get("median")
    if med is not None and med["time_ns"] is not None:
        return med["time_ns"]
    mean = case.get("mean")
    if mean is not None and mean["time_ns"] is not None:
        return mean["time_ns"]
    return None


def _stddev_ns(case):
    sd = case.get("stddev")
    if sd is not None and sd["time_ns"] is not None:
        return sd["time_ns"]
    return 0.0


def match_family(present, needle):
    """Return the actual family name that contains needle, or None.

    Benchmark suites vary the exact family label. Matching on a substring keeps
    the charts working across small naming differences.
    """
    needle = needle.lower()
    for fam in present:
        if needle in fam.lower():
            return fam
    return None


def save(fig, outdir, filename):
    path = os.path.join(outdir, filename)
    fig.savefig(path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print("wrote {}".format(path))
    return path


def chart_allocators(cases, present, outdir, filename):
    """Grouped bar chart of median latency across the memory pool allocators.

    Each allocator is its own benchmark family named BM_Alloc_<strategy> with a
    single aggregate case. This collects every such family into one bar chart and
    sorts fastest first so the allocator shootout reads at a glance.
    """
    fams = sorted(f for f in present if f.startswith("BM_Alloc_"))
    if not fams:
        print("skip {}. no BM_Alloc_ families present.".format(filename))
        return False

    items = []
    for fam in fams:
        case = cases.get((fam, ()))
        if case is None:
            # Some allocator families may carry an arg. Fall back to any case
            # that shares the family name.
            for (f, args), c in cases.items():
                if f == fam:
                    case = c
                    break
        if case is None:
            continue
        med = _median_ns(case)
        if med is None:
            continue
        label = fam[len("BM_Alloc_"):]
        items.append((label, med, _stddev_ns(case)))

    if not items:
        print("skip {}. no median rows for allocators.".format(filename))
        return False

    items.sort(key=lambda x: x[1])
    labels = [i[0] for i in items]
    medians = [i[1] for i in items]
    errs = [i[2] for i in items]

    fig, ax = plt.subplots(figsize=(max(5, len(labels) * 1.2), 4.4))
    x = np.arange(len(labels))
    colors = [PALETTE[i % len(PALETTE)] for i in range(len(labels))]
    ax.set_axisbelow(True)
    ax.grid(**GRID_KW)
    ax.bar(x, medians, yerr=errs, capsize=4, color=colors, width=0.62,
           zorder=3, error_kw=dict(ecolor="#444444", lw=1.0))
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=20, ha="right")
    ax.set_ylabel("median latency (ns)")
    ax.set_title("Memory pool allocator latency. lower is better")
    for spine in ("top", "right"):
        ax.spines[spine].set_visible(False)
    return save(fig, outdir, filename)


def chart_queues(cases, present, outdir, filename):
    """Grouped bars comparing the SPSC lock free queue against the mutex queues.

    Family names follow BM_SPSC_<scenario>_<impl> where scenario is Single or
    TwoThread and impl is Nano, MutexQueue or MutexRing. Bars are grouped by
    scenario with one bar per implementation showing median latency in ns.
    """
    prefix = "BM_SPSC_"
    scenarios = []
    impls = []
    data = {}  # (scenario, impl) to (median, stddev)
    for fam in present:
        if not fam.startswith(prefix):
            continue
        rest = fam[len(prefix):]
        parts = rest.split("_", 1)
        if len(parts) != 2:
            continue
        scenario, impl = parts
        # Find the case for this family regardless of any trailing timing arg
        # such as real_time on the two thread manual timing benchmarks.
        case = None
        for (f, args), c in cases.items():
            if f == fam:
                case = c
                break
        if case is None:
            continue
        med = _median_ns(case)
        if med is None:
            continue
        if scenario not in scenarios:
            scenarios.append(scenario)
        if impl not in impls:
            impls.append(impl)
        data[(scenario, impl)] = (med, _stddev_ns(case))

    if not data:
        print("skip {}. no BM_SPSC_ families present.".format(filename))
        return False

    scenarios.sort()
    impls.sort()
    # The Single scenario reports per operation latency in single digit ns while
    # the TwoThread scenario reports whole run wall time in millions of ns. These
    # live on very different scales so each scenario gets its own subplot with an
    # independent y axis rather than being crushed onto one shared axis.
    fig, axes = plt.subplots(1, len(scenarios),
                             figsize=(max(4.2, len(scenarios) * 3.4), 4.4))
    if len(scenarios) == 1:
        axes = [axes]
    x = np.arange(len(impls))
    colors = [PALETTE[i % len(PALETTE)] for i in range(len(impls))]
    for ax, scenario in zip(axes, scenarios):
        ax.set_axisbelow(True)
        ax.grid(**GRID_KW)
        vals = [data.get((scenario, impl), (0.0, 0.0))[0] for impl in impls]
        errs = [data.get((scenario, impl), (0.0, 0.0))[1] for impl in impls]
        ax.bar(x, vals, width=0.62, yerr=errs, capsize=4, color=colors,
               zorder=3, error_kw=dict(ecolor="#444444", lw=1.0))
        ax.set_xticks(x)
        ax.set_xticklabels(impls, rotation=20, ha="right")
        ax.set_title(scenario)
        for spine in ("top", "right"):
            ax.spines[spine].set_visible(False)
    axes[0].set_ylabel("median latency (ns)")
    fig.suptitle("SPSC queue latency. lock free versus mutex",
                 y=1.02, fontsize=13, fontweight="bold")
    return save(fig, outdir, filename)


def chart_containers(cases, present, outdir, filename):
    """Price level container shootout across the three workload families.

    Names follow workload/container/levels where workload is insert_heavy, mixed
    or read_heavy, container is Map, Array or Vector and levels is the level
    count. One subplot per workload. x is the level count on a log scale and each
    container is one line of throughput in items per second.
    """
    workloads = [w for w in ("insert_heavy", "mixed", "read_heavy") if w in present]
    if not workloads:
        print("skip {}. no container workload families present.".format(filename))
        return False

    # Collect throughput per workload, container and level count.
    series = {w: defaultdict(list) for w in workloads}
    for (fam, args), case in cases.items():
        if fam not in series or len(args) < 2:
            continue
        container = args[0]
        try:
            levels = int(args[1])
        except ValueError:
            continue
        med = case.get("median") or case.get("mean")
        if med is None:
            continue
        ips = med.get("items_per_second")
        if ips is not None:
            value = float(ips)
        elif med["time_ns"]:
            value = 1e9 / med["time_ns"]
        else:
            continue
        series[fam][container].append((levels, value))

    if not any(series[w] for w in workloads):
        print("skip {}. no plottable container rows.".format(filename))
        return False

    containers = sorted({c for w in workloads for c in series[w]})
    color_of = {c: PALETTE[i % len(PALETTE)] for i, c in enumerate(containers)}

    fig, axes = plt.subplots(1, len(workloads),
                             figsize=(5.2 * len(workloads), 4.4), sharey=False)
    if len(workloads) == 1:
        axes = [axes]
    for ax, workload in zip(axes, workloads):
        ax.set_axisbelow(True)
        ax.grid(color="#e2e2e2", linewidth=0.8)
        for container in containers:
            pts = sorted(series[workload].get(container, []))
            if not pts:
                continue
            xs = [p[0] for p in pts]
            ys = [p[1] for p in pts]
            ax.plot(xs, ys, marker="o", markersize=5, linewidth=1.8,
                    color=color_of[container], label=container)
        ax.set_xscale("log")
        ax.set_xlabel("price levels")
        ax.set_title(workload.replace("_", " "))
        for spine in ("top", "right"):
            ax.spines[spine].set_visible(False)
    axes[0].set_ylabel("throughput (items per second)")
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, title="container", loc="upper center",
               ncol=len(containers), frameon=False,
               bbox_to_anchor=(0.5, 1.06))
    fig.suptitle("Price level container throughput shootout", y=1.12,
                 fontsize=13, fontweight="bold")
    return save(fig, outdir, filename)


def chart_throughput_scaling(records, cases, present, outdir, filename):
    """Line chart of throughput versus swept size, one line per family, log x.

    Throughput uses items_per_second when present. Otherwise it falls back to the
    inverse of the median time so a scaling shape still appears.
    """
    # Consider only families whose first arg is a numeric size and that sweep at
    # least two distinct sizes.
    series = defaultdict(list)
    for (fam, args), case in cases.items():
        if not args:
            continue
        try:
            size = int(args[0])
        except ValueError:
            continue
        med = case.get("median") or case.get("mean")
        if med is None:
            continue
        ips = med.get("items_per_second")
        if ips is not None:
            throughput = float(ips)
        elif med["time_ns"]:
            throughput = 1e9 / med["time_ns"]
        else:
            continue
        series[fam].append((size, throughput))

    plottable = {f: pts for f, pts in series.items() if len({p[0] for p in pts}) >= 2}
    if not plottable:
        print("skip {}. no family sweeps at least two sizes.".format(filename))
        return False

    fig, ax = plt.subplots(figsize=(6.4, 4.4))
    ax.set_axisbelow(True)
    ax.grid(color="#e2e2e2", linewidth=0.8)
    for i, (fam, pts) in enumerate(sorted(plottable.items())):
        pts.sort()
        xs = [p[0] for p in pts]
        ys = [p[1] for p in pts]
        ax.plot(xs, ys, marker="o", markersize=5, linewidth=1.8,
                color=PALETTE[i % len(PALETTE)], label=fam)
    ax.set_xscale("log")
    ax.set_xlabel("swept size argument")
    ax.set_ylabel("throughput (items per second)")
    ax.set_title("Throughput scaling")
    ax.legend()
    for spine in ("top", "right"):
        ax.spines[spine].set_visible(False)
    return save(fig, outdir, filename)


ORDERBOOK_OPS = [
    "BM_AddLimit",
    "BM_AddAndMatch",
    "BM_Cancel",
    "BM_Modify",
    "BM_BestBidAsk",
    "BM_Snapshot",
]


def chart_orderbook_distribution(cases, present, outdir, filename):
    """Median latency versus book density per order book operation.

    Each operation is its own family swept over a density argument. This plots
    median latency in ns against density on a log x axis with one line per
    operation and a shaded stddev band where the stddev aggregate is available.

    Google Benchmark does not emit raw per iteration samples so a true CDF cannot
    be drawn. The stddev band is a symmetric approximation of the spread around
    the median rather than a real percentile envelope.
    """
    ops = [op for op in ORDERBOOK_OPS if op in present]
    if not ops:
        print("skip {}. no order book operation families present.".format(filename))
        return False

    # Collect density, median and stddev per operation.
    series = {}
    for op in ops:
        pts = []
        for (fam, args), case in cases.items():
            if fam != op or not args:
                continue
            try:
                density = int(args[0])
            except ValueError:
                continue
            med = _median_ns(case)
            if med is None:
                continue
            pts.append((density, med, _stddev_ns(case)))
        if pts:
            pts.sort()
            series[op] = pts

    if not series:
        print("skip {}. no order book rows with a density sweep.".format(filename))
        return False

    fig, ax = plt.subplots(figsize=(7.0, 4.6))
    ax.set_axisbelow(True)
    ax.grid(color="#e2e2e2", linewidth=0.8)
    for i, op in enumerate(ops):
        pts = series.get(op)
        if not pts:
            continue
        color = PALETTE[i % len(PALETTE)]
        xs = np.array([p[0] for p in pts])
        med = np.array([p[1] for p in pts])
        err = np.array([p[2] for p in pts])
        label = op[len("BM_"):] if op.startswith("BM_") else op
        ax.fill_between(xs, med - err, med + err, color=color, alpha=0.15,
                        zorder=2)
        ax.plot(xs, med, marker="o", markersize=5, linewidth=1.8, color=color,
                label=label, zorder=3)
    ax.set_xscale("log")
    ax.set_xlabel("book density (resting orders)")
    ax.set_ylabel("median latency (ns)")
    ax.set_title("Order book operation latency versus density")
    ax.legend(title="operation", ncol=2)
    for spine in ("top", "right"):
        ax.spines[spine].set_visible(False)
    return save(fig, outdir, filename)


def run(input_path, outdir):
    apply_base_style()
    records = load_benchmarks(input_path)
    if records is None:
        print("No charts were produced. This is expected when results are absent.")
        return 0

    os.makedirs(outdir, exist_ok=True)
    cases = index_by_case(records)
    present = families_present(records)
    print("families found {}".format(sorted(present)))

    written = 0
    # (a) grouped latency charts.
    if chart_allocators(cases, present, outdir, "pool_allocators.png"):
        written += 1
    if chart_queues(cases, present, outdir, "queue_spsc_vs_mutex.png"):
        written += 1
    if chart_containers(cases, present, outdir, "price_level_containers.png"):
        written += 1
    # (b) throughput scaling.
    if chart_throughput_scaling(records, cases, present, outdir,
                                "throughput_scaling.png"):
        written += 1
    # (c) order book distribution style chart.
    if chart_orderbook_distribution(cases, present, outdir,
                                    "orderbook_latency.png"):
        written += 1

    if written == 0:
        print("No matching benchmark families were found so no charts were drawn.")
    else:
        print("done. {} chart file(s) written to {}.".format(written, outdir))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Plot NanoExchange benchmark charts from Google Benchmark JSON."
    )
    parser.add_argument("--input", default="results/benchmarks.json",
                        help="path to the Google Benchmark JSON results")
    parser.add_argument("--outdir", default="results",
                        help="directory to write PNG charts into")
    args = parser.parse_args(argv)
    return run(args.input, args.outdir)


if __name__ == "__main__":
    sys.exit(main())
