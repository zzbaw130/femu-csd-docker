#!/usr/bin/env python3
"""Targeted cold SF8 A/B with a paced FIO read probe and NVMe tracepoints.

CSV fields, in order:
query: official SF8 query number at target selectivity 0.1.
policy: legacy or priority FEMU PCIe behavior.
runs: number of valid cold runs included in the row.
read_n, read_p95_us, read_p99_us, read_max_us: FIO NVMe reads sampled,
    and nearest-rank tail latency and maximum, in microseconds.
afdm_n, afdm_p95_us, afdm_p99_us, afdm_max_us: query READ_AFDM
    commands sampled, and nearest-rank tail latency and maximum, in microseconds.
published_mib: device bytes successfully published to the Guest receive ring,
    summed across runs and expressed in MiB.
dropped_mib: device queue, credit, overwrite, error and STOP dropped bytes
    summed across runs in MiB. Already-resident bytes are excluded.

Percentiles pool request samples from cold runs of one query and one policy.
The raw JSON records timestamps, request lengths, query output, and the byte
counters used to calculate these fields. Plotting reads only the CSV.
"""

import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import tempfile
import time


PG = Path("/home/femu/opt/postgres/release/bin")
PGDATA = Path("/data/tpch_s8")
BUILD = Path("/home/femu/.cache/femu-pcie-priority-ab")
SQL_DIR = Path("/home/femu/tests/queries/sf8_sel0.1")
PG_START = (
    "-c autovacuum=off -c shared_buffers=2GB -c work_mem=1GB "
    "-c max_parallel_workers_per_gather=8 -c max_parallel_workers=16 "
    "-c max_worker_processes=16 -c enable_hashagg=off"
)
PG_OPTIONS = """
SET statement_timeout='300s';
SET pg_sina_csd.exe_path='csd-with-hpcp';
SET pg_sina_csd.experimental_operator_graph=on;
SET pg_sina_csd.hpcp=on;
SET pg_sina_csd.dcp=on;
SET pg_sina_csd.profile=on;
SET auto_explain.log_min_duration=0;
SET auto_explain.log_analyze=on;
SET auto_explain.log_timing=off;
SET auto_explain.log_level=notice;
"""
TRACE = r"""
BEGIN { printf("trace_ready\n"); }

tracepoint:nvme:nvme_setup_cmd
/args->ctrl_id == 0 && args->qid > 0 &&
 ((args->opcode == 0x02 && comm == "fio") || args->opcode == 0xf2)/
{
    $qid = (uint32)args->qid;
    $cid = (uint32)args->cid;
    @started[$qid, $cid] = nsecs;
    @opcode[$qid, $cid] = args->opcode;
    $lo = (uint32)args->cdw10[8] | ((uint32)args->cdw10[9] << 8);
    $hi = (uint32)args->cdw10[10] | ((uint32)args->cdw10[11] << 8);
    @bytes[$qid, $cid] = args->opcode == 0x02 ? ($lo + 1) * 512 : $lo | ($hi << 16);
}

tracepoint:nvme:nvme_complete_rq
/args->ctrl_id == 0 && args->qid > 0/
{
    $qid = (uint32)args->qid;
    $cid = (uint32)args->cid;
    $start = @started[$qid, $cid];
    if ($start) {
        printf("io,%d,%llu,%llu,%u,%u\n", @opcode[$qid, $cid], $start,
               nsecs - $start, @bytes[$qid, $cid], args->status);
        delete(@started[$qid, $cid]);
        delete(@opcode[$qid, $cid]);
        delete(@bytes[$qid, $cid]);
    }
}

END { clear(@started); clear(@opcode); clear(@bytes); }
"""
QMP = """
import json, socket, sys
with socket.socket(socket.AF_UNIX) as client:
    client.connect("./qmp-sock")
    stream = client.makefile("rwb")
    stream.readline()
    def cmd(name, **arguments):
        stream.write(json.dumps({"execute": name, "arguments": arguments}).encode() + b"\\n")
        stream.flush()
        while True:
            message = json.loads(stream.readline())
            if "error" in message:
                raise RuntimeError(message["error"])
            if "return" in message:
                return message["return"]
    cmd("qmp_capabilities")
    paths = []
    for group in ("peripheral", "peripheral-anon"):
        root = "/machine/" + group
        paths.extend(root + "/" + e["name"] for e in cmd("qom-list", path=root)
                     if e["type"] == "child<femu>")
    path, = paths
    if len(sys.argv) > 1:
        legacy = sys.argv[1] == "legacy"
        cmd("qom-set", path=path, property="x-pcie-ab-legacy", value=legacy)
        assert cmd("qom-get", path=path, property="x-pcie-ab-legacy") == legacy
    print(cmd("qom-get", path=path, property="x-pcie-ab-published-bytes"))
"""
NAME = Path(__file__).stem
OUT = Path(__file__).resolve().parents[2] / "results" / NAME
STATS = ["n", "p95_us", "p99_us", "max_us"]
FIELDS = (
    ["query", "policy", "runs"]
    + [f"{prefix}_{stat}" for prefix in ("read", "afdm") for stat in STATS]
    + ["published_mib", "dropped_mib"]
)


def command(args, **kwargs):
    return subprocess.run([str(arg) for arg in args], text=True, check=True, **kwargs)


def cache_counts():
    output = command(["sudo", "-n", "cat", "/sys/kernel/debug/femu-cache/profile"],
                     capture_output=True).stdout
    return {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", output)}


def guest_run(query, probe):
    """Capture on the Guest, where FIO and NVMe timestamps share a clock."""
    assert PGDATA.is_dir() and probe.stat().st_size >= 1024**3
    assert 29_000_000 < int(Path("/proc/meminfo").read_text().split()[1]) < 31_500_000
    sql = SQL_DIR / f"{query}.sql"
    command([PG / "pg_ctl", "-D", PGDATA, "-m", "fast", "-w", "stop"],
            stdout=subprocess.DEVNULL)
    command(["sync"])
    command(["sudo", "-n", "sh", "-c", "echo 3 > /proc/sys/vm/drop_caches"])
    command([PG / "pg_ctl", "-D", PGDATA, "-l", PGDATA / "log", "-w", "start",
             "-o", PG_START], stdout=subprocess.DEVNULL)
    before = cache_counts()
    env = dict(os.environ, LC_ALL="C", PGOPTIONS=(
        "-c session_preload_libraries=" + str(BUILD / "pg/pg_sina_csd.so")
        + ",auto_explain"
    ), LD_LIBRARY_PATH=str(BUILD / "prefix/lib"), SNIA_FEMU_PROFILE="cache")
    with tempfile.TemporaryDirectory(prefix=NAME) as directory:
        temp = Path(directory)
        trace_file = temp / "trace.bt"
        trace_file.write_text(TRACE)
        with (temp / "trace").open("w") as trace, (temp / "trace.err").open("w") as trace_err, \
             (temp / "fio.json").open("w") as fio_json:
            tracer = subprocess.Popen(
                ["sudo", "-n", "env", "BPFTRACE_PERF_RB_PAGES=128", "bpftrace",
                 "-q", "-B", "line", str(trace_file)], stdout=trace, stderr=trace_err,
                start_new_session=True,
            )
            fio = None
            try:
                for _ in range(100):
                    if "trace_ready" in (temp / "trace").read_text():
                        break
                    if tracer.poll() is not None:
                        break
                    time.sleep(0.05)
                else:
                    raise RuntimeError("NVMe tracer startup timed out")
                if tracer.poll() is not None:
                    raise RuntimeError((temp / "trace.err").read_text())
                fio = subprocess.Popen(
                    ["/usr/bin/fio", "--readonly", "--name=pcie-probe",
                     f"--filename={probe}", "--allow_file_create=0",
                     "--rw=randread", "--direct=1", "--ioengine=psync",
                     "--iodepth=1", "--numjobs=1", "--thread=1", "--bs=16k",
                     "--size=1G", "--rate_iops=1000", "--randrepeat=1",
                     "--randseed=42042", "--time_based=1", "--runtime=330",
                     "--eta=never", "--output-format=json"],
                    stdout=fio_json, stderr=subprocess.STDOUT,
                )
                for _ in range(200):
                    if "io,2," in (temp / "trace").read_text():
                        break
                    if fio.poll() is not None:
                        raise RuntimeError((temp / "fio.json").read_text())
                    time.sleep(0.05)
                else:
                    raise RuntimeError("No FIO NVMe read reached the tracepoint")
                begin = time.monotonic_ns()
                sql_result = subprocess.run(
                    [str(PG / "psql"), "-XqAt", "-v", "ON_ERROR_STOP=1",
                     "-d", "tpch_s8", "-f", "-", "-f", str(sql)],
                    input=PG_OPTIONS, env=env, capture_output=True, text=True,
                    timeout=320,
                )
                end = time.monotonic_ns()
            finally:
                if fio is not None and fio.poll() is None:
                    fio.send_signal(signal.SIGINT)
                    fio.wait(timeout=10)
                if tracer.poll() is None:
                    command(["sudo", "-n", "kill", "-INT", "--", f"-{tracer.pid}"])
                    tracer.wait(timeout=10)
        trace_error = (temp / "trace.err").read_text()
        entries = [tuple(map(int, line.split(",")[1:]))
                   for line in (temp / "trace").read_text().splitlines()
                   if line.startswith("io,")]
        samples = [entry for entry in entries if begin <= entry[1] <= end - entry[2]]
        fio_output = (temp / "fio.json").read_text()
        fio_status = json.loads(fio_output[fio_output.index("{"):])["jobs"][0]
    after = cache_counts()
    rows = [line for line in sql_result.stdout.splitlines()
            if not line.startswith(("Time:", "Timing is "))]
    return {
        "sql_sha256": hashlib.sha256(sql.read_bytes()).hexdigest(),
        "rows": rows,
        "query_ok": sql_result.returncode == 0,
        "notices": sql_result.stderr,
        "sql_ms": [float(t) for t in re.findall(r"^Time: (\d+(?:\.\d+)?) ms",
                                              sql_result.stdout, re.MULTILINE)],
        "sample_window_ns": [begin, end],
        "samples": samples,
        "fio_error": fio_status["error"],
        "fio_total_ios": fio_status["read"]["total_ios"],
        "trace_error": trace_error,
        "counters": {key: after[key] - before[key] for key in after},
    }


def stats(samples, prefix):
    if not samples:
        return {f"{prefix}_{key}": (0 if key == "n" else "") for key in STATS}
    samples.sort()
    result = [len(samples), samples[math.ceil(len(samples) * 0.95) - 1] / 1000,
              samples[math.ceil(len(samples) * 0.99) - 1] / 1000, samples[-1] / 1000]
    return {f"{prefix}_{key}": value for key, value in zip(STATS, result)}


def test(args):
    out = args.output
    out.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).read_text()
    docker = ["docker", "exec", args.container]
    guest = ["docker", "exec", "-i", args.container, "./run-guest-ssh.sh"]
    qmp = [*docker, "python3", "-c", QMP]
    results = {}
    valid = True
    for rep in range(1, args.reps + 1):
        order = ("legacy", "priority") if rep % 2 else ("priority", "legacy")
        for query in args.queries:
            paired = []
            for policy in order:
                initial = int(command([*qmp, policy], capture_output=True).stdout)
                remote = shlex.join(["python3", "-", "guest-run", str(query), str(args.probe)])
                raw = subprocess.run([*guest, remote], input=source, text=True,
                                     capture_output=True, timeout=390)
                if raw.returncode:
                    (out / f"Q{query}-{rep}-{policy}.err").write_text(raw.stderr)
                    raise RuntimeError(raw.stderr)
                row = json.loads(raw.stdout)
                final = int(command(qmp, capture_output=True).stdout)
                row["published_bytes"] = final - initial
                row.update(query=query, policy=policy, rep=rep)
                row["valid"] = (
                    row["query_ok"] and "CSD Executor: SNIA operator graph" in row["notices"]
                    and row["counters"]["candidate_bytes"] > 0 and not row["fio_error"]
                    and not re.search(r"lost\s+\d+\s+events", row["trace_error"], re.I)
                    and any(op == 0x02 for op, *_ in row["samples"])
                    and any(op == 0xf2 for op, *_ in row["samples"])
                    and all(status == 0 for *_, status in row["samples"])
                )
                (out / f"Q{query}-{rep}-{policy}.json").write_text(json.dumps(row))
                print(f"Q{query} run {rep} {policy}: "
                      f"valid={row['valid']} published={row['published_bytes']}", flush=True)
                paired.append(row)
            match = (all(row["valid"] for row in paired)
                     and paired[0]["rows"] == paired[1]["rows"]
                     and paired[0]["sql_sha256"] == paired[1]["sql_sha256"])
            valid &= match
            for row in paired:
                results.setdefault((query, row["policy"]), []).extend([row] if match else [])
    with (out / f"{NAME}.csv").open("w", newline="") as output:
        writer = csv.DictWriter(output, FIELDS)
        writer.writeheader()
        for (query, policy), runs in sorted(results.items()):
            if not runs:
                continue
            writer.writerow(dict(
                query=query, policy=policy, runs=len(runs),
                **stats([lat for row in runs for op, _, lat, *_ in row["samples"] if op == 0x02], "read"),
                **stats([lat for row in runs for op, _, lat, *_ in row["samples"] if op == 0xf2], "afdm"),
                published_mib=sum(row["published_bytes"] for row in runs) / 1024**2,
                dropped_mib=sum(value for row in runs for key, value in row["counters"].items()
                                if key.endswith("_dropped_bytes")) / 1024**2,
            ))
    return 0 if valid else 1


def plot(args):
    import matplotlib.pyplot as plt

    out = args.output
    with (out / f"{NAME}.csv").open() as stream:
        rows = list(csv.DictReader(stream))
    if not rows:
        raise ValueError("No valid pairs to plot")
    metrics = [("read_p99_us", "FIO NVMe read p99 (µs)"),
               ("afdm_p99_us", "READ_AFDM p99 (µs)"),
               ("published_mib", "Published refill (MiB)"),
               ("dropped_mib", "Dropped refill (MiB)")]
    queries = sorted({int(row["query"]) for row in rows})
    positions = {query: index for index, query in enumerate(queries)}
    fig, axes = plt.subplots(2, 2, figsize=(8.4, 5.5), layout="constrained")
    for ax, (field, label) in zip(axes.flat, metrics):
        for index, policy in enumerate(("legacy", "priority")):
            arm = [row for row in rows if row["policy"] == policy]
            ax.bar([positions[int(row["query"])] + (index - 0.5) * 0.34 for row in arm],
                   [float(row[field]) for row in arm], 0.34, label=policy)
        ax.set_xticks(list(positions.values()), [f"Q{query}" for query in queries])
        ax.set_ylabel(label)
    axes[0, 0].legend()
    for suffix in ("svg", "pdf"):
        fig.savefig(out / f"{NAME}.{suffix}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="mode", required=True)
    runner = sub.add_parser("test")
    runner.add_argument("--container", required=True)
    runner.add_argument("--probe", required=True, type=Path)
    runner.add_argument("--queries", nargs="+", type=int, default=[3, 6, 10])
    runner.add_argument("--reps", type=int, default=3)
    visual = sub.add_parser("plot")
    for action in (runner, visual):
        action.add_argument("--output", type=Path, default=OUT)
    internal = sub.add_parser("guest-run", help=argparse.SUPPRESS)
    internal.add_argument("query", type=int)
    internal.add_argument("probe", type=Path)
    args = parser.parse_args()
    if args.mode == "guest-run":
        print(json.dumps(guest_run(args.query, args.probe)))
        return 0
    return (test if args.mode == "test" else plot)(args)


if __name__ == "__main__":
    raise SystemExit(main())
