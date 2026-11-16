#!/usr/bin/env python3
"""Measure native PostgreSQL spill cost for SF8 Q21, or plot the final CSV.

Run test in the Guest and plot with Matplotlib.
The Guest test uses bpftrace to time the backend's temporary-file calls.

CSV fields
    work_mem        Memory tier relative to the M1a Q21 peak. The >peak
                    tier uses the 2 GiB no-spill baseline. The remaining
                    tiers run from 0.9peak through 0.1peak.
    spilled_bytes   Sum of PostgreSQL's logged temporary-file sizes (bytes).
    latency_ms      Wall-clock latency of the Q21 statement from psql (ms).
    extra_delay_ms  latency_ms minus the no-spill baseline latency (ms).
    read_temp_lat   Total wall time in temporary-file reads (ms).
    write_temp_lat  Total wall time in temporary-file writes (ms).
    extra_cal_lat   Change in backend CPU time outside temporary-file calls
                    relative to the no-spill baseline (ms).
    other_lat       Signed remainder after subtracting read_temp_lat,
                    write_temp_lat and extra_cal_lat from extra_delay_ms (ms).
"""

import argparse
from contextlib import ExitStack
import csv
from decimal import Decimal
from itertools import chain, cycle
import json
import math
import os
from pathlib import Path
import re
import selectors
import shlex
import subprocess
import tempfile


TEST_NAME = Path(__file__).stem
PROJECT_ROOT = Path(__file__).resolve().parents[3]
GUEST_TESTS = PROJECT_ROOT / "tests"
if (GUEST_TESTS / "guest").is_dir():
    GUEST_TESTS /= "guest"
RESULTS_DIR = PROJECT_ROOT / "results" / "m1"
BASE_FIELDS = ("work_mem", "spilled_bytes", "latency_ms", "extra_delay_ms")
BREAKDOWN_FIELDS = ("read_temp_lat", "write_temp_lat", "extra_cal_lat", "other_lat")
FIELDS = BASE_FIELDS + BREAKDOWN_FIELDS
LEVELS = (">peak", *(f"0.{step}peak" for step in range(9, 0, -1)))
RETEST_LEVELS = ("0.6peak", "0.5peak")
RETEST_SPILLS = (458226132, 458347568)
REPEAT_LEVELS = LEVELS[1:6]
DIAGNOSTIC_LEVELS = ("0.4peak", "0.3peak")
# Q21's native final batch/bucket counts for the large l3 hash build.
# A preset makes that layout available before the first build tuple.
Q21_HASH_PRESETS = {
    "0.9peak": (2, 2**25), "0.8peak": (2, 2**25),
    "0.7peak": (2, 2**25), "0.6peak": (2, 2**25),
    "0.5peak": (2, 2**24), "0.4peak": (4, 2**24), "0.3peak": (4, 2**24),
    "0.2peak": (8, 2**23), "0.1peak": (16, 2**22),
}
EXPLAIN_OPTIONS = (
    " -c session_preload_libraries=auto_explain -c auto_explain.log_min_duration=0 "
    "-c auto_explain.log_analyze=on -c auto_explain.log_timing=off -c auto_explain.log_format=json"
)

SPILL_TRACE = r"""
BEGIN { printf("M1C_READY\n"); }

kfunc:vfs_read,kfunc:vfs_write
/pid == $1 && strncmp(str(args->file->f_path.dentry->d_name.name), "pgsql_tmp", 9) == 0/
{
    @started = nsecs;
    @off_cpu_ns = 0;
}

tracepoint:sched:sched_switch
/args->prev_pid == $1 && @started/
{
    @off_cpu_start = nsecs;
}

tracepoint:sched:sched_switch
/args->next_pid == $1 && @off_cpu_start/
{
    @off_cpu_ns += nsecs - @off_cpu_start;
    @off_cpu_start = 0;
}

kretfunc:vfs_read,kretfunc:vfs_write
/pid == $1 && @started/
{
    $elapsed = nsecs - @started;
    @wall_ns[probe] = sum($elapsed);
    @cpu_ns = sum($elapsed > @off_cpu_ns ? $elapsed - @off_cpu_ns : 0);
    if ((int64)retval > 0) {
        @io_bytes[probe] = sum(retval);
    }
    @started = 0;
}

tracepoint:sched:sched_process_exit
/tid == $1/
{
    exit();
}

END { printf("M1C_COMPLETE\n"); }
"""


def _profile_query(psql, query, env, trace_path):
    """Run the original SQL and collect timings for just its PostgreSQL backend."""
    with tempfile.TemporaryFile(mode="w+") as pg_errors, tempfile.TemporaryFile(mode="w+") as trace_errors:
        with subprocess.Popen(
            psql, cwd=query.parent, env=env, stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=pg_errors, text=True,
        ) as client:
            client.stdin.write("SELECT pg_backend_pid();\n")
            client.stdin.flush()
            pid = client.stdout.readline().strip()
            if not pid.isdecimal():
                pg_errors.seek(0)
                raise RuntimeError(f"Could not obtain the PostgreSQL backend PID: {pg_errors.read().strip()}")
            schedstat = Path(f"/proc/{pid}/schedstat")
            trace = subprocess.Popen(
                ["sudo", "-n", "bpftrace", "-q", "-B", "none", str(trace_path), pid],
                stdout=subprocess.PIPE, stderr=trace_errors, text=True,
            )
            try:
                with selectors.DefaultSelector() as ready:
                    ready.register(trace.stdout, selectors.EVENT_READ)
                    if not ready.select(20) or trace.stdout.readline().strip() != "M1C_READY":
                        trace_errors.seek(0)
                        raise RuntimeError(f"Could not start the spill probe: {trace_errors.read().strip()}")
                # schedstat measures backend CPU time. The trace also measures
                # the CPU time inside temp-file calls, which must be subtracted.
                cpu_start = int(schedstat.read_text().split()[0])
                marker = "__M1C_QUERY_COMPLETE__"
                client.stdin.write(f"\\i {query.name}\n\\echo {marker}\n")
                client.stdin.flush()
                lines = []
                for line in client.stdout:
                    if line.rstrip("\n") == marker:
                        break
                    lines.append(line)
                else:
                    pg_errors.seek(0)
                    raise RuntimeError(f"Formal SQL did not complete: {pg_errors.read().strip()}")
                cpu_ns = int(schedstat.read_text().split()[0]) - cpu_start
            finally:
                client.stdin.close()
                # The exit tracepoint stops the probe when psql's backend exits.
                try:
                    trace_output, _ = trace.communicate(timeout=20)
                except subprocess.TimeoutExpired:
                    trace.terminate()
                    trace_output, _ = trace.communicate(timeout=10)

            if client.wait() or trace.returncode or "M1C_COMPLETE" not in trace_output.splitlines():
                trace_errors.seek(0)
                raise RuntimeError(f"Spill probe did not complete: {trace_errors.read().strip()}")
        pg_errors.seek(0)
        stderr = pg_errors.read()

    counts = dict(re.findall(r"^(@[\w]+(?:\[[^\]]+\])?): (\d+)$", trace_output, re.MULTILINE))
    read_ns = int(counts.get("@wall_ns[kretfunc:vmlinux:vfs_read]", 0))
    write_ns = int(counts.get("@wall_ns[kretfunc:vmlinux:vfs_write]", 0))
    temp_cpu_ns = int(counts.get("@cpu_ns", 0))
    write_bytes = int(counts.get("@io_bytes[kretfunc:vmlinux:vfs_write]", 0))
    if temp_cpu_ns > cpu_ns:
        raise RuntimeError("Measured temporary-file CPU time exceeds backend CPU time")
    return "".join(lines), stderr, read_ns, write_ns, cpu_ns - temp_cpu_ns, write_bytes


def _write_csv(path, rows):
    with tempfile.TemporaryDirectory(dir=path.parent) as temp_dir:
        temporary_csv = Path(temp_dir) / path.name
        with temporary_csv.open("w", encoding="utf-8", newline="") as stream:
            writer = csv.writer(stream, lineterminator="\n")
            writer.writerow(FIELDS)
            writer.writerows(rows)
        temporary_csv.replace(path)


def _read_q21_plan(stderr):
    decoder = json.JSONDecoder()
    for match in re.finditer(r"\bplan:\s*(?=\{)", stderr):
        explained, _ = decoder.raw_decode(stderr[match.end():])
        if "l_receiptdate > l1.l_commitdate" in explained.get("Query Text", ""):
            return explained["Plan"]
    raise RuntimeError("No Q21 plan in auto_explain output")


def _q21_hash_nodes(plan):
    nodes = [plan]
    for node in nodes:
        nodes.extend(node.get("Plans", []))
    hashes = [node for node in nodes if node["Node Type"] == "Hash"]
    targets = [node for node in hashes if node["Plans"][0].get("Alias") == "l3"]
    if len(targets) != 1:
        raise RuntimeError("Expected one Hash node building Q21's l3 input")
    return targets[0], hashes


def _describe_spill(level, stderr, file_count, spill_bytes, write_bytes):
    plan = _read_q21_plan(stderr)

    print(f"Q21 {level} diagnostic: {file_count} files, {spill_bytes} logged bytes, "
          f"{write_bytes} bytes written by temp-file calls", flush=True)
    nodes = [plan]
    for node in nodes:
        children = node.get("Plans", [])
        nodes.extend(children)
        read_blocks = node.get("Temp Read Blocks", 0) - sum(c.get("Temp Read Blocks", 0) for c in children)
        write_blocks = node.get("Temp Written Blocks", 0) - sum(c.get("Temp Written Blocks", 0) for c in children)
        kind = node["Node Type"]
        if kind == "Hash" or read_blocks or write_blocks:
            print(f"Q21 {level} {kind} {node.get('Join Type', '')} "
                  f"{node.get('Hash Cond', '')}: rows={node['Actual Rows']}, "
                  f"buckets={node.get('Hash Buckets')}/{node.get('Original Hash Buckets')}, "
                  f"batches={node.get('Hash Batches')}/{node.get('Original Hash Batches')}, "
                  f"local_temp_read_blocks={read_blocks}, local_temp_write_blocks={write_blocks}", flush=True)


def _restore_postgres(pg_ctl, start_command):
    if subprocess.run([*pg_ctl, "status"], stdout=subprocess.DEVNULL).returncode == 0:
        subprocess.run([*pg_ctl, "-m", "fast", "stop"], check=True)
    subprocess.run(start_command, check=True)


def test(csv_path, *, retest=False, repeat_upper=False, preset_batches=False):
    """Run the cold sweep or refresh selected levels in the existing CSV."""
    import fcntl
    from m1a_peak_mem import DEFAULT_CSV as PEAK_CSV

    mem_total_kib = int(Path("/proc/meminfo").read_text().splitlines()[0].split()[1])
    if not 29 * 2**20 <= mem_total_kib <= 30 * 2**20:
        raise RuntimeError(f"M1c requires the 30 GiB Guest (MemTotal: {mem_total_kib / 2**20:.2f} GiB)")
    pgdata = Path("/data/tpch_s8")
    pg_bin = Path("/home/femu/opt/postgres/release/bin")
    # Q21 retains its Hash Anti Join across all ten settings with the SF8 statistics.
    query = GUEST_TESTS / "queries" / "sf8_sel0.1" / "21.sql"
    if not query.is_file():
        raise ValueError(f"Missing formal Q21: {query}")
    with PEAK_CSV.open(newline="") as stream:
        peak_mib = next(
            (float(row["peak_mem"]) for row in csv.DictReader(stream) if row["query"] == "Q21"),
            None,
        )
    if peak_mib is None or not math.isfinite(peak_mib) or not 0 < peak_mib < 2048:
        raise ValueError("M1a must contain a positive Q21 peak below the 2 GiB baseline")
    # PostgreSQL accepts integer KiB. Round down so every setting stays within
    # its intended fraction of the whole-query anonymous RSS peak from M1a.
    budgets_kib = [2 * 1024**2, *(int(peak_mib * 1024 * step / 10) for step in range(9, 0, -1))]

    env = {key: value for key, value in os.environ.items() if not key.startswith("PG")}
    env["LC_ALL"] = "C"
    psql = [str(pg_bin / "psql"), "-XqAt", "-v", "ON_ERROR_STOP=1", "-d", "tpch_s8"]
    pg_ctl = [str(pg_bin / "pg_ctl"), "-s", "-w", "-D", str(pgdata)]
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    lock_path = Path(tempfile.gettempdir()) / f"{TEST_NAME}-{os.getuid()}.lock"
    measurements = []
    if retest or repeat_upper:
        with csv_path.open(encoding="utf-8", newline="") as stream:
            reader = csv.reader(stream)
            if tuple(next(reader, ())) != FIELDS:
                raise ValueError("Retest requires an existing CSV with latency components")
            measurements = list(reader)
        if tuple(row[0] for row in measurements) != LEVELS:
            raise ValueError("Retest requires all ten original work_mem levels")
        # Keep the published wall-time baseline for all levels. The fresh
        # baseline below supplies backend CPU for the two new breakdowns.
        previous_baseline_ms = Decimal(measurements[0][2])
    budgets = dict(zip(LEVELS, budgets_kib))
    with lock_path.open("a") as lock, tempfile.TemporaryDirectory(prefix=f"{TEST_NAME}-") as probe_dir, ExitStack() as cleanup:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        trace_path = Path(probe_dir) / "spill.bt"
        trace_path.write_text(SPILL_TRACE)
        settings = subprocess.check_output(
            [*psql, "-c", "SELECT current_setting('server_version_num'), "
             "current_setting('data_directory'), current_setting('shared_buffers'), "
             "current_setting('autovacuum'), current_setting('shared_preload_libraries'), "
             "current_setting('session_preload_libraries'), current_setting('local_preload_libraries')"],
            env=env, text=True,
        ).rstrip("\n").split("|")
        if settings != ["120022", str(pgdata), "2GB", "off", "", "", ""]:
            raise RuntimeError(f"Expected native SF8 PostgreSQL 12.22, observed settings: {settings}")
        subprocess.run(["sudo", "-n", "true"], check=True)
        startup = shlex.split((pgdata / "postmaster.opts").read_text())
        startup_args = startup[3:] if startup[1:3] == ["-D", str(pgdata)] else startup[1:]
        start_pg = [*pg_ctl, "-l", str(pgdata / "log"), "-o", shlex.join(startup_args), "start"]
        if preset_batches:
            executable = pg_bin / "postgres-hash-preset"
            if not executable.is_file():
                raise ValueError(f"Missing experimental PostgreSQL binary: {executable}")
            original_start = [*start_pg[:-1], "-p", startup[0], "start"]
            start_pg[-1:] = ["-p", str(executable), "start"]
            explain_sql = "EXPLAIN (VERBOSE, FORMAT JSON) " + re.sub(
                r"^\\timing.*\n", "", query.read_text(), flags=re.MULTILINE,
            )

        if repeat_upper:
            # Reserve stable round numbers without overwriting earlier repeats.
            next_number = 1 + max((
                int(path.stem.rsplit("_", 1)[1])
                for path in csv_path.parent.glob(f"{csv_path.stem}_*.csv")
                if path.stem.rsplit("_", 1)[1].isdecimal()
            ), default=0)
            rounds = [[], [], []]
            schedule = chain(
                ((None, LEVELS[0]),), ((None, level) for level in DIAGNOSTIC_LEVELS),
                ((i, level) for i in range(len(rounds)) for level in REPEAT_LEVELS),
            )
        elif retest:
            schedule = ((None, level) for level in chain((LEVELS[0],), cycle(RETEST_LEVELS)))
            pair = {}
            attempt = 1
        else:
            schedule = ((None, level) for level in LEVELS)

        for round_index, level in schedule:
            budget_kib = budgets[level]
            work_options = f"-c work_mem={budget_kib}kB -c max_parallel_workers_per_gather=0"
            preset = Q21_HASH_PRESETS.get(level) if preset_batches else None
            if preset is not None:
                # Identify the node on the running server, before the next cold reset.
                preflight = subprocess.run(psql, input=explain_sql, text=True, stdout=subprocess.PIPE,
                                           env=dict(env, PGOPTIONS=work_options), check=True)
                target, _ = _q21_hash_nodes(json.loads(preflight.stdout)[0]["Plan"])
                node_id = target["Hash Plan Node ID"]
            active = subprocess.check_output(
                [*psql, "-c", "SELECT count(*) FROM pg_stat_activity "
                 "WHERE backend_type = 'client backend' AND pid <> pg_backend_pid()"],
                env=env, text=True,
            ).strip()
            if active != "0":
                raise RuntimeError(f"{level}: another PostgreSQL client is connected")
            if preset_batches and level == LEVELS[0]:
                cleanup.callback(_restore_postgres, pg_ctl, original_start)
            subprocess.run([*pg_ctl, "-m", "fast", "stop"], check=True)
            try:
                os.sync()
                subprocess.run(
                    ["sudo", "-n", "sh", "-c", "echo 3 > /proc/sys/vm/drop_caches"],
                    check=True,
                )
            finally:
                subprocess.run(start_pg, check=True)

            # Keep work_mem session-local. Emit every temp-file size to the
            # client and use psql's own statement timing from the formal SQL.
            session_env = dict(
                env,
                PGOPTIONS=work_options + " -c statement_timeout=600s -c log_temp_files=0 "
                "-c client_min_messages=log -c lc_messages=C",
            )
            diagnostic = repeat_upper and level in DIAGNOSTIC_LEVELS
            if diagnostic:
                session_env["PGOPTIONS"] += EXPLAIN_OPTIONS + " -c auto_explain.log_buffers=on"
            if preset_batches:
                session_env["PGOPTIONS"] += EXPLAIN_OPTIONS + " -c auto_explain.log_verbose=on"
                if preset is not None:
                    batches, buckets = preset
                    session_env["PGOPTIONS"] += f" -c experiment.hash_batch_preset={node_id}:{batches}:{buckets}"
            prefix = f"Q21 round {next_number + round_index} {level}" if round_index is not None else f"Q21 {level}"
            print(f"{prefix}: started ({budget_kib} KiB)", flush=True)
            stdout, stderr, read_ns, write_ns, cpu_ns, write_bytes = _profile_query(
                psql, query, session_env, trace_path,
            )
            timings = re.findall(r"^Time: (\d+(?:\.\d+)?) ms\b", stdout, re.MULTILINE)
            if len(timings) != 1:
                raise RuntimeError(f"Q21 {level}: expected one statement timing, got {timings}")
            latency_ms = Decimal(timings[0])
            spill_files = re.findall(
                r'\bLOG:\s+temporary file: path "[^"]+", size (\d+)\s*$', stderr,
                re.MULTILINE,
            )
            spill_bytes = sum(map(int, spill_files))
            if bool(spill_bytes) != bool(write_bytes) or write_bytes < spill_bytes:
                raise RuntimeError(f"Q21 {level}: temporary-file trace and logged file sizes disagree")
            rows = sorted(line for line in stdout.splitlines() if not line.startswith("Time: "))
            if level == ">peak":
                if spill_bytes:
                    raise RuntimeError(f"The 2 GiB baseline spilled {spill_bytes} bytes")
                baseline_ms, baseline_rows, baseline_cpu_ns = latency_ms, rows, cpu_ns
                if retest or repeat_upper:
                    print(f"Q21 retest baseline: {latency_ms:.3f} ms wall, "
                          f"{cpu_ns / 1e6:.3f} ms CPU; using {previous_baseline_ms:.3f} ms "
                          "as the existing wall-time reference", flush=True)
                    baseline_ms = previous_baseline_ms
                    continue
            elif rows != baseline_rows:
                raise RuntimeError(f"Q21 {level}: result differs from the no-spill baseline")
            if diagnostic:
                _describe_spill(level, stderr, len(spill_files), spill_bytes, write_bytes)
                continue
            if preset_batches:
                target, hashes = _q21_hash_nodes(_read_q21_plan(stderr))
                if any(node["Original Hash Batches"] != node["Hash Batches"] for node in hashes):
                    raise RuntimeError(f"Q21 {level}: a hash table expanded its batches")
                if any(node["Peak Memory Usage"] > budget_kib for node in hashes):
                    raise RuntimeError(f"Q21 {level}: a hash table exceeded work_mem")
                if preset is not None and (
                    target["Hash Plan Node ID"] != node_id
                    or (target["Hash Batches"], target["Hash Buckets"]) != preset
                    or target["Original Hash Buckets"] != target["Hash Buckets"]
                ):
                    raise RuntimeError(f"Q21 {level}: l3 hash did not use its preset layout")
                print(f"Q21 {level}: l3 initial/final batches {target['Original Hash Batches']}/"
                      f"{target['Hash Batches']}, buckets {target['Hash Buckets']}, "
                      f"peak {target['Peak Memory Usage']} KiB", flush=True)
            extra_ms = latency_ms - baseline_ms
            read_ms, write_ms, extra_cpu_ms = (
                (Decimal(ns) / 1_000_000).quantize(Decimal("0.001"))
                for ns in (read_ns, write_ns, cpu_ns - baseline_cpu_ns)
            )
            # Keep the residual signed. It includes any changes in base-table
            # I/O and scheduling, and preserves the exact sum at CSV precision.
            other_ms = extra_ms - read_ms - write_ms - extra_cpu_ms
            measurement = (
                level, spill_bytes, f"{latency_ms:.3f}", f"{extra_ms:.3f}",
                f"{read_ms:.3f}", f"{write_ms:.3f}", f"{extra_cpu_ms:.3f}", f"{other_ms:.3f}",
            )
            print(f"{prefix}: {spill_bytes} spilled bytes, {latency_ms:.3f} ms", flush=True)
            if repeat_upper:
                rounds[round_index].append(measurement)
                _write_csv(csv_path.with_stem(f"{csv_path.stem}_{next_number + round_index}"), rounds[round_index])
                continue
            if not retest:
                measurements.append(measurement)
                continue

            if spill_bytes != RETEST_SPILLS[RETEST_LEVELS.index(level)]:
                raise RuntimeError(f"Q21 {level}: observed a different spill size in the targeted retest")
            pair[level] = measurement
            if level == RETEST_LEVELS[-1]:
                smaller, larger = (Decimal(pair[key][2]) for key in RETEST_LEVELS)
                print(f"Q21 retest pair {attempt}: {smaller:.3f} ms, {larger:.3f} ms", flush=True)
                if larger > smaller:
                    measurements = [pair.get(row[0], row) for row in measurements]
                    break
                attempt += 1

        if repeat_upper:
            # The middle latency is closest in total to the other two trials.
            # Select its whole row so spill size and delay components stay paired.
            representative = {
                level: sorted(samples, key=lambda sample: Decimal(sample[2]))[1]
                for level, samples in zip(REPEAT_LEVELS, zip(*rounds), strict=True)
            }
            measurements = [representative.get(row[0], row) for row in measurements]
        _write_csv(csv_path, measurements)


def plot(csv_path):
    """Plot spill volume and the signed delay components at each work_mem."""
    with csv_path.open(encoding="utf-8", newline="") as stream:
        reader = csv.reader(stream)
        header = tuple(next(reader, ()))
        if header not in (FIELDS, BASE_FIELDS):
            raise ValueError(f"Expected M1c CSV columns: {', '.join(FIELDS)}")
        rows = list(reader)
    if any(len(row) != len(header) for row in rows) or tuple(row[0] for row in rows) != LEVELS:
        raise ValueError("Expected all ten work_mem levels in order")
    has_breakdown = any(value for row in rows for value in row[4:])
    if has_breakdown and any(not all(row[4:]) for row in rows):
        raise ValueError("The latency breakdown must be complete for all levels")

    spill_mib = []
    delays = []
    for row in rows:
        level, size, *times = row if has_breakdown else row[:4]
        spilled_bytes = int(size)
        latency, extra, *components = (float(value) / 1000 for value in times)
        if spilled_bytes < 0 or latency <= 0 or not all(map(math.isfinite, (latency, extra, *components))):
            raise ValueError(f"{level}: invalid spill size or execution time")
        if has_breakdown and (
            min(components[:2]) < 0 or not math.isclose(sum(components), extra, abs_tol=1e-9)
        ):
            raise ValueError(f"{level}: latency components do not add up to extra delay")
        spill_mib.append(spilled_bytes / 2**20)
        delays.append((extra, *components))
    if spill_mib[0] != 0 or delays[0][0] != 0:
        raise ValueError("Expected a zero-spill baseline with zero extra delay")

    import matplotlib as mpl

    mpl.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.ticker import StrMethodFormatter

    mpl.rcParams.update({
        "font.family": "sans-serif",
        "font.sans-serif": ["DejaVu Sans", "Arial", "Helvetica"],
        "font.size": 8,
        "axes.spines.top": False,
        "axes.axisbelow": True,
        "svg.fonttype": "none",
        "pdf.fonttype": 42,
    })
    fig, delay_axis = plt.subplots(figsize=(7.2, 3.5), layout="constrained")
    spill_axis = delay_axis.twinx()
    positions = range(len(rows))
    delay_axis.set_xticks(positions, [">1", *(level.removesuffix("peak") for level in LEVELS[1:])])
    delay_axis.set(xlabel="database work_mem (× peak memory)", ylabel="extra delay (s)")
    delay_axis.axhline(0, color="#555555", lw=0.65)
    delay_axis.grid(axis="y", color="#dce3e9", lw=0.55)
    delay_axis.spines["right"].set_visible(False)

    positives = [0.0] * len(rows)
    negatives = [0.0] * len(rows)
    layers = (
        ("Temp read", "#3e8eae"),
        ("Temp write", "#19796c"),
        ("Extra compute", "#d38b47"),
        ("Other", "#8c849b"),
    ) if has_breakdown else (("Extra delay", "#d38b47"),)
    for index, (label, color) in enumerate(layers, start=int(has_breakdown)):
        heights = [values[index] for values in delays]
        bottoms = [positives[i] if height >= 0 else negatives[i] for i, height in enumerate(heights)]
        delay_axis.bar(positions, heights, bottom=bottoms, label=label, color=color, width=0.65,
                       edgecolor="#333333", linewidth=0.5)
        for i, height in enumerate(heights):
            positives[i] += max(height, 0)
            negatives[i] += min(height, 0)
    if has_breakdown and min(negatives) < 0:
        # A tick shows the algebraic sum when a component extends below zero.
        delay_axis.scatter(positions, [values[0] for values in delays], color="#222222",
                           marker="_", s=45, label="Net extra delay", zorder=3)

    spill_color = "#2868A7"
    spill_axis.set(ylabel="spilled bytes (MiB)", ylim=(-45, max(spill_mib) * 1.2 + 40))
    spill_axis.yaxis.label.set_color("black")
    spill_axis.tick_params(axis="y", colors="black")
    spill_axis.spines["left"].set_visible(False)
    spill_axis.spines["right"].set_color("black")
    spill_axis.yaxis.set_major_formatter(StrMethodFormatter("{x:.1f}"))
    spill_points = spill_axis.scatter(positions, spill_mib, color=spill_color, s=26,
                                     edgecolor="white", linewidth=0.55, label="Spilled bytes", zorder=4)
    for x, volume in zip(positions, spill_mib):
        spill_axis.annotate(f"{volume:.1f}", (x, volume), xytext=(0, 5), textcoords="offset points",
                            ha="center", color="black", fontsize=7)
    handles, labels = delay_axis.get_legend_handles_labels()
    delay_axis.legend([*handles, spill_points], [*labels, spill_points.get_label()],
                      frameon=False, loc="lower center", bbox_to_anchor=(0.5, 1.01), ncol=3)

    for extension in ("svg", "pdf"):
        fig.savefig(csv_path.with_suffix(f".{extension}"), format=extension)
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("test", "plot"))
    parser.add_argument("--csv", type=Path, help="Override the default CSV under results/m1/.")
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument("--retest", action="store_true", help="Repeat the 0.6 and 0.5 peak pair until latency is ordered.")
    selection.add_argument("--repeat-upper", action="store_true", help="Record three cold rounds from 0.9 to 0.5 peak and diagnose 0.4/0.3 peak.")
    selection.add_argument("--preset-batches", action="store_true", help="Use the separate hash-batch preset comparison CSV.")
    args = parser.parse_args()
    suffix = "_preset" if args.preset_batches else ""
    csv_path = args.csv or RESULTS_DIR / f"{TEST_NAME}{suffix}.csv"
    try:
        if args.mode == "test":
            test(csv_path, retest=args.retest, repeat_upper=args.repeat_upper, preset_batches=args.preset_batches)
        else:
            if args.retest or args.repeat_upper:
                parser.error("--retest and --repeat-upper are only available for test")
            plot(csv_path)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
