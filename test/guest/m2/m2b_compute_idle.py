#!/usr/bin/env python3
"""Measure CSD internal reads, compute activity, and simulated PCIe traffic."""

# Summary CSV columns:
# target_selectivity: requested qualifying row fraction (0.1 to 0.9).
# filter_active_ns: union of the wall-clock intervals of device filter calls.
# both_idle_ns: part of filter_active_ns with no modeled PCIe payload in either direction.
# both_idle_fraction: both_idle_ns divided by filter_active_ns.
#
# Timeline CSV (0.9 selectivity):
# event: NVM read completion, heap decode, filter call, or a PCIe transfer direction.
# start_ns / end_ns: elapsed ns from the first recorded workload activity.
#     Equal timestamps mark an internal read becoming available in device memory.
#     Other events cover half-open intervals. Compute intervals may overlap.
# bytes: payload bytes delivered at the internal completion or over the PCIe interval.
#     Compute events carry zero bytes.

import argparse
import csv
import fcntl
import json
import re
import shlex
import subprocess
import tempfile
from pathlib import Path

TEST_NAME = Path(__file__).stem
ROOT = Path(__file__).resolve().parents[3]
CSV_PATH = ROOT / "results" / TEST_NAME / f"{TEST_NAME}.csv"
LEVELS = tuple(f"{tenth / 10:.1f}" for tenth in range(1, 10))
GUEST = ["docker", "compose", "exec", "-T", "femu-csd", "./run-guest-ssh.sh"]
PSQL = "/home/femu/opt/postgres/release/bin/psql -XqAt -v ON_ERROR_STOP=1 -d tpch_s8"
PG_CTL = "/home/femu/opt/postgres/release/bin/pg_ctl -D /data/tpch_s8 -w"
FILTER_OPTIONS = (
    "-c session_preload_libraries=pg_sina_csd -c pg_sina_csd.exe_path=filter-only "
    "-c pg_sina_csd.hpcp=off -c pg_sina_csd.dcp=off"
)
NVME_WRITE, NVME_READ = 0x01, 0x02
AFDM_READ, AFDM_WRITE = 0xF2, 0xF5
DIRECTIONS = ("csd_to_host", "host_to_csd")
TRACE_FIELDS = (
    "xfer=req->xfer_bytes:u64 lbas=req->nlb:u16 begin=start:u64 span=transfer:u64 "
    "direction=req->is_write:u16 op=req->cmd_opcode:u8 cap=n->pcie_bandwidth_mbps:u32"
)
TRACE_EVENT = re.compile(
    r"\bxfer=(\d+) lbas=(\d+) begin=(\d+) span=(\d+) "
    r"direction=([01]) op=(\d+) cap=(\d+)\s*$"
)
SUMMARY_FIELDS = ("target_selectivity", "filter_active_ns", "both_idle_ns", "both_idle_fraction")
TRACE_LINE = re.compile(r"\s*(\d+)\s+(\d+)\.(\d{9}):\s+(probe:[^: ]+):\s+(.*)")
NVM_EVENT = re.compile(r"\bbytes=(\d+) op=(\d+) state=(\d+) status=(\d+)\s*$")
GRAPH_LOAD, GRAPH_IO_DONE = 1, 5
ACTIVE_SQL = """SELECT count(*) FROM pg_stat_activity
                WHERE backend_type = 'client backend' AND pid <> pg_backend_pid()"""
COLD_RESTART = (
    f"{PG_CTL} -m fast stop >&2 && "
    "{ sync && sudo -n sh -c 'echo 3 > /proc/sys/vm/drop_caches'; status=$?; "
    f"{PG_CTL} -l /data/tpch_s8/log start >&2 && exit \"$status\"; }}"
)


def run(command):
    return subprocess.check_output(command, cwd=ROOT, text=True).strip()


def guest(command):
    return run([*GUEST, command])


def sql(query, filter_only=False):
    options = f"PGOPTIONS={shlex.quote(FILTER_OPTIONS)} " if filter_only else "PGOPTIONS= "
    return options + PSQL + " -c " + shlex.quote(query)


def lineitem_thresholds():
    # This preparation runs before the experimental cold restarts. It sets the
    # thresholds and supplies exact reference row counts for the measured scans.
    histogram = guest(sql(
        "SELECT l_quantity::int, count(*) FROM public.lineitem GROUP BY 1 ORDER BY 1"
    ))
    cumulative = {}
    total = 0
    for line in histogram.splitlines():
        quantity, count = map(int, line.split("|"))
        total += count
        cumulative[quantity] = total
    if not 47_000_000 < total < 49_000_000:
        raise RuntimeError(f"Unexpected TPC-H SF8 lineitem row count: {total}")
    cases = []
    for tenth, level in enumerate(LEVELS, start=1):
        quantity = min(cumulative, key=lambda value: abs(10 * cumulative[value] - tenth * total))
        cases.append((level, quantity, total, cumulative[quantity]))
    return cases


def merged(intervals):
    result = []
    for start, end in sorted(intervals):
        if not result or start > result[-1][1]:
            result.append((start, end))
        elif end > result[-1][1]:
            result[-1] = (result[-1][0], end)
    return result


def overlap_time(a, b):
    first = second = duration = 0
    while first < len(a) and second < len(b):
        start_a, end_a = a[first]
        start_b, end_b = b[second]
        duration += max(0, min(end_a, end_b) - max(start_a, start_b))
        if end_a <= end_b:
            first += 1
        else:
            second += 1
    return duration


def capture(pid, events, trace_path, query, cap, lba_bytes):
    filter_entry, filter_exit, scan_entry, scan_exit, pcie, nvm = events
    entries = {filter_entry: "filter", scan_entry: "decode"}
    exits = {filter_exit: "filter", scan_exit: "decode"}
    trace_path.unlink(missing_ok=True)
    output = run([
        "sudo", "-n", "perf", "record", "-q", "--clockid", "mono", "-m", "1024",
        "-o", str(trace_path), "-p", pid, "-e", ",".join(events), "--", *GUEST, sql(query, True),
    ])
    trace = run([
        "sudo", "-n", "perf", "script", "--ns", "--show-lost-events", "-i", str(trace_path),
        "-F", "trace:tid,time,event,trace",
    ])
    active = {}
    compute = {"filter": [], "decode": []}
    reads = []
    transfers = []
    for line in trace.splitlines():
        record = TRACE_LINE.fullmatch(line)
        if record is None:
            raise RuntimeError(f"Unexpected perf record (possible sample loss): {line}")
        tid, seconds, ns, event, detail = record.groups()
        stamp = int(seconds) * 1_000_000_000 + int(ns)
        if event in entries:
            key = (entries[event], tid)
            if key in active:
                raise RuntimeError(f"Unexpected nested compute call on thread {tid}")
            active[key] = stamp
        elif event in exits:
            kind = exits[event]
            start = active.pop((kind, tid))
            if start >= stamp:
                raise RuntimeError("Invalid compute duration")
            compute[kind].append((start, stamp))
        elif event == nvm:
            match = NVM_EVENT.search(detail)
            if match is None:
                raise RuntimeError(f"Unexpected internal read event: {line}")
            size, operation, state, status = map(int, match.groups())
            if operation == GRAPH_LOAD and state == GRAPH_IO_DONE:
                if not size or status:
                    raise RuntimeError(f"Incomplete internal read: {line}")
                reads.append((stamp, size))
        elif event == pcie:
            match = TRACE_EVENT.search(detail)
            if match is None:
                raise RuntimeError(f"Unexpected PCIe event: {line}")
            xfer, lbas, start, span, direction, opcode, bandwidth = map(int, match.groups())
            size = lbas * lba_bytes if opcode in (NVME_READ, NVME_WRITE) else xfer
            if (
                bandwidth != cap or not size or span != (size * 1000 + cap - 1) // cap
                or abs(start - stamp) > 1_000_000_000
            ):
                raise RuntimeError(f"PCIe duration or monotonic timestamp check failed: {line}")
            transfers.append((start, start + span, direction, opcode, size))
        else:
            raise RuntimeError(f"Unexpected trace event: {event}")
    if active or not all(compute.values()) or not reads or not transfers:
        raise RuntimeError(f"Incomplete CSD trace: unmatched={len(active)}, "
                           f"filter={len(compute['filter'])}, decode={len(compute['decode'])}, "
                           f"nvm={len(reads)}, pcie={len(transfers)}")
    return json.loads(output)[0], compute, reads, transfers


def write_csv(path, fields, rows):
    with path.open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(fields)
        writer.writerows(rows)


def test(csv_path):
    summary = []
    timeline = []
    with tempfile.TemporaryDirectory(prefix=f"{TEST_NAME}-") as tmp, Path(
        f"/tmp/{TEST_NAME}.lock"
    ).open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if guest(sql(ACTIVE_SQL)) != "0":
            raise RuntimeError("Another PostgreSQL client is connected")
        source = guest("findmnt -n -o SOURCE -T /data/tpch_s8")
        memory = int(guest("awk '/^MemTotal:/ {print $2}' /proc/meminfo"))
        settings = guest(sql(
            "SELECT current_setting('data_directory'), current_setting('shared_buffers'), "
            "current_setting('work_mem'), current_setting('max_parallel_workers_per_gather'), "
            "current_setting('max_parallel_workers'), current_setting('max_worker_processes'), "
            "current_setting('enable_hashagg'), current_setting('autovacuum')"
        ))
        if source != "/dev/nvme0n1" or not 29 * 1024**2 <= memory <= 30 * 1024**2:
            raise RuntimeError("Expected the SF8 FEMU volume and a Guest configured for 30 GiB")
        if settings != "/data/tpch_s8|2GB|1GB|8|16|16|off|off":
            raise RuntimeError(f"Unexpected PostgreSQL settings: {settings}")
        lba_bytes = int(guest("cat /sys/class/block/nvme0n1/queue/logical_block_size"))
        container = run(["docker", "compose", "ps", "-q", "femu-csd"])
        processes = run(["docker", "top", container, "-eo", "pid,comm"]).splitlines()[1:]
        (pid,) = [line.split()[0] for line in processes if line.split()[1].startswith("qemu-system")]
        print(f"Guest MemTotal={memory} KiB, PostgreSQL={settings}, FEMU PID={pid}", flush=True)
        workload = lineitem_thresholds()
        qemu = run(["sudo", "-n", "readlink", f"/proc/{pid}/exe"])
        program = run(["docker", "compose", "exec", "-T", "femu-csd", "printenv", "CSD_PROGRAM_DIR"])
        library = str(Path(program) / "pg-sina-plan.so")
        cap = int(run([
            "docker", "compose", "exec", "-T", "femu-csd", "printenv", "CSD_PCIE_BANDWIDTH_MBPS",
        ]))
        if cap <= 0:
            raise RuntimeError("Expected a positive modeled PCIe limit")
        prefix = f"probe:{TEST_NAME}_{pid}"
        probes = (
            (library, f"{prefix}_begin", "pg_sina_filter_impl", ""),
            (library, f"{prefix}_end", "pg_sina_filter_impl%return", "__return"),
            (library, f"{prefix}_scan_begin", "pg_sina_scan_impl", ""),
            (library, f"{prefix}_scan_end", "pg_sina_scan_impl%return", "__return"),
            (qemu, f"{prefix}_link", f"nvme_charge_link:17 {TRACE_FIELDS}", ""),
            # For explicit LOAD nodes, graph completion follows both the
            # modeled NAND deadline and the copy into device memory.
            (qemu, f"{prefix}_nvm", "csd_graph_node_done bytes=v->transferred:u64 "
             "op=v->wire.operation:u8 state=v->state:u32 status=v->graph->status:u16", ""),
        )
        events = []
        try:
            for binary, event, point, suffix in probes:
                run(["sudo", "-n", "perf", "probe", "--target-ns", pid, "-x", binary,
                     "--add", f"{event}={point}"])
                events.append(event + suffix)
            trace_path = Path(tmp) / "perf.data"
            for level, quantity, input_rows, output_rows in workload:
                if guest(sql(ACTIVE_SQL)) != "0":
                    raise RuntimeError("Another PostgreSQL client is connected")
                guest(COLD_RESTART)
                query = (
                    "EXPLAIN (ANALYZE, TIMING OFF, FORMAT JSON) "
                    f"SELECT l_orderkey FROM public.lineitem WHERE l_quantity <= {quantity}"
                )
                explain, calls, reads, transfers = capture(pid, events, trace_path, query, cap, lba_bytes)
                root = explain["Plan"]
                plan = root["Plans"][0] if root["Node Type"] == "Gather" else root
                if (
                    root["Actual Rows"] != output_rows
                    or plan.get("Custom Plan Provider") != "SniaCsdPlan"
                    or plan.get("Execution Mode") != "filter-only"
                    or plan.get("CSD Plan") != "0:Scan(lineitem)"
                    or plan.get("CSD Heap Rows") != input_rows
                    or plan.get("HPCP") != "disabled" or plan.get("DCP") != "disabled"
                ):
                    raise RuntimeError(f"Unexpected execution plan for {level}: {root}")
                afdm = [(start, end) for start, end, _, op, _ in transfers if op in (AFDM_READ, AFDM_WRITE)]
                read_bytes = sum(size for _, _, _, op, size in transfers if op == AFDM_READ)
                if read_bytes < plan["CSD Host Result Bytes"]:
                    raise RuntimeError(f"Missing CSD result transfers for {level}")
                nvm_bytes = sum(size for _, size in reads)
                if nvm_bytes != plan["CSD Internal Read"]:
                    raise RuntimeError(f"Internal read trace for {level}: {nvm_bytes} bytes, "
                                       f"executor consumed {plan['CSD Internal Read']} bytes")
                compute = merged(calls["filter"])
                busy = merged((start, end) for start, end, _, _, _ in transfers)
                filter_ns = sum(end - start for start, end in compute)
                simultaneous_ns = overlap_time(compute, busy)
                idle_ns = filter_ns - simultaneous_ns
                fraction = idle_ns / filter_ns
                summary.append((level, filter_ns, idle_ns, f"{fraction:.12f}"))
                print(
                    f"selectivity={level} filter_calls={len(calls['filter'])} decode_calls={len(calls['decode'])} "
                    f"nvm_reads={len(reads)} nvm_bytes={nvm_bytes} pcie_transfers={len(transfers)} "
                    f"filter_active_ms={filter_ns / 1e6:.3f} busy_during_filter_ms={simultaneous_ns / 1e6:.3f} "
                    f"both_idle={fraction:.4%} returned={root['Actual Rows']} "
                    f"execution_ms={explain['Execution Time']}",
                    flush=True,
                )
                if level == LEVELS[-1]:
                    all_activity = [*afdm, *calls["filter"], *calls["decode"]]
                    start = min(min(a for a, _ in all_activity), min(stamp for stamp, _ in reads))
                    end = max(max(b for _, b in all_activity), max(stamp for stamp, _ in reads) + 1)
                    for kind, intervals in calls.items():
                        timeline.extend((kind, a - start, b - start, 0) for a, b in intervals)
                    timeline.extend(("nvm_read", stamp - start, stamp - start, size) for stamp, size in reads)
                    timeline.extend(
                        (DIRECTIONS[direction], a - start, b - start, size)
                        for a, b, direction, _, size in transfers if start <= a < b <= end
                    )
        finally:
            for event in reversed(events):
                run(["sudo", "-n", "perf", "probe", "-d", event])
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    timeline_path = csv_path.with_name(f"{csv_path.stem}_timeline.csv")
    write_csv(timeline_path, ("event", "start_ns", "end_ns", "bytes"), sorted(timeline, key=lambda event: event[1]))
    write_csv(csv_path, SUMMARY_FIELDS, summary)
    print(f"Wrote {csv_path} and {timeline_path}", flush=True)


def binned_resource(events, left, right, width):
    """Average active calls or delivered decimal GB/s in each window."""
    edges = [*range(left, right, width), right]
    bins = [0.0] * (len(edges) - 1)
    for start, end, size in events:
        if start == end:
            if left <= end < right:
                bins[(end - left) // width] += size
            continue
        a, b = max(start, left), min(end, right)
        if a >= b:
            continue
        for index in range((a - left) // width, (b - 1 - left) // width + 1):
            overlap = min(b, edges[index + 1]) - max(a, edges[index])
            bins[index] += size * overlap / (end - start) if size else overlap
    return [value / (b - a) for value, a, b in zip(bins, edges, edges[1:])]


def plot(csv_path):
    import matplotlib.pyplot as plt
    from matplotlib.ticker import MaxNLocator, MultipleLocator

    with csv_path.open(newline="") as handle:
        rows = list(csv.DictReader(handle))
    if tuple(row["target_selectivity"] for row in rows) != LEVELS:
        raise ValueError("Expected the nine selectivities in the summary CSV")
    kinds = ("nvm_read", "decode", "filter", *DIRECTIONS)
    events = {kind: [] for kind in kinds}
    with csv_path.with_name(f"{csv_path.stem}_timeline.csv").open(newline="") as handle:
        for row in csv.DictReader(handle):
            events[row["event"]].append((int(row["start_ns"]), int(row["end_ns"]), int(row["bytes"])))
    end = max(b for segments in events.values() for _, b, _ in segments)
    # Detail window is centered on the midpoint of the recorded workload.
    zoom_left = max(0, (end // 2 - 50_000_000) // 10_000_000 * 10_000_000)
    zoom_right = min(end, zoom_left + 100_000_000)
    windows = ((0, end, 10_000_000), (zoom_left, zoom_right, 1_000_000))

    plt.rcParams.update({
        "font.family": "sans-serif",
        "font.sans-serif": ["Liberation Sans", "DejaVu Sans"],
        "font.size": 8,
        "axes.labelsize": 8,
        "axes.titlesize": 9,
        "axes.spines.top": False,
        "axes.spines.right": False,
        "axes.linewidth": 0.6,
        "pdf.fonttype": 42,
        "svg.fonttype": "none",
    })
    fig = plt.figure(figsize=(7.2, 5.3))
    grid = fig.add_gridspec(5, 2, width_ratios=(2.25, 1), height_ratios=(1, 1, 1, 0.3, 1.45))
    fig.subplots_adjust(left=0.19, right=0.965, top=0.86, bottom=0.105, hspace=0.45, wspace=0.24)
    tracks = (
        ("Internal read\n(GB/s)", (("nvm_read", "NVM to device memory", "#6159A4"),)),
        ("Compute\n(active calls)", (("decode", "Decode", "#8BBCA5"), ("filter", "Filter", "#22735C"))),
        ("External PCIe\n(GB/s)", ((DIRECTIONS[0], "CSD to HOST", "#BD572A"),
                                    (DIRECTIONS[1], "HOST to CSD", "#D7A144"))),
    )
    legend_handles = []
    # Use equal vertical scales for the two views of each resource.
    for i, (label, track) in enumerate(tracks):
        track_axes = []
        peak = 0
        for j, (left, right, width) in enumerate(windows):
            ax = fig.add_subplot(grid[i, j])
            track_axes.append(ax)
            edges = [stamp / 1e9 for stamp in (*range(left, right, width), right)]
            bottom = [0.0] * (len(edges) - 1)
            for kind, name, color in track:
                values = binned_resource(events[kind], left, right, width)
                top = [a + b for a, b in zip(bottom, values)]
                artist = ax.stairs(top, edges, baseline=bottom, fill=True,
                                   color=color, linewidth=0, label=name)
                if j == 0:
                    legend_handles.append(artist)
                bottom = top
            if j == 0:
                ax.axvspan(zoom_left / 1e9, zoom_right / 1e9, color="#86929C", alpha=0.23, lw=0)
                ax.set_ylabel(label, rotation=0, rotation_mode="anchor",
                              ha="right", va="center", labelpad=8)
                ax.xaxis.set_major_locator(MultipleLocator(1))
            else:
                ax.xaxis.set_major_locator(MultipleLocator(0.05))
            ax.set_xlim(left / 1e9, right / 1e9)
            peak = max(peak, max(bottom))
            ax.yaxis.set_major_locator(MaxNLocator(3))
            if i == 0:
                ax.set_title(("A   Selectivity 0.9 · 10 ms bins", "100 ms detail · 1 ms bins")[j],
                             loc="left", fontweight="bold", pad=7)
            if i < 2:
                ax.tick_params(axis="x", bottom=False, labelbottom=False)
            else:
                ax.set_xlabel("Elapsed time (s)", labelpad=3)
        for ax in track_axes:
            ax.set_ylim(0, peak * 1.08)
    fig.legend(handles=legend_handles, loc="upper center", bbox_to_anchor=(0.56, 0.983),
               ncol=5, frameon=False, columnspacing=1.2, handlelength=1.25)

    summary = fig.add_subplot(grid[4, :])
    x = list(range(len(LEVELS)))
    idle = [100 * float(row["both_idle_fraction"]) for row in rows]
    summary.bar(x, idle, width=0.62, color="#23745D")
    summary.bar(x, [100 - value for value in idle], bottom=idle, width=0.62, color="#D4DEE4")
    summary.set_title("B   Both PCIe directions idle during filtering", loc="left", fontweight="bold", pad=8)
    summary.set_ylim(0, 117)
    summary.set_yticks([0, 25, 50, 75, 100])
    summary.set_ylabel("Share of filter\nactive time (%)", rotation=0, rotation_mode="anchor",
                       ha="right", va="center", labelpad=8)
    summary.set_xticks(x, LEVELS)
    summary.set_xlabel("Target filter selectivity", labelpad=3)
    for position, value in zip(x, idle):
        summary.annotate(f"{value:.2f}%", (position, 100), xytext=(0, 3), textcoords="offset points",
                         ha="center", va="bottom", color="#1F5D4C", fontsize=7)
    fig.canvas.draw()
    for extension in (".svg", ".pdf", ".png"):
        fig.savefig(csv_path.with_suffix(extension), dpi=600)
    return fig


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("test", "plot"))
    parser.add_argument("--csv", type=Path, default=CSV_PATH)
    args = parser.parse_args()
    if args.mode == "test":
        test(args.csv.resolve())
    else:
        plot(args.csv.resolve())


if __name__ == "__main__":
    main()
