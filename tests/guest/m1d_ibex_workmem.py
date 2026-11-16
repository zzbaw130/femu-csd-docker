#!/usr/bin/env python3
"""Measure how device work memory affects Ibex on cold SF8 Q18."""

import argparse
import csv
from pathlib import Path

from m1c_csd_spill_handoff import cold_q18, ibex_ranges


SCRIPT = Path(__file__).resolve()
TEST_NAME = Path(__file__).stem
PROJECT_ROOT = SCRIPT.parents[1] if SCRIPT.parent.name == "tests" else SCRIPT.parents[2]
RESULTS = PROJECT_ROOT / "results" / TEST_NAME
WORKMEM_MIB = (128, 256, 512, 896)
MIB = 1024**2
FIELDS = (
    "device_workmem_mib", "device_peak_mem_mib", "bypass_pct",
    "device_to_host_bytes", "end_to_end_ms",
)


def test(diagnose=False, workmem=None):
    """Measure one independently cold Q18 execution per memory budget."""
    rows = []
    budgets = (workmem,) if workmem is not None else WORKMEM_MIB
    if diagnose and workmem is None:
        budgets = (WORKMEM_MIB[0], WORKMEM_MIB[-1])
    configurations = [("host-only", budgets[0]), *(("ibex", mib) for mib in budgets)]
    expected_input = None
    for mode, workmem_mib, plan, elapsed_ms in cold_q18(configurations):
        if mode == "host-only":
            # cold_q18 checks each split against this native result.
            print(f"Host reference: {elapsed_ms:.3f} ms", flush=True)
            continue
        nodes = [plan]
        for node in nodes:
            nodes.extend(node.get("Plans", []))
        device = [node for node in nodes if "CSD Host Result Bytes" in node]
        if len(device) != 1 or device[0]["Execution Path"] != "ibex":
            raise RuntimeError(f"{workmem_mib} MiB did not execute the Ibex range plan")
        scan = device[0]
        if scan["CSD Query Memory Limit"] != workmem_mib * MIB:
            raise RuntimeError(f"{workmem_mib} MiB was not the effective device budget")
        if not scan["CSD Result Delivery"].startswith("SNIA callbacks"):
            raise RuntimeError(f"{workmem_mib} MiB did not use callback delivery")
        if scan["CSD Spill Read"] or scan["CSD Spill Write"]:
            raise RuntimeError(f"{workmem_mib} MiB unexpectedly spilled on the device")

        # The HOST branch scans its own key range. Only final device groups
        # contribute to return bytes. The two input counts define bypass.
        cutoff, device_rows, host_rows = ibex_ranges(scan)
        scanned = device_rows + host_rows
        if expected_input is None:
            expected_input = scanned
        if not 0 < device_rows < scanned or scanned != expected_input:
            raise RuntimeError(f"{workmem_mib} MiB reported inconsistent partition row counts")
        peak = scan["CSD Workmem Peak"]
        if not 0 < peak <= workmem_mib * MIB:
            raise RuntimeError(f"{workmem_mib} MiB reported an invalid memory peak")
        row = dict(
            device_workmem_mib=workmem_mib,
            device_peak_mem_mib=f"{peak / MIB:.6f}",
            bypass_pct=f"{100 * host_rows / scanned:.6f}",
            device_to_host_bytes=scan["CSD Host Result Bytes"],
            end_to_end_ms=elapsed_ms,
        )
        rows.append(row)
        print(f"{workmem_mib} MiB: key < {cutoff} on device, device_rows={device_rows}, "
              f"host_rows={host_rows}, device_groups={scan['CSD Returned Rows']}, "
              f"workmem_peak_bytes={peak}", flush=True)
        print(row, flush=True)
        if diagnose:
            # Pipeline wall includes delivery and overlaps PG consumption.
            # The scan row counts show the work retained on each side.
            print(f"Ibex output rows: {scan['CSD Returned Rows']}", flush=True)
            for name, value in scan.items():
                if name.startswith(("CSD Pipeline ", "CSD Ibex ")) or name in (
                    "CSD CSF CPU", "CSD Result Read Wall", "CSD Host Idle Sleep",
                    "CSD Aggregate Sink Coordinator CPU", "CSD Delivery Full Wait Sum",
                ):
                    print(f"{name} = {value}", flush=True)
            for node in nodes:
                if node["Node Type"] in ("Seq Scan", "Hash Join", "Aggregate"):
                    relation = node.get("Relation Name", "")
                    print(f"PG {node['Node Type']} {relation}: "
                          f"rows={node['Actual Rows']}, loops={node['Actual Loops']}",
                          flush=True)

    if diagnose and workmem is None:
        return
    RESULTS.mkdir(parents=True, exist_ok=True)
    with (RESULTS / f"{TEST_NAME}.csv").open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(rows)


def plot():
    """Read the measured budgets and write their SVG and PDF."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    matplotlib.rcParams.update({"svg.fonttype": "none", "pdf.fonttype": 42})

    with (RESULTS / f"{TEST_NAME}.csv").open(newline="") as source:
        rows = list(csv.DictReader(source))
    budgets = [int(row["device_workmem_mib"]) for row in rows]
    if not budgets or tuple(budgets) != tuple(mib for mib in WORKMEM_MIB if mib in budgets):
        raise ValueError("Expected measured device work memory budgets in increasing order")

    fig, axes = plt.subplots(2, 2, figsize=(8.4, 5.2), layout="constrained", sharex=True)
    panels = (
        ("device_peak_mem_mib", 1, "Device memory peak (MiB)"),
        ("bypass_pct", 1, "Bypass rate (%)"),
        ("device_to_host_bytes", MIB, "Device-to-host data (MiB)"),
        ("end_to_end_ms", 1000, "End-to-end latency (s)"),
    )
    for ax, (field, scale, label) in zip(axes.flat, panels):
        values = [float(row[field]) / scale for row in rows]
        ax.plot(budgets, values, color="#406B93", marker="o", linewidth=1.6)
        ax.set_ylabel(label)
        ax.set_ylim(0, max(values) * 1.25)
        ax.set_xticks(budgets)
        ax.spines[["top", "right"]].set_visible(False)
        ax.grid(axis="y", color="#dce3e9", linewidth=0.6)
        for budget, value in zip(budgets, values):
            ax.annotate(f"{value:.2f}", (budget, value), xytext=(0, 6),
                        textcoords="offset points", ha="center", fontsize=8)
    for ax in axes[-1]:
        ax.set_xlabel("Device work memory (MiB)")
    fig.suptitle("Ibex · cold SF8 Q18")
    for extension in ("svg", "pdf"):
        fig.savefig(RESULTS / f"{TEST_NAME}.{extension}", format=extension)
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("test", "plot"))
    parser.add_argument("--workmem", type=int, choices=WORKMEM_MIB,
                        help="Test one device work memory budget in MiB.")
    parser.add_argument("--diagnose", action="store_true",
                        help="Print stage profiles. With no --workmem, diagnose both endpoints.")
    args = parser.parse_args()
    if args.action == "test":
        test(args.diagnose, args.workmem)
    else:
        if args.diagnose or args.workmem is not None:
            parser.error("--diagnose and --workmem are only available for test")
        plot()


if __name__ == "__main__":
    main()
