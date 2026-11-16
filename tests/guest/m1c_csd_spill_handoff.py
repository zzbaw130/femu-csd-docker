#!/usr/bin/env python3
"""Compare Host, Filter-only, CSD-only and Ibex on cold SF8 Q18."""

import argparse
import csv
import json
import os
from pathlib import Path
import re
import shlex
import subprocess


SCRIPT = Path(__file__).resolve()
TEST_NAME = Path(__file__).stem
PROJECT_ROOT = SCRIPT.parents[1] if SCRIPT.parent.name == "tests" else SCRIPT.parents[2]
RESULTS = PROJECT_ROOT / "results" / TEST_NAME
PG_BIN = Path("/home/femu/opt/postgres/release/bin")
PG_DATA = Path("/data/tpch_s8")
DEVICE_MEMORY_MIB = 128
MODES = ("host-only", "filter-only", "csd-without-hpcp", "ibex")
FIELDS = (
    "exe_path", "end_to_end_ms", "device_to_host_bytes",
    "spill_read_wait_ms", "spill_write_wait_ms",
)


def cold_q18(configurations):
    """Yield the plan and psql latency for each independently cold Q18 run."""
    query = SCRIPT.with_name("queries") / "sf8_sel0.1" / "18.sql"
    sql = query.read_text()
    # Keep the current PG resource configuration through all cold starts.
    options = shlex.split((PG_DATA / "postmaster.opts").read_text())[1:]
    if options[:2] == ["-D", str(PG_DATA)]:
        options = options[2:]
    pg_ctl = str(PG_BIN / "pg_ctl")
    psql = str(PG_BIN / "psql")
    env = {
        **os.environ,
        "LC_ALL": "C",
        "PGOPTIONS": "-c session_preload_libraries=pg_sina_csd,auto_explain",
    }
    reference = None
    for mode, workmem_mib in configurations:
        label = f"{mode}, CSD workmem {workmem_mib} MiB"
        print(f"Cold SF8 Q18, {label}", flush=True)
        subprocess.run([pg_ctl, "-D", PG_DATA, "-m", "fast", "-w", "stop"], check=True)
        try:
            subprocess.run(["sync"], check=True)
            subprocess.run(["sudo", "-n", "sh", "-c", "echo 3 > /proc/sys/vm/drop_caches"], check=True)
        finally:
            subprocess.run(
                [pg_ctl, "-D", PG_DATA, "-l", PG_DATA / "log", "-w", "start",
                 "-o", shlex.join(options)],
                check=True,
            )
        settings = f"""
SET statement_timeout='600s';
SET pg_sina_csd.exe_path='{mode}';
SET pg_sina_csd.hpcp=off;
SET pg_sina_csd.dcp=off;
SET pg_sina_csd.max_memory={workmem_mib * 1024};
SET pg_sina_csd.profile=on;
SET auto_explain.log_analyze=on;
SET auto_explain.log_timing=off;
SET auto_explain.log_format=json;
SET auto_explain.log_level=notice;
SET auto_explain.log_min_duration=0;
"""
        run = subprocess.run(
            [psql, "-XqAt", "-v", "ON_ERROR_STOP=1", "-d", "tpch_s8"],
            input=settings + sql, text=True, capture_output=True, env=env,
        )
        if run.returncode:
            raise RuntimeError(f"{label}\n{run.stderr}")
        plan = None
        decoder = json.JSONDecoder()
        for match in re.finditer(r"\bplan:\s*(?=\{)", run.stderr):
            document, _ = decoder.raw_decode(run.stderr[match.end():])
            if "o_orderkey in" in document.get("Query Text", ""):
                plan = document["Plan"]
                break
        if plan is None:
            raise RuntimeError(f"Missing Q18 execution plan in {label}\n{run.stderr}")
        timings = re.findall(r"^Time: ([\d.]+) ms.*$", run.stdout, re.MULTILINE)
        if len(timings) != 1:
            raise RuntimeError(f"Expected one psql statement time in {label}: {timings}")
        result = re.sub(r"^Time: .*\n?", "", run.stdout, flags=re.MULTILINE).strip().splitlines()
        if len(result) != 100:
            raise RuntimeError(f"{label} returned {len(result)} rows, expected 100")
        if reference is None:
            reference = sorted(result)
        elif sorted(result) != reference:
            raise RuntimeError(f"{label} returned different Q18 rows from the first sample")
        yield mode, workmem_mib, plan, float(timings[0])


def ibex_ranges(scan):
    """Read each range's input count from the device and native scans."""
    cutoff = scan["CSD Ibex Exclusive Upper Key"]
    host_aggregate, = scan["Plans"]
    host_scan, = host_aggregate["Plans"]
    if host_scan["Node Type"] != "Index Scan" or host_scan["Actual Loops"] != 1:
        raise RuntimeError("Ibex did not scan the complementary index range")
    return cutoff, scan["CSD Heap Rows"], host_scan["Actual Rows"]


def test(paths=MODES):
    """Run the same SQL once per independently cold execution path."""
    csv_path = RESULTS / f"{TEST_NAME}.csv"
    rows = {}
    if paths != MODES and csv_path.exists():
        with csv_path.open(newline="") as source:
            rows = {row["exe_path"]: row for row in csv.DictReader(source)}
    for mode, workmem_mib, plan, elapsed_ms in cold_q18((mode, DEVICE_MEMORY_MIB) for mode in paths):
        nodes = [plan]
        for node in nodes:
            nodes.extend(node.get("Plans", []))
        device = [node for node in nodes if "CSD Host Result Bytes" in node]
        device_bytes = read_wait_ms = write_wait_ms = 0
        if mode == "host-only":
            if any(node["Node Type"] == "Custom Scan" for node in nodes):
                raise RuntimeError(f"{mode} did not run a native PG plan\n{json.dumps(plan)}")
        else:
            if mode == "filter-only":
                expected = device and all(
                    scan.get("Execution Mode") == "filter-only"
                    and re.fullmatch(r"0:Scan\([^)]*\).*", scan["CSD Plan"])
                    for scan in device
                )
            else:
                expected_path = "ibex" if mode == "ibex" else "csd"
                expected = len(device) == 1 and device[0].get("Execution Path") == expected_path
            if not expected:
                raise RuntimeError(f"{mode} did not run the expected device path\n{json.dumps(plan)}")
            if any(scan["CSD Query Memory Limit"] != workmem_mib * 1024**2 for scan in device):
                raise RuntimeError(f"{mode} did not use the configured CSD workmem")
            reads = sum(scan["CSD Spill Read"] for scan in device)
            writes = sum(scan["CSD Spill Write"] for scan in device)
            if mode == "ibex" and (reads or writes):
                raise RuntimeError("Ibex device range unexpectedly spilled")
            if mode == "ibex" and (
                not device[0]["CSD Result Delivery"].startswith("SNIA callbacks")
                or device[0]["CSD Direct CU Output"]
            ):
                raise RuntimeError("Ibex did not deliver coordinator results by callback")
            if mode == "csd-without-hpcp" and not (reads and writes):
                raise RuntimeError("Full device execution did not both write and read spill")
            # Each plan node reports its complete device stream, including
            # shared streams. Do not traverse EXPLAIN's separate Workers entries.
            device_bytes = sum(scan["CSD Host Result Bytes"] for scan in device)
            # Task wait sums include reentry scheduling and overlap across CUs.
            read_wait_ms = round(sum(scan["CSD Spill Read Wait Sum"] for scan in device) / 1e6, 3)
            write_wait_ms = round(sum(scan["CSD Spill Write Wait Sum"] for scan in device) / 1e6, 3)
            print(f"{mode}: device spill read/write {reads}/{writes} bytes", flush=True)
            for scan in device:
                # Rows and delivery counters distinguish data reduction from
                # the costs of feeding the native PG operators.
                print(
                    f"{scan['CSD Plan']}: {scan['CSD Result Delivery']}, "
                    f"scanned={scan['CSD Heap Rows']}, returned={scan['CSD Returned Rows']}, "
                    f"result_reads={scan['CSD Host Result Requests']}, "
                    f"result_read_ms={scan.get('CSD Result Read Wall')}, "
                    f"host_idle_ms={scan.get('CSD Host Idle Sleep')}, "
                    f"delivery_full_wait_ms={scan.get('CSD Delivery Full Wait Sum')}, "
                    f"peak_bytes={scan['CSD Workmem Peak']}",
                    flush=True,
                )
            if mode == "ibex":
                ibex = device[0]
                cutoff, device_rows, host_rows = ibex_ranges(ibex)
                print(f"Ibex: key < {cutoff} on device, device_rows={device_rows}, "
                      f"host_rows={host_rows}, device_groups={ibex['CSD Returned Rows']}", flush=True)
                # Separate work within the device calls from waits between
                # calls and host-side command handling. Worker waits overlap.
                delivery_counters = (
                    "CSD CSF CPU", "CSD CSF Calls", "CSD Scan Decode CPU",
                    "CSD Aggregate Sink Coordinator CPU",
                    "CSD Host Submissions", "CSD Callback Submissions", "CSD Host Submit",
                    "CSD Host Completion Copy", "CSD Control Read Sum", "CSD Host Control Write (within Submit)",
                    "CSD Pending Requests Sum", "CSD Completion Notice Average", "CSD PG Scan Requeue Average",
                    "CSD Shared Sink Wait", "CSD Delivery Advance Wall",
                )
                for name, value in ibex.items():
                    if name.startswith(("CSD Pipeline ", "CSD Ibex ")) or name in delivery_counters:
                        print(f"{name} = {value}", flush=True)
        rows[mode] = dict(
            exe_path=mode,
            end_to_end_ms=elapsed_ms,
            device_to_host_bytes=device_bytes,
            spill_read_wait_ms=read_wait_ms,
            spill_write_wait_ms=write_wait_ms,
        )
        print(rows[mode], flush=True)
    RESULTS.mkdir(parents=True, exist_ok=True)
    with csv_path.open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(rows[mode] for mode in MODES if mode in rows)


def plot():
    """Plot the final CSV without running the database."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    matplotlib.rcParams.update({"svg.fonttype": "none", "pdf.fonttype": 42})

    with (RESULTS / f"{TEST_NAME}.csv").open(newline="") as source:
        rows = list(csv.DictReader(source))
    fig, axes = plt.subplots(1, 3, figsize=(10.4, 3.0), layout="constrained")
    styles = {
        "host-only": ("Host", "#72777E"),
        "filter-only": ("Filter-only", "#568D81"),
        "csd-without-hpcp": ("CSD-only", "#406B93"),
        "ibex": ("Ibex", "#CE784A"),
    }
    labels, colors = zip(*(styles[row["exe_path"]] for row in rows))
    latency = [float(row["end_to_end_ms"]) / 1000 for row in rows]
    transfer = [int(row["device_to_host_bytes"]) / 1024**2 for row in rows]
    for ax, values, ylabel in zip(axes[:2], (latency, transfer),
                                  ("End-to-end latency (s)", "Device-to-host data (MiB)")):
        bars = ax.bar(labels, values, color=colors, width=0.56)
        ax.set_ylabel(ylabel)
        ax.bar_label(bars, fmt="%.3g", padding=3)
    for field, label, color, offset in (
        ("spill_read_wait_ms", "Read", "#568D81", -0.17),
        ("spill_write_wait_ms", "Write", "#9C749F", 0.17),
    ):
        values = [float(row[field]) / 1000 for row in rows]
        bars = axes[2].bar([index + offset for index in range(len(rows))], values,
                           width=0.32, label=label, color=color)
        axes[2].bar_label(bars, fmt="%.3g", padding=3)
    axes[2].set_xticks(range(len(rows)), labels)
    axes[2].set_ylabel("Device spill wait sum (s)")
    axes[2].legend(frameon=False, loc="upper left")
    for ax in axes:
        ax.spines[["top", "right"]].set_visible(False)
        ax.margins(y=0.25)
    fig.suptitle(f"SF8 Q18, {DEVICE_MEMORY_MIB} MiB device work memory")
    for extension in ("svg", "pdf"):
        fig.savefig(RESULTS / f"{TEST_NAME}.{extension}", format=extension)
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("test", "plot"))
    parser.add_argument("--path", choices=MODES, action="append",
                        help="Cold-test only the selected paths and update their CSV rows.")
    args = parser.parse_args()
    if args.action == "test":
        test(tuple(mode for mode in MODES if mode in args.path) if args.path else MODES)
    else:
        if args.path:
            parser.error("--path is only available for test")
        plot()


if __name__ == "__main__":
    main()
