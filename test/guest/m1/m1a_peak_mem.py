#!/usr/bin/env python3
"""Measure SF8 whole-query memory in the Guest, or plot the resulting CSV.

Run the test mode in the Guest and the plot mode with Matplotlib installed.
Both modes default to results/m1/ under the project root. Use --csv to
select a different CSV.

CSV fields
    query      TPC-H query identifier, Q1 through Q22.
    peak_mem   Maximum sampled query memory (MiB). Each sample sums the
               leader's anonymous RSS increase from its pre-query baseline
               and the anonymous RSS of its concurrent parallel workers.
"""

import argparse
import csv
import math
import os
from pathlib import Path
import selectors
import shlex
import subprocess
import tempfile
import time


TEST_NAME = Path(__file__).stem
PROJECT_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_CSV = PROJECT_ROOT / "results" / "m1" / f"{TEST_NAME}.csv"
GUEST_TESTS = PROJECT_ROOT / "tests"
if (GUEST_TESTS / "guest").is_dir():
    GUEST_TESTS /= "guest"
QUERIES = tuple(f"Q{number}" for number in range(1, 23))
# DEVICE_WORK_MEMORY_MIB = 1024 - 128
DEVICE_WORK_MEMORY_MIB = 1024


def test(csv_path, queries=QUERIES, workers=0):
    """Measure concurrent leader and worker anonymous RSS for each cold query."""
    import fcntl

    pgdata = Path("/data/tpch_s8")
    pg_bin = Path("/home/femu/opt/postgres/release/bin")
    query_dir = GUEST_TESTS / "queries" / "sf8_sel0.1"
    for query in queries:
        if not (query_dir / f"{query[1:]}.sql").is_file():
            raise ValueError(f"{query}: missing formal SQL in {query_dir}")

    env = {key: value for key, value in os.environ.items() if not key.startswith("PG")}
    # Use the same per-operator setting for serial and parallel samples.
    env["PGOPTIONS"] = (
        f"-c work_mem=2GB -c max_parallel_workers_per_gather={workers} -c statement_timeout=600s"
    )
    psql = [str(pg_bin / "psql"), "-XqAt", "-v", "ON_ERROR_STOP=1", "-d", "tpch_s8"]
    pg_ctl = [str(pg_bin / "pg_ctl"), "-s", "-w", "-D", str(pgdata)]
    # Sampling every 2 ms captures short-lived states without storing a trace.
    sample_interval = 0.002

    def anonymous_rss_kib(backend_pid, *, worker=False):
        # RssAnon isolates backend anonymous resident pages from shared buffers
        # and file mappings, which would otherwise inflate the peak.
        with Path(f"/proc/{backend_pid}/status").open() as status:
            for line in status:
                if line.startswith("RssAnon:"):
                    return int(line.split()[1])
        if worker:
            return 0  # An exited worker can still have a /proc entry.
        raise RuntimeError(f"Missing RssAnon for PostgreSQL backend {backend_pid}")

    def worker_rss_kib(children, title):
        samples = []
        for pid in children.read_text().split():
            proc = Path(f"/proc/{pid}")
            try:
                cmdline = (proc / "cmdline").read_bytes().split(b"\0", 1)[0].rstrip()
                if cmdline.endswith(title):
                    kib = anonymous_rss_kib(pid, worker=True)
                    if kib:
                        samples.append(kib)
            except (FileNotFoundError, ProcessLookupError):
                continue  # A worker can exit between PID discovery and sampling.
        return samples

    csv_path.parent.mkdir(parents=True, exist_ok=True)
    lock_path = Path(tempfile.gettempdir()) / f"{TEST_NAME}-{os.getuid()}.lock"
    rows = []
    with lock_path.open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        settings_sql = """
            SELECT current_setting('server_version_num'), current_setting('data_directory'),
                   current_setting('shared_buffers'), current_setting('autovacuum'),
                   current_setting('shared_preload_libraries'),
                   current_setting('session_preload_libraries'),
                   current_setting('local_preload_libraries')
        """
        settings = subprocess.check_output(
            [*psql, "-c", settings_sql], env=env, text=True
        ).rstrip("\n").split("|")
        if settings != ["120022", str(pgdata), "2GB", "off", "", "", ""]:
            raise RuntimeError(
                "Start native PostgreSQL 12.22 for /data/tpch_s8 with "
                f"shared_buffers=2GB and autovacuum=off. Observed settings: {settings}"
            )
        mem_total_kib = int(Path("/proc/meminfo").read_text().splitlines()[0].split()[1])
        if not 29 * 2**20 <= mem_total_kib <= 30 * 2**20:
            raise RuntimeError("M1a requires the 30 GiB Guest")
        subprocess.run(["sudo", "-n", "true"], check=True)
        # Reuse the current server arguments across cold restarts.
        startup = shlex.split((pgdata / "postmaster.opts").read_text())
        startup_args = startup[3:] if startup[1:3] == ["-D", str(pgdata)] else startup[1:]
        start_pg = [*pg_ctl, "-l", str(pgdata / "log"), "-o", shlex.join(startup_args), "start"]

        for query in queries:
            active = subprocess.check_output(
                [*psql, "-c", "SELECT count(*) FROM pg_stat_activity "
                 "WHERE backend_type = 'client backend' AND pid <> pg_backend_pid()"],
                env=env, text=True,
            ).strip()
            if active != "0":
                raise RuntimeError(f"{query}: another PostgreSQL client is connected")
            subprocess.run([*pg_ctl, "-m", "fast", "stop"], check=True)
            try:
                os.sync()
                subprocess.run(
                    ["sudo", "-n", "sh", "-c", "echo 3 > /proc/sys/vm/drop_caches"],
                    check=True,
                )
            finally:
                subprocess.run(start_pg, check=True)
            postmaster = (pgdata / "postmaster.pid").read_text().splitlines()[0]
            children = Path(f"/proc/{postmaster}/task/{postmaster}/children")

            print(f"{query} started", flush=True)
            with subprocess.Popen(
                psql, cwd=query_dir, env=env, stdin=subprocess.PIPE,
                stdout=subprocess.PIPE, bufsize=0,
            ) as client, selectors.DefaultSelector() as output:
                try:
                    client.stdin.write(b"SELECT pg_backend_pid();\n")
                    backend_pid = int(client.stdout.readline())
                    # Subtract the idle leader footprint. Query-launched workers
                    # have no pre-query baseline. Sum each sample before taking
                    # the maximum, since participants can peak at different times.
                    baseline_kib = anonymous_rss_kib(backend_pid)
                    peak_kib = peak_leader_kib = peak_worker_kib = 0
                    max_workers = peak_workers = 0
                    worker_title = f"parallel worker for PID {backend_pid}".encode()
                    marker = f"__M1A_DONE_{backend_pid}__\n".encode()
                    output.register(client.stdout, selectors.EVENT_READ)
                    client.stdin.write(b"\\o /dev/null\n")
                    next_sample = time.monotonic() + sample_interval
                    client.stdin.write(f"\\i {query[1:]}.sql\n\\echo ".encode() + marker)
                    pending = b""
                    completed = False
                    while not completed:
                        if output.select(max(0, next_sample - time.monotonic())):
                            chunk = os.read(client.stdout.fileno(), 4096)
                            if not chunk:
                                break
                            pending += chunk
                            completed = marker in pending.splitlines(keepends=True)
                            pending = pending.rpartition(b"\n")[2]
                        now = time.monotonic()
                        if now >= next_sample or completed:
                            leader_kib = anonymous_rss_kib(backend_pid) - baseline_kib
                            worker_samples = worker_rss_kib(children, worker_title) if workers else []
                            worker_kib = sum(worker_samples)
                            max_workers = max(max_workers, len(worker_samples))
                            if leader_kib + worker_kib > peak_kib:
                                peak_kib = leader_kib + worker_kib
                                peak_leader_kib, peak_worker_kib = leader_kib, worker_kib
                                peak_workers = len(worker_samples)
                            next_sample = max(next_sample + sample_interval, now)
                    client.stdin.close()
                    if client.wait() or not completed:
                        raise RuntimeError(f"{query}: formal SQL did not complete")
                except BaseException:
                    client.terminate()
                    raise
            peak_mib = peak_kib / 1024
            rows.append((query, f"{peak_mib:.6f}"))
            print(
                f"{query} peak {peak_mib:.2f} MiB, leader {peak_leader_kib / 1024:.2f} MiB, "
                f"workers {peak_worker_kib / 1024:.2f} MiB across {peak_workers} at peak, "
                f"{max_workers} observed concurrently", flush=True,
            )

    # Publish the final CSV only after all queries have completed successfully.
    with tempfile.TemporaryDirectory(dir=csv_path.parent) as temp_dir:
        temporary_csv = Path(temp_dir) / csv_path.name
        with temporary_csv.open("w", encoding="utf-8", newline="") as stream:
            writer = csv.writer(stream, lineterminator="\n")
            writer.writerow(("query", "peak_mem"))
            writer.writerows(rows)
        temporary_csv.replace(csv_path)


def plot(csv_path):
    """Plot the final CSV and write an SVG and a PDF beside it."""
    with csv_path.open(encoding="utf-8", newline="") as stream:
        reader = csv.reader(stream)
        if next(reader, None) != ["query", "peak_mem"]:
            raise ValueError("Expected CSV columns query,peak_mem")
        rows = list(reader)
    if not rows or any(len(row) != 2 for row in rows):
        raise ValueError("Expected two-column rows for the measured queries")
    labels = tuple(row[0] for row in rows)
    if labels != tuple(query for query in QUERIES if query in labels):
        raise ValueError("Expected unique TPC-H query labels in query order")
    peaks = [float(value) for _, value in rows]
    if any(not math.isfinite(peak) or peak < 0 for peak in peaks):
        raise ValueError("peak_mem must contain finite, nonnegative MiB values")

    import matplotlib as mpl

    mpl.use("Agg")
    import matplotlib.pyplot as plt

    mpl.rcParams.update({
        "font.family": "sans-serif",
        "font.sans-serif": ["DejaVu Sans", "Arial", "Helvetica"],
        "font.size": 8,
        "axes.spines.top": False,
        "axes.spines.right": False,
        "axes.axisbelow": True,
        "xtick.labelsize": 7,
        "ytick.labelsize": 7,
        "svg.fonttype": "none",
        "pdf.fonttype": 42,
    })
    fig, ax = plt.subplots(figsize=(7, 3.2), layout="constrained")
    ax.axhline(DEVICE_WORK_MEMORY_MIB, ls="--", lw=1, color="#8f4b28",
               label=f"device work memory ({DEVICE_WORK_MEMORY_MIB} MiB)")
    for query, peak in zip(labels, peaks):
        exceeds = peak > DEVICE_WORK_MEMORY_MIB
        ax.plot(query, peak, ls="none", marker="D" if exceeds else "o",
                markersize=4 if exceeds else 3.3, color="#a44826" if exceeds else "#086d78")
    ax.set_yscale("symlog", base=2, linthresh=1, linscale=0.4)
    ymax = max(DEVICE_WORK_MEMORY_MIB, max(peaks)) * 1.42
    ticks = [value for value in (0, 1, 4, 16, 64, 256, 1024, 4096, 8192, 16384, 32768) if value < ymax]
    ax.set_yticks(ticks, [f"{value:,}" for value in ticks])
    ax.set_ylim(-0.4, ymax)
    ax.set(xlabel="TPC-H query", ylabel="peak memory (MiB)")
    ax.grid(axis="y", color="#dce3e9", lw=0.55)
    ax.legend(frameon=False, loc="lower right", bbox_to_anchor=(1, 1.03))
    for extension in ("svg", "pdf"):
        fig.savefig(csv_path.with_suffix(f".{extension}"), format=extension)
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("test", "plot"))
    parser.add_argument("--csv", type=Path, default=DEFAULT_CSV, help="CSV path (default: %(default)s)")
    parser.add_argument("--query", choices=QUERIES, help="Test only this query.")
    parser.add_argument("--workers", type=int, default=0, help="Maximum workers per Gather (default: 0).")
    args = parser.parse_args()
    try:
        if args.mode == "test":
            if args.workers < 0:
                parser.error("--workers must be nonnegative")
            if (args.query or args.workers) and args.csv == DEFAULT_CSV:
                parser.error("Specify --csv to save a targeted or parallel sample separately.")
            test(args.csv, (args.query,) if args.query else QUERIES, args.workers)
        else:
            plot(args.csv)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
