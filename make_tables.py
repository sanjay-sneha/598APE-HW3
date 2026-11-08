#!/usr/bin/env python3
"""Build the report's tables and runtime graph from the saved timing and perf stat files.

Usage (in the repo folder):
    python3 make_tables.py

Writes:
    summary_table.md / summary_table.tex   - runtime at N=1001 for every version, with speedups
    opt_tables.md / opt_tables.tex         - before/after perf stat tables, only for the versions in TABLE_FOR
    runtime_graph.pdf / runtime_graph.png  - line graph of the N=1001 runtimes (needs matplotlib)

Edit VERSIONS below if a file name is different, and TABLE_FOR to pick which optimizations get a table.
"""
import os
import re
import sys

# (label in the tables, short label for the graph, time file at N=1001, perf stat file at N=401)
VERSIONS = [
    ("Baseline", "Base", "baseline_time.txt", "baseline_stat.txt"),
    ("Opt1: shape bounds", "1", "opt1_precompute_shape_bounds_time.txt", "opt1_precompute_shape_bounds_stat.txt"),
    ("Opt2: branchless topple", "2", "opt2_branchless_topple_time.txt", "opt2_branchless_topple_stat.txt"),
    ("Opt3: flat padded array", "3", "opt3_flat_padded_array_time.txt", "opt3_flat_padded_array_stat.txt"),
    ("Opt4: two-pass update", "4", "opt4_two_pass_update_time.txt", "opt4_two_pass_update_stat.txt"),
    ("Opt5: OpenMP", "5", "opt5_openmp_time.txt", "opt5_openmp_stat.txt"),
    ("Opt6: symmetry", "6", "opt6_symmetry_time.txt", "opt6_symmetry_stat.txt"),
    ("Opt7: int16", "7", "opt7_int16_time.txt", "opt7_int16_stat.txt"),
    ("Opt8: -march=native", "8", "opt8_march_native_time.txt", "opt8_march_native_stat.txt"),
]

# Which optimizations get a before/after counter table (numbers refer to OptN).
# The counters tell a story the runtime alone doesn't for these:
#   2: branch misses -97% but instructions +33%, so only 1.04x
#   3: instructions -46% but cycles only -7% (now waiting on the in-place dependency)
#   5: instructions and cycles go UP (summed over threads), yet runtime drops 3.5x
TABLE_FOR = [2, 3, 5]

COUNTERS = ["instructions", "cycles", "branches", "branch-misses", "cache-misses"]


def read_time(path):
    """Return the 'real' time in seconds from `time` output, e.g. 'real\t10m57.293s'."""
    with open(path) as f:
        for line in f:
            m = re.match(r"\s*real\s+(\d+)m([\d.]+)s", line)
            if m:
                return int(m.group(1)) * 60 + float(m.group(2))
    raise ValueError(f"no 'real' line in {path}")


def read_stat(path):
    """Return {counter: value, 'elapsed': seconds} from perf stat output."""
    out = {}
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 2 and parts[1] in COUNTERS:
                out[parts[1]] = int(parts[0].replace(",", ""))
            m = re.match(r"\s*([\d.]+) seconds time elapsed", line)
            if m:
                out["elapsed"] = float(m.group(1))
    if "instructions" in out and "cycles" in out:
        out["ipc"] = out["instructions"] / out["cycles"]
    return out


def billions(x):
    return f"{x / 1e9:.1f}B"


def millions(x):
    return f"{x / 1e6:.1f}M"


def tex(s):
    return s.replace("_", r"\_").replace("%", r"\%")


def load():
    rows, missing = [], []
    for label, short, tfile, sfile in VERSIONS:
        row = {"version": label, "short": short}
        if os.path.exists(tfile):
            row["real"] = read_time(tfile)
        else:
            missing.append(tfile)
        if os.path.exists(sfile):
            row.update(read_stat(sfile))
        else:
            missing.append(sfile)
        rows.append(row)
    if missing:
        print("Missing files (edit VERSIONS in make_tables.py if they're named differently):", file=sys.stderr)
        for m in missing:
            print("  " + m, file=sys.stderr)
    return rows


def summary_table(rows):
    timed = [r for r in rows if "real" in r]
    base = timed[0]["real"]
    md = ["| Version | Time (s) | vs. baseline | vs. previous |", "|---|---|---|---|"]
    tx = [r"\begin{table}[t]",
          r"\caption{Runtime at $N=1001$ on the course VM (4 cores). Each optimization builds on the previous one.}",
          r"\label{tab:summary}", r"\small", r"\begin{tabular}{lrrr}", r"\toprule",
          r"Version & Time (s) & vs.\ base & vs.\ prev. \\", r"\midrule"]
    prev = None
    for r in timed:
        vs_base = base / r["real"]
        vs_prev = prev / r["real"] if prev else None
        md.append(f"| {r['version']} | {r['real']:.1f} | {vs_base:.1f}x | "
                  f"{f'{vs_prev:.2f}x' if vs_prev else '-'} |")
        tx.append(f"{tex(r['version'])} & {r['real']:.1f} & {vs_base:.1f}$\\times$ & "
                  f"{f'{vs_prev:.2f}$\\times$' if vs_prev else '--'} \\\\")
        prev = r["real"]
    tx += [r"\bottomrule", r"\end{tabular}", r"\end{table}"]
    with open("summary_table.md", "w") as f:
        f.write("\n".join(md) + "\n")
    with open("summary_table.tex", "w") as f:
        f.write("\n".join(tx) + "\n")


def opt_tables(rows):
    metrics = [
        ("Time at N=1001 (s)", "real", lambda v: f"{v:.1f}"),
        ("Time at N=401 (s)", "elapsed", lambda v: f"{v:.2f}"),
        ("Instructions", "instructions", billions),
        ("Cycles", "cycles", billions),
        ("IPC", "ipc", lambda v: f"{v:.2f}"),
        ("Branches", "branches", billions),
        ("Branch misses", "branch-misses", millions),
        ("Cache misses", "cache-misses", millions),
    ]

    def change(before, after, key):
        if key in ("real", "elapsed"):
            return f"{before / after:.2f}x faster" if after < before else f"{after / before:.2f}x slower"
        pct = (after - before) / before * 100
        return f"{pct:+.1f}%" if abs(pct) < 10 else f"{pct:+.0f}%"

    md, tx = [], []
    for k in TABLE_FOR:
        before, after = rows[k - 1], rows[k]
        threads_note = " Counters are summed over all 4 threads." if k >= 5 else ""
        md += [f"## {after['version']} (vs. {before['version']})", "",
               "| Metric | Before | After | Change |", "|---|---|---|---|"]
        tx += [r"\begin{table}[t]",
               f"\\caption{{{tex(after['version'])} compared with {tex(before['version'])}. "
               f"Counters from \\texttt{{perf stat}} at $N=401$.{threads_note}}}",
               f"\\label{{tab:opt{k}}}", r"\small", r"\begin{tabular}{lrrr}", r"\toprule",
               r"Metric & Before & After & Change \\", r"\midrule"]
        for name, key, fmt in metrics:
            if key in before and key in after:
                ch = change(before[key], after[key], key)
                md.append(f"| {name} | {fmt(before[key])} | {fmt(after[key])} | {ch} |")
                tx.append(f"{name.replace('N=', '$N$=')} & {fmt(before[key])} & {fmt(after[key])} & "
                          f"{tex(ch).replace('x ', '$\\times$ ')} \\\\")
        md.append("")
        tx += [r"\bottomrule", r"\end{tabular}", r"\end{table}", ""]
    with open("opt_tables.md", "w") as f:
        f.write("\n".join(md))
    with open("opt_tables.tex", "w") as f:
        f.write("\n".join(tx))


def runtime_graph(rows):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not installed, skipping runtime_graph (pip install matplotlib)", file=sys.stderr)
        return
    timed = [r for r in rows if "real" in r]
    xs = list(range(len(timed)))
    ys = [r["real"] for r in timed]

    ink, muted, line = "#0b0b0b", "#52514e", "#2a78d6"
    plt.rcParams.update({"font.size": 8, "axes.edgecolor": muted, "axes.labelcolor": ink,
                         "xtick.color": muted, "ytick.color": muted})
    fig, ax = plt.subplots(figsize=(3.33, 2.2))  # one ACM column wide
    ax.plot(xs, ys, color=line, linewidth=2, marker="o", markersize=4, zorder=3)
    ax.set_yscale("log")  # each step is a ratio; a linear axis would flatten everything after Opt5
    from matplotlib.ticker import FixedLocator, NullLocator
    ax.yaxis.set_major_locator(FixedLocator([10, 30, 100, 300, 1000]))
    ax.yaxis.set_minor_locator(NullLocator())
    ax.set_yticklabels(["10", "30", "100", "300", "1000"])
    ax.set_ylim(8, 1000)
    ax.set_xticks(xs)
    ax.set_xticklabels([r["short"] for r in timed])
    ax.set_xlabel("Optimization (cumulative)")
    ax.set_ylabel("Time at N=1001 (s, log scale)")
    ax.grid(axis="y", which="major", color="#e5e4e0", linewidth=0.6, zorder=0)
    ax.spines[["top", "right"]].set_visible(False)

    # label only the start, the biggest single step (OpenMP) and the end
    label_at = {0, len(timed) - 1}
    label_at |= {i for i, r in enumerate(timed) if r["short"] == "5"}
    for i in sorted(label_at):
        ax.annotate(f"{ys[i]:.1f} s", (xs[i], ys[i]), textcoords="offset points",
                    xytext=(6, 4) if i < len(timed) - 1 else (6, -2),
                    ha="left", color=ink, fontsize=7)
    ax.set_xlim(-0.4, len(timed) - 1 + 1.3)  # room for the last label to the right of the last point
    fig.tight_layout()
    fig.savefig("runtime_graph.pdf")
    fig.savefig("runtime_graph.png", dpi=200)


def main():
    rows = load()
    summary_table(rows)
    opt_tables(rows)
    runtime_graph(rows)
    print("Wrote summary_table.md/.tex, opt_tables.md/.tex, runtime_graph.pdf/.png")


if __name__ == "__main__":
    main()