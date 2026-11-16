#!/usr/bin/env python3
"""Compare cold SF8 Q18 latency across Ibex bypass rates and native Host.

Each CSV row pairs one Ibex memory tier with an independent native Host run
of the full query.

CSV fields
    device_tuples         Inner lineitem rows scanned on the device in Ibex.
    host_tuples           Inner lineitem rows scanned on the host in Ibex.
    bypass_pct            Host share of those rows, as a percentage.
                          Calculated as 100 * host_tuples /
                          (device_tuples + host_tuples).
    device_peak_mem_mib   Peak device work memory reported for Ibex (MiB).
    latency_s             End-to-end latency of cold Q18 in Ibex (s).
    host_lat              End-to-end latency of the paired cold native Host
                          Q18 run (s).
"""

import argparse
import csv
from pathlib import Path
import sys


TEST_NAME = Path(__file__).stem
PROJECT_ROOT = Path(__file__).resolve().parents[3]
RESULTS = PROJECT_ROOT / "results" / "m1"
TABLE_MIB = (512, 256, 128, 64)
RESERVE_MIB = 16
MIB = 1024**2
IBEX_FIELDS = ("device_tuples", "host_tuples", "bypass_pct", "device_peak_mem_mib", "latency_s")
FIELDS = (*IBEX_FIELDS, "host_lat")


def test(host_only=False):
    """Run each Ibex split and native Host cold and write the verified samples."""
    guest_tests = PROJECT_ROOT / "tests"
    if (guest_tests / "guest").is_dir():
        guest_tests /= "guest"
    sys.path.insert(0, str(guest_tests))
    from m1c_csd_spill_handoff import cold_q18, ibex_ranges

    RESULTS.mkdir(parents=True, exist_ok=True)
    csv_path = RESULTS / f"{TEST_NAME}.csv"
    rows = []
    total_tuples = None
    budgets = [table_mib + RESERVE_MIB for table_mib in TABLE_MIB]
    if host_only:
        with csv_path.open(newline="") as source:
            reader = csv.DictReader(source)
            if tuple(reader.fieldnames or ()) not in (IBEX_FIELDS, FIELDS):
                raise ValueError(f"{csv_path} has unexpected columns")
            rows = [row for row in reader if int(row["device_tuples"]) > 0]
        rows.sort(key=lambda row: float(row["device_peak_mem_mib"]), reverse=True)
        if len(rows) != len(TABLE_MIB) or any(
            not table <= float(row["device_peak_mem_mib"]) <= budget
            for row, table, budget in zip(rows, TABLE_MIB, budgets)
        ):
            raise ValueError(f"{csv_path} must contain one Ibex sample for each configured budget")
    modes = ("host-only",) if host_only else ("ibex", "host-only")
    configurations = [(mode, budget) for budget in budgets for mode in modes]
    host_latencies = {}
    for mode, workmem_mib, plan, elapsed_ms in cold_q18(configurations):
        nodes = [plan]
        for node in nodes:
            nodes.extend(node.get("Plans", []))
        if mode == "host-only":
            if any(node["Node Type"] == "Custom Scan" for node in nodes):
                raise RuntimeError("Host did not run a native PostgreSQL plan")
            # Pair an independent full-Q18 Host measurement with this Ibex budget.
            # The device budget labels the sample and does not limit native PG memory.
            host_latencies[workmem_mib] = f"{elapsed_ms / 1000:.6f}"
            print(f"Host paired with {workmem_mib} MiB CSD budget: "
                  f"{host_latencies[workmem_mib]} s", flush=True)
            continue

        device = [node for node in nodes if node.get("Execution Path") == "ibex"]
        if len(device) != 1:
            raise RuntimeError(f"{workmem_mib} MiB did not run the Ibex split")
        scan = device[0]
        table_mib = workmem_mib - RESERVE_MIB
        if scan["CSD Query Memory Limit"] != workmem_mib * MIB:
            raise RuntimeError(f"{workmem_mib} MiB was not the effective device budget")

        # The compact integer SUM table uses 64 bytes per bucket at 7/8 occupancy.
        # Check the exact rank used for the split and that the full table fit.
        slots = table_mib * MIB // 64
        groups = scan.get("CSD Ibex Ranked Groups")
        if groups != slots * 7 // 8 or scan["CSD Spill Read"] or scan["CSD Spill Write"]:
            raise RuntimeError(f"{table_mib} MiB did not hold its ranked groups without spill")
        peak_bytes = scan["CSD Workmem Peak"]
        if not table_mib * MIB <= peak_bytes <= workmem_mib * MIB:
            raise RuntimeError(f"{table_mib} MiB reported an unexpected device memory peak")

        # Input counts come from the complementary inner scans, before SUM or HAVING.
        cutoff, device_tuples, host_tuples = ibex_ranges(scan)
        scanned = device_tuples + host_tuples
        if total_tuples is None:
            total_tuples = scanned
        if device_tuples <= 0 or host_tuples <= 0 or scanned != total_tuples:
            raise RuntimeError(f"{table_mib} MiB reported inconsistent tuple counts")
        row = dict(
            device_tuples=device_tuples,
            host_tuples=host_tuples,
            bypass_pct=f"{100 * host_tuples / scanned:.6f}",
            device_peak_mem_mib=f"{peak_bytes / MIB:.6f}",
            latency_s=f"{elapsed_ms / 1000:.6f}",
        )
        rows.append(row)
        print(f"{table_mib} MiB table, {workmem_mib} MiB device budget, "
              f"{slots} buckets, {groups} groups, key < {cutoff}: {row}", flush=True)

    for row, budget in zip(rows, budgets):
        row["host_lat"] = host_latencies[budget]
    with csv_path.open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(rows)


def plot():
    """Plot measured latency against bypass rate using the saved CSV."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    matplotlib.rcParams.update({
        "font.family": "DejaVu Sans",
        "svg.fonttype": "none",
        "pdf.fonttype": 42,
    })

    with (RESULTS / f"{TEST_NAME}.csv").open(newline="") as source:
        rows = list(csv.DictReader(source))
    ibex = sorted(
        (row for row in rows if int(row["device_tuples"]) > 0),
        key=lambda row: float(row["bypass_pct"]),
    )
    bypass = [float(row["bypass_pct"]) for row in ibex]
    latency = [float(row["latency_s"]) for row in ibex]
    host_latency = [float(row["host_lat"]) for row in ibex if "host_lat" in row]

    fig, ax = plt.subplots(figsize=(6.2, 3.7), layout="constrained")
    ax.plot(bypass, latency, color="#406B93", marker="o", linewidth=1.8, label="Ibex")
    if host_latency:
        ax.plot(bypass, host_latency, color="#3E8257",
                marker="s", linewidth=1.8, label="Host", zorder=1.5)
    else:
        for row in rows:
            if int(row["device_tuples"]) == 0:
                ax.axhline(float(row["latency_s"]), color="#3E8257",
                           linewidth=1.8, label="Host", zorder=1.5)
                break
    ax.legend(frameon=False, loc="lower right")
    ax.set_xlabel("Bypass tuples(% of all tuples)")
    ax.set_ylabel("End-to-end latency (s)")
    ax.spines[["top", "right"]].set_visible(False)
    ax.grid(axis="y", color="#dce3e9", linewidth=0.7)
    ax.margins(y=0.22)
    for index, (x, y) in enumerate(zip(bypass, latency)):
        label_below = bool(host_latency) and y <= host_latency[index]
        last_offset = 5 if host_latency and index == len(bypass) - 1 else 0
        height = 9 if last_offset else 7
        ax.annotate(f"{y:.2f}", (x, y), xytext=(last_offset, -height if label_below else height),
                    textcoords="offset points", ha="center",
                    va="top" if label_below else "bottom", fontsize=9)
        if host_latency:
            host_y = host_latency[index]
            ax.annotate(f"{host_y:.2f}", (x, host_y),
                        xytext=(-last_offset, height if label_below else -height),
                        textcoords="offset points", ha="center",
                        va="bottom" if label_below else "top", fontsize=9)
    for extension in ("svg", "pdf"):
        fig.savefig(RESULTS / f"{TEST_NAME}.{extension}", format=extension)
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("test", "plot"))
    parser.add_argument("--host-only", action="store_true",
                        help="Cold-test Host once per Ibex budget and keep the saved Ibex samples.")
    args = parser.parse_args()
    if args.action == "test":
        test(host_only=args.host_only)
    else:
        if args.host_only:
            parser.error("--host-only is only available for test")
        plot()


if __name__ == "__main__":
    main()
