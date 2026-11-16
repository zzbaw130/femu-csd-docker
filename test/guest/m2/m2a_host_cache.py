#!/usr/bin/env python3
"""Measure native HOST query latency at controlled Guest page-cache residency."""

# HOST grid CSV, in header order:
# query: TPC-H query identifier. target_residency: requested Guest page-cache fraction.
# target_os_pages, relation_os_pages: requested and eligible 4 KiB file-page counts.
# resident_pages_before, resident_pages_after: eligible 4 KiB pages resident before/after SQL.
# actual_residency_before: resident_pages_before / relation_os_pages.
# cache_setup_ms: page-cache preparation time in ms, outside query timing.
# cache_status, results_match, plan_matches: cache, result, and native plan validation.
# speedup: cold HOST latency divided by this sample's latency.
# status: SQL and measurement status. planning_ms, execution_ms, latency_ms: PG planning,
#     SELECT execution, and psql statement times in ms (all three statements for Q15 latency).
# shared_access_blocks, shared_hit_blocks, shared_miss_blocks: total, hit, and read counts in
#     8 KiB PostgreSQL shared buffers. shared_hit_pct: hits as a percentage of accesses.
# temp_read_blocks, temp_written_blocks: temporary 8 KiB blocks read/written by SELECT.
# pg_read_ms, pg_write_ms: cumulative PostgreSQL I/O timing in ms.
# page_cache_access_pages, page_cache_hit_pages, page_cache_miss_pages: 4 KiB Guest demand
#     page-cache lookups, hits, and misses. page_cache_hit_pct: hits as a percentage of lookups.
# nvme_read_requests, nvme_read_bytes, nvme_read_ms, nvme_write_requests, nvme_write_bytes,
#     nvme_write_ms: Guest namespace completion counts, bytes, and cumulative I/O ms.
#
# Filter CSV: same columns in the same order, omitting plan_matches and planning_ms.
#     speedup is cold HOST latency divided by filter_latency_ms. Additional columns:
# actual_path: observed placement. executed_csd_scans: executed device scan count.
# fdm_clean: whether device memory was released after SQL. executor_end_ms: maintenance time.
# filter_latency_ms: psql latency minus maintenance time. host_latency_ms: paired HOST
#     latency at this residency. host_over_filter: host_latency_ms / filter_latency_ms.
#
# File inventory CSV: query, schema, relation, kind (heap/index/TOAST), oid (PG relation ID),
#     fork (PG fork filename), segment (ordinal), path (Guest file), os_pages (4 KiB pages).
# Residency CSV: query, target_residency (fraction), path, os_pages, target_os_pages,
#     resident_pages_before, resident_pages_after (all four counts in 4 KiB pages).

import argparse
from collections import Counter
import csv
from decimal import Decimal
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import re
import select
import shlex
import subprocess
import tempfile
import time

TEST_NAME = Path(__file__).stem
PROJECT_ROOT = Path(__file__).resolve().parents[3]
CSV_PATH = PROJECT_ROOT / "results" / TEST_NAME / f"{TEST_NAME}.csv"
FILTER_CSV = CSV_PATH.with_name(f"{TEST_NAME}_filter_cold.csv")
REFERENCE_LOGS = (
    PROJECT_ROOT / "results" / "tpch-sf8-sel0.1-host-only-30g-w8-m1g-20261111" / "logs" / "cold"
)
PG_BIN = Path("/home/femu/opt/postgres/release/bin")
PG_DATA = Path("/data/tpch_s8")
DATABASE = "tpch_s8"
PAGE_BYTES = 4096
RUN_FIELDS = (
    "status", "planning_ms", "execution_ms", "latency_ms",
    "shared_access_blocks", "shared_hit_blocks", "shared_miss_blocks",
    "shared_hit_pct", "temp_read_blocks", "temp_written_blocks",
    "pg_read_ms", "pg_write_ms", "page_cache_access_pages", "page_cache_hit_pages",
    "page_cache_miss_pages", "page_cache_hit_pct", "nvme_read_requests",
    "nvme_read_bytes", "nvme_read_ms", "nvme_write_requests", "nvme_write_bytes",
    "nvme_write_ms",
)
QUERIES = tuple(range(1, 23))
LEVELS = tuple(range(11))
PG_BLOCK_PAGES = 2
BAND_PAGES = 320
FIELDS = (
    "query", "target_residency", "target_os_pages", "relation_os_pages",
    "resident_pages_before", "resident_pages_after", "actual_residency_before",
    "cache_setup_ms", "cache_status", "results_match", "plan_matches", "speedup",
    *RUN_FIELDS,
)
FILE_FIELDS = ("schema", "relation", "kind", "oid", "fork", "segment", "path", "os_pages")
FILTER_FIELDS = (
    *(field for field in FIELDS if field not in ("plan_matches", "planning_ms")),
    "actual_path", "executed_csd_scans", "fdm_clean", "executor_end_ms",
    "filter_latency_ms", "host_latency_ms", "host_over_filter",
)
FILTER_ENV = (
    "-c session_preload_libraries=pg_sina_csd -c pg_sina_csd.exe_path=filter-only "
    "-c pg_sina_csd.hpcp=off -c pg_sina_csd.dcp=off -c pg_sina_csd.profile=on"
)


PAGE_CACHE_PROBE = r"""
tracepoint:syscalls:sys_enter_openat
/pid == $1/
{
    $marker = str(args.filename);
    if ($marker == "/tmp/.m2a-ping") {
        printf("READY\n");
    } else if ($marker == "/tmp/.m2a-start") {
        @run = @run + 1;
        @active = @run;
    } else if ($marker == "/tmp/.m2a-stop") {
        @active = 0;
    } else if ($marker == "/tmp/.m2a-finish") {
        exit();
    }
}

kfunc:filemap_get_pages
/comm == "postgres" && @active && curtask->real_parent->tgid == $2 &&
 args.iocb->ki_filp->f_inode->i_sb->s_dev == $3/
{
    @first[tid] = 1;
}

kretfunc:filemap_get_read_batch
/comm == "postgres" && @first[tid] == 1/
{
    $nr = (int64)args.fbatch->nr;
    if ($nr > 0 && $nr <= 15) {
        $base = (uint64)args.fbatch + offsetof(struct folio_batch, folios);
        $last = *(struct page **)($base + 8 * (uint64)($nr - 1));
        if (!($last->flags & 8)) {
            $nr--;
        }
    }
    @first[tid] = $nr + 2;
}

kretfunc:filemap_get_pages
/comm == "postgres" && @first[tid]/
{
    $first = @first[tid];
    delete(@first[tid]);
    $sample = @active;
    if ($sample && retval == 0) {
        $nr = (int64)args.fbatch->nr;
        if ($nr < 1 || $nr > 15 || $first < 2) {
            @bad[$sample] = count();
        } else {
            $base = (uint64)args.fbatch + offsetof(struct folio_batch, folios);
            $first_page = *(struct page **)$base;
            $last = *(struct page **)($base + 8 * (uint64)($nr - 1));
            if (($first_page->flags | $last->flags) & 64) {
                @bad[$sample] = count();
            } else {
                $hits = $first - 2;
                @pages[$sample] = sum($nr);
                @hits[$sample] = sum($hits > $nr ? $nr : $hits);
            }
        }
    }
}

END
{
    print(@pages);
    print(@hits);
    print(@bad);
    clear(@pages);
    clear(@hits);
    clear(@bad);
    clear(@first);
    clear(@active);
    clear(@run);
}
"""


SESSION_SETUP = """
LOAD 'auto_explain';
SET auto_explain.log_min_duration = 0;
SET auto_explain.log_level = notice;
SET auto_explain.log_analyze = on;
SET auto_explain.log_timing = off;
SET auto_explain.log_buffers = on;
SET auto_explain.log_format = json;
SET auto_explain.log_verbose = {verbose};
SET log_planner_stats = on;
SET client_min_messages = log;
SET track_io_timing = on;
SET statement_timeout = '600s';
\\set ON_ERROR_STOP off
\\echo M2A_READY
"""


PLAN_HEADER = re.compile(r"^NOTICE: +duration: ([\d.]+) ms  plan:\n", re.M)


PLANNING_TIME = re.compile(
    r"^LOG: +PLANNER STATISTICS\nDETAIL:.*\n!.* ([\d.]+) s elapsed\n(?:!.*\n)*",
    re.M,
)


LATENCY = re.compile(r"^Time: ([\d.]+) ms(?: \([^\n]*\))?$", re.M)


BUFFER_FIELDS = {
    "shared_hit_blocks": "Shared Hit Blocks",
    "shared_miss_blocks": "Shared Read Blocks",
    "temp_read_blocks": "Temp Read Blocks",
    "temp_written_blocks": "Temp Written Blocks",
    "pg_read_ms": "I/O Read Time",
    "pg_write_ms": "I/O Write Time",
}


def marker(action):
    try:
        os.close(os.open(f"/tmp/.m2a-{action}", os.O_RDONLY))
    except FileNotFoundError:
        pass


def send(psql, commands):
    psql.stdin.write(commands)
    psql.stdin.flush()


def read_until(psql, boundary):
    lines = []
    for line in psql.stdout:
        if line.rstrip("\n") == boundary:
            return "".join(lines)
        lines.append(line)
    raise RuntimeError(f"psql exited before {boundary}\n{''.join(lines)}")


def check_native_plan(plan):
    if plan["Node Type"] == "Custom Scan":
        raise ValueError("unexpected Custom Scan in native HOST execution")
    for child in plan.get("Plans", []):
        check_native_plan(child)


def percent(hits, accesses):
    return f"{100 * hits / accesses:.6f}" if accesses else ""


def statement_latencies(output, sql_file):
    expected_statements = 3 if sql_file.stem == "15" else 1
    latencies = [Decimal(match[1]) for match in LATENCY.finditer(output)]
    # At file EOF, psql can send a trailing block comment as an empty query
    # and time the round trip. Keep only actual SQL statement timings.
    if len(latencies) == expected_statements + 1 and re.search(
        r";\s*/\*.*?\*/\s*\Z", sql_file.read_text(), flags=re.S,
    ):
        latencies.pop()
    return latencies, expected_statements


def nvme_metrics(before, after):
    # NVMe counts completions on the entire Guest namespace. Sectors in
    # Linux block statistics are 512 bytes, including on 4 KiB filesystems.
    delta = [end - start for start, end in zip(before, after)]
    if min(delta[:8]) < 0:
        raise RuntimeError("NVMe counters reset during a sample")
    return {
        "nvme_read_requests": delta[0], "nvme_read_bytes": 512 * delta[2],
        "nvme_read_ms": delta[3], "nvme_write_requests": delta[4],
        "nvme_write_bytes": 512 * delta[6], "nvme_write_ms": delta[7],
    }


def measure_sample(output, sql_file, before, after, *, inspect_plan=None):
    # psql prefixes server messages with the included SQL file and line.
    output = re.sub(r"^psql:[^\n]*?:\d+: ", "", output, flags=re.M)
    metrics = {"status": "ok", **nvme_metrics(before, after)}
    latencies, expected_statements = statement_latencies(output, sql_file)
    if latencies:
        metrics["latency_ms"] = sum(latencies)

    if re.search(r"^(?:ERROR|FATAL|psql:).*", output, re.M):
        metrics["status"] = "sql_error"
        return metrics, None
    try:
        if len(latencies) != expected_statements:
            raise ValueError("missing psql latency for a SQL statement")
        plan_header, = PLAN_HEADER.finditer(output)
        planning, = PLANNING_TIME.finditer(output)
        explain, end = json.JSONDecoder().raw_decode(output, plan_header.end())
        plan = explain["Plan"]
        check_native_plan(plan)
        if inspect_plan is not None:
            inspect_plan(plan)
        metrics.update({field: plan[key] for field, key in BUFFER_FIELDS.items()})
    except (ValueError, KeyError) as error:
        print(f"Q{sql_file.stem}: {error}", flush=True)
        metrics["status"] = "missing_pg_metrics"
        return metrics, None
    # The top-level plan includes its descendants and parallel workers.
    # PostgreSQL 12 reports these buffers for SELECT execution. For Q15 the
    # psql total above also includes CREATE VIEW and DROP VIEW.
    metrics["planning_ms"] = Decimal(planning[1]) * 1000
    metrics["execution_ms"] = Decimal(plan_header[1])
    hits = metrics["shared_hit_blocks"]
    accesses = hits + metrics["shared_miss_blocks"]
    metrics["shared_access_blocks"] = accesses
    metrics["shared_hit_pct"] = percent(hits, accesses)

    result = output[:plan_header.start()] + output[end:].lstrip("\n")
    result = PLANNING_TIME.sub("", result)
    result = LATENCY.sub("", result)
    rows = Counter(line for line in result.splitlines() if line)
    return metrics, rows


def run_sample(psql_cmd, env, probe, postmaster, device_id, nvme_stat, sql_file, *,
               verbose=False, messages=subprocess.STDOUT, before_sample=None):
    def read_nvme():
        return [int(field) for field in nvme_stat.read_text().split()]

    with tempfile.TemporaryFile(mode="w+t") as diagnostics, subprocess.Popen(
        ["sudo", "-n", "bpftrace", "-q", "-B", "line", str(probe),
         str(os.getpid()), postmaster, str(device_id)],
        stdout=subprocess.PIPE, stderr=diagnostics, text=True,
    ) as trace:
        ready = False
        try:
            for _ in range(100):
                marker("ping")
                if select.select([trace.stdout], [], [], 0.1)[0]:
                    ready = trace.stdout.readline().strip() == "READY"
                    break
            if not ready:
                diagnostics.seek(0)
                raise RuntimeError(f"page-cache probe did not start\n{diagnostics.read()}")

            with subprocess.Popen(
                ["stdbuf", "-oL", *psql_cmd], env=env, stdin=subprocess.PIPE,
                stdout=subprocess.PIPE, stderr=messages, text=True,
            ) as psql:
                try:
                    send(psql, SESSION_SETUP.format(verbose="on" if verbose else "off"))
                    read_until(psql, "M2A_READY")
                    if before_sample is not None:
                        before_sample()
                    before = read_nvme()
                    marker("start")
                    send(psql, f"\\i '{sql_file}'\n\\timing off\n\\echo M2A_END\n")
                    output = read_until(psql, "M2A_END")
                    marker("stop")
                    after = read_nvme()
                finally:
                    psql.stdin.close()
        finally:
            marker("finish")
            if not ready and trace.poll() is None:
                trace.terminate()
        counts = trace.stdout.read()
        status = trace.wait()
        diagnostics.seek(0)
        trace_errors = diagnostics.read()
        if status or "ERROR" in trace_errors or "lost" in trace_errors.lower():
            raise RuntimeError(f"page-cache probe failed\n{trace_errors}\n{counts}")

    counters = {
        (kind, int(run)): int(value) for kind, run, value in
        re.findall(r"^@(pages|hits|bad)\[([12])\]: ([\d]+)$", counts, re.M)
    }
    return output, before, after, counters


def plan_relations(plan):
    """Find heap, index and visibility-map dependencies in the plan tree."""
    check_native_plan(plan)
    references = set()
    nodes = [plan]
    while nodes:
        node = nodes.pop()
        schema = node.get("Schema", "public")
        if "Relation Name" in node:
            references.add((schema, node["Relation Name"], "main"))
            if node["Node Type"] == "Index Only Scan":
                references.add((schema, node["Relation Name"], "vm"))
        if "Index Name" in node:
            references.add((schema, node["Index Name"], "main"))
        nodes.extend(node.get("Plans", []))
    return references


def explain_sql(sql_file):
    sql = "\n".join(line for line in sql_file.read_text().splitlines() if not line.startswith("\\timing"))
    explain = "EXPLAIN (VERBOSE, FORMAT JSON)\n"
    if sql_file.stem == "15":
        create_view, rest = sql.split(";", 1)
        return f"BEGIN;\n{create_view};\n{explain}{rest}\nROLLBACK;\n"
    return explain + sql + "\n"


def sql_literal(text):
    return "'" + text.replace("'", "''") + "'"


def relation_values(relations):
    return ", ".join(f"({oid}, {sql_literal(fork)})" for oid, fork in sorted(relations))


def collect_files(psql, sql_files, manifest_path):
    """Resolve each plan's relations to the actual segment files in PGDATA."""
    catalog = {}
    toast = {}
    for line in psql("""
        SELECT n.nspname, c.relname, c.oid, c.relkind,
               c.reltoastrelid, i.indexrelid
        FROM pg_class c JOIN pg_namespace n ON c.relnamespace = n.oid
        LEFT JOIN pg_index i ON i.indrelid = c.reltoastrelid
        WHERE n.nspname IN ('public', 'pg_toast') AND c.relkind IN ('r', 'i', 't')
    """).splitlines():
        schema, name, oid, kind, toast_oid, toast_index = line.split("|")
        catalog[(schema, name)] = int(oid), kind
        if toast_oid != "0":
            toast[int(oid)] = int(toast_oid), int(toast_index)
    by_oid = {oid: (schema, name, kind) for (schema, name), (oid, kind) in catalog.items()}
    plans = {
        query: plan_relations(json.loads(psql(explain_sql(path)))[0]["Plan"])
        for query, path in sql_files.items()
    }
    dependencies = {}
    for query, plan in plans.items():
        deps = {(catalog[(schema, name)][0], fork) for schema, name, fork in plan}
        for oid, fork in sorted(deps):
            if fork == "main" and oid in toast:
                toast_oid, index_oid = toast[oid]
                if int(psql(f"SELECT pg_relation_size({toast_oid}::regclass);")):
                    deps.update(((toast_oid, "main"), (index_oid, "main")))
        dependencies[query] = deps

    all_deps = set().union(*dependencies.values())
    segments = {}
    for line in psql(f"""
        SELECT d.oid, d.fork, f.segment, f.relpath, f.os_page_size, f.rel_os_pages
        FROM (VALUES {relation_values(all_deps)}) AS d(oid, fork)
        CROSS JOIN LATERAL pgfincore(d.oid::regclass, d.fork, false) AS f
        ORDER BY d.oid, d.fork, f.segment
    """).splitlines():
        oid, fork, segment, path, os_page_size, os_pages = line.split("|")
        if int(os_page_size) != PAGE_BYTES:
            raise RuntimeError(f"{path}: unexpected OS page size {os_page_size}")
        if int(os_pages) == 0:
            continue
        schema, name, kind = by_oid[int(oid)]
        file = dict(zip(FILE_FIELDS, (
            schema, name, kind, int(oid), fork, int(segment), path, int(os_pages),
        )))
        segments.setdefault((int(oid), fork), []).append(file)

    files = {
        query: [file for dep in sorted(deps) for file in segments.get(dep, [])]
        for query, deps in dependencies.items()
    }
    if any(not items for items in files.values()):
        raise RuntimeError("a query has no resolved relation segments")
    if any(dep not in segments and dep[1] == "main" for dep in all_deps):
        raise RuntimeError("a main-fork dependency has no physical segments")
    with manifest_path.open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=("query", *FILE_FIELDS))
        writer.writeheader()
        for query, items in files.items():
            writer.writerows({"query": f"Q{query}", **file} for file in items)
    return plans, files


def cache_bitmap(pages, level):
    """Select nested bands distributed across the file in whole PG blocks."""
    if pages % PG_BLOCK_PAGES:
        raise ValueError(f"invalid relation segment size in OS pages: {pages}")
    bands, tail = divmod(pages, BAND_PAGES)
    band_hot = BAND_PAGES * level // 10
    tail_hot = ((tail // PG_BLOCK_PAGES * level + 5) // 10) * PG_BLOCK_PAGES
    expr = (
        f"repeat(repeat('1', {band_hot}) || repeat('0', {BAND_PAGES - band_hot}), {bands})"
        f" || repeat('1', {tail_hot}) || repeat('0', {tail - tail_hot})"
    )
    return bands * band_hot + tail_hot, expr


def cache_snapshot(psql, files):
    relations = relation_values({(file["oid"], file["fork"]) for file in files})
    pages = {}
    for line in psql(f"""
        SELECT f.relpath, f.rel_os_pages, f.pages_mem
        FROM (VALUES {relations}) AS d(oid, fork)
        CROSS JOIN LATERAL pgfincore(d.oid::regclass, d.fork, false) AS f
        WHERE f.rel_os_pages > 0
    """).splitlines():
        path, count, resident = line.split("|")
        pages[path] = int(count), int(resident)
    if {path: count for path, (count, _) in pages.items()} != {
        file["path"]: file["os_pages"] for file in files
    }:
        raise RuntimeError("relation segment inventory changed during measurement")
    return {path: resident for path, (_, resident) in pages.items()}


def prepare_cache(psql, files, level):
    targets = {}
    loads = []
    for file in files:
        target, bitmap = cache_bitmap(file["os_pages"], level)
        targets[file["path"]] = target
        if target:
            loads.append(
                f"({file['oid']}, {sql_literal(file['fork'])}, {file['segment']}, ({bitmap})::varbit)"
            )
    started = time.monotonic()
    if loads:
        requested = psql(f"""
            SELECT sum(f.pages_loaded) FROM (VALUES {", ".join(loads)}) AS d(oid, fork, seg, bits)
            CROSS JOIN LATERAL pgfadvise_loader(d.oid::regclass, d.fork, d.seg, true, false, d.bits) AS f
        """)
        if int(requested) != sum(targets.values()):
            raise RuntimeError("pgfadvise_loader did not request all selected pages")
    resident = cache_snapshot(psql, files)
    deadline = time.monotonic() + 15
    while resident != targets and time.monotonic() < deadline and loads:
        time.sleep(0.25)
        resident = cache_snapshot(psql, files)
    elapsed_ms = (time.monotonic() - started) * 1000
    return targets, resident, elapsed_ms


def paired_host_inputs(host_csv, hashes, levels, settings):
    run_log = host_csv.with_name("run.log").read_text().splitlines()
    if f"PostgreSQL settings in SELECT order: {settings}" not in run_log:
        raise ValueError("current PostgreSQL settings differ from the frozen HOST batch")
    frozen_hashes, = (
        json.loads(line.removeprefix("SQL SHA256: "))
        for line in run_log if line.startswith("SQL SHA256: ")
    )
    if any(frozen_hashes.get(str(query)) != digest for query, digest in hashes.items()):
        raise ValueError("current SQL differs from the frozen HOST batch")

    with host_csv.open(newline="") as stream:
        rows = {(row["query"], row["target_residency"]): row for row in csv.DictReader(stream)}
    files = {query: [] for query in hashes}
    with host_csv.with_name(f"{host_csv.stem}_files.csv").open(newline="") as stream:
        for row in csv.DictReader(stream):
            query = int(row.pop("query")[1:])
            if query in files:
                for field in ("oid", "segment", "os_pages"):
                    row[field] = int(row[field])
                files[query].append(row)
    snapshots = {}
    with host_csv.with_name(f"{host_csv.stem}_residency.csv").open(newline="") as stream:
        for row in csv.DictReader(stream):
            key = row["query"], row["target_residency"]
            snapshots.setdefault(key, {})[row["path"]] = int(row["resident_pages_before"])

    reference_hashes = {
        Path(filename).name: digest for digest, filename in (
            line.split() for line in (REFERENCE_LOGS.parents[1] / "queries.sha256").read_text().splitlines()
        )
    }
    references = {}
    for query, digest in hashes.items():
        if reference_hashes.get(f"{query}.sql") != digest or not files[query]:
            raise ValueError(f"Q{query}: missing file inventory or matching result reference")
        reference = (REFERENCE_LOGS / f"Q{query}.host-only.1.out").read_text()
        body = reference.split("TPCH_BEGIN 1\n", 1)[1].split("TPCH_END 1", 1)[0]
        references[query] = result_rows(body)
        for level in {0, *levels}:
            key = f"Q{query}", f"{level / 10:.1f}"
            row = rows[key]
            if (row["status"], row["cache_status"], row["results_match"], row["plan_matches"]) != (
                "ok", "ok", "yes", "yes",
            ) or Decimal(row["latency_ms"]) <= 0 or not snapshots[key]:
                raise ValueError(f"{key}: invalid frozen HOST measurement")
    return rows, files, snapshots, references


def result_rows(output):
    return Counter(
        line for line in LATENCY.sub("", output).splitlines()
        if line and not line.startswith("Timing is ")
    )


def filter_metrics(output, messages, sql_file, before, after):
    metrics = {"status": "ok", "actual_path": "unknown", **nvme_metrics(before, after)}
    latencies, expected = statement_latencies(output, sql_file)
    if latencies:
        metrics["latency_ms"] = sum(latencies)
    metrics["executor_end_ms"] = sum((
        Decimal(value) for value in re.findall(
            r"cache ledger refresh reason=executor_end[^\n]*?\bwall_ms=([\d.]+)", messages,
        )
    ), Decimal(0))
    messages = re.sub(r"^psql:[^\n]*?:\d+: ", "", messages, flags=re.M)
    if re.search(r"^(?:ERROR|FATAL|PANIC|psql:)", messages, re.M):
        metrics["status"] = "sql_error"
        return metrics, None
    if len(latencies) != expected or metrics["latency_ms"] <= metrics["executor_end_ms"]:
        metrics["status"] = "invalid_timing"
        return metrics, None
    metrics["filter_latency_ms"] = metrics["latency_ms"] - metrics["executor_end_ms"]

    # The extension may log plans for its own SQL. The top-level statement
    # must come from the formal SQL file (the SELECT inside Q15's script).
    source = " ".join(sql_file.read_text().split())
    main_plans = []
    try:
        for header in PLAN_HEADER.finditer(messages):
            explain, _ = json.JSONDecoder().raw_decode(messages, header.end())
            query_text = " ".join(explain.get("Query Text", "").split())
            if query_text and query_text in source:
                main_plans.append((Decimal(header[1]), explain["Plan"]))
    except (ValueError, KeyError):
        metrics["status"] = "invalid_plan_log"
        return metrics, result_rows(output)
    if len(main_plans) != 1:
        metrics["status"] = "missing_main_plan"
        return metrics, result_rows(output)

    execution_ms, plan = main_plans[0]
    nodes = [plan]
    scans = 0
    while nodes:
        node = nodes.pop()
        if node.get("Custom Plan Provider", "").startswith("SniaCsd") and (
            node.get("Actual Loops", 0) or any(worker.get("Actual Loops", 0) for worker in node.get("Workers", []))
        ):
            scans += 1
        nodes.extend(node.get("Plans", []))
    metrics.update(
        execution_ms=execution_ms, executed_csd_scans=scans,
        actual_path="filter-only" if scans else "host",
    )
    metrics.update({field: plan.get(key, 0) for field, key in BUFFER_FIELDS.items()})
    hits, misses = metrics["shared_hit_blocks"], metrics["shared_miss_blocks"]
    metrics.update(shared_access_blocks=hits + misses, shared_hit_pct=percent(hits, hits + misses))
    return metrics, result_rows(output)


def test(csv_path=None, queries=QUERIES, levels=None, *, filter_only=False, host_csv=CSV_PATH):
    """Restart PG and clear Guest page cache for each independent grid cell."""
    csv_path = csv_path or (FILTER_CSV if filter_only else CSV_PATH)
    if levels is None:
        levels = (0,) if filter_only else LEVELS
    guest_tests = PROJECT_ROOT / "tests"
    if (guest_tests / "guest").is_dir():
        guest_tests /= "guest"
    sql_files = {q: guest_tests / "queries" / "sf8_sel0.1" / f"{q}.sql" for q in queries}
    hashes = {q: hashlib.sha256(path.read_bytes()).hexdigest() for q, path in sql_files.items()}
    mem_kib = int(Path("/proc/meminfo").read_text().splitlines()[0].split()[1])
    mount = subprocess.check_output(
        ["findmnt", "-n", "-o", "SOURCE,FSTYPE", "-T", str(PG_DATA)], text=True,
    ).split()
    if (
        not 29 * 2**20 <= mem_kib <= 30 * 2**20 or os.sysconf("SC_PAGE_SIZE") != PAGE_BYTES
        or mount != ["/dev/nvme0n1", "ext4"] or not os.uname().release.startswith("6.8.")
    ):
        raise RuntimeError("run in the 30 GiB Linux 6.8 Guest with NVMe ext4 and 4 KiB pages")

    env = {key: value for key, value in os.environ.items() if not key.startswith("PG")}
    env["LC_ALL"] = "C"
    measured_env = {**env, "PGOPTIONS": FILTER_ENV, "SNIA_FEMU_PROFILE": "0"} if filter_only else env
    psql_cmd = [str(PG_BIN / "psql"), "-XqAt", "-v", "ON_ERROR_STOP=1", "-d", DATABASE]
    pg_ctl = [str(PG_BIN / "pg_ctl"), "-s", "-w", "-D", str(PG_DATA)]
    active_sql = (
        "SELECT count(*) FROM pg_stat_activity "
        "WHERE backend_type = 'client backend' AND pid <> pg_backend_pid();"
    )

    def psql(sql):
        return subprocess.check_output(psql_cmd, input=sql, env=env, text=True).strip()

    startup = shlex.split((PG_DATA / "postmaster.opts").read_text())
    if startup[0] != str(PG_BIN / "postgres"):
        raise RuntimeError(f"unexpected PostgreSQL startup command: {startup}")
    args = startup[3:] if startup[1:3] == ["-D", str(PG_DATA)] else startup[1:]
    start_pg = [*pg_ctl, "-l", str(PG_DATA / "log"), "-o", shlex.join(args), "start"]
    nvme_stat = Path("/sys/class/block/nvme0n1/stat")
    dev = PG_DATA.stat().st_dev
    device_id = (os.major(dev) << 20) | os.minor(dev)
    settings_sql = """
        SELECT current_setting('server_version_num'), current_setting('data_directory'),
               current_setting('shared_preload_libraries'),
               current_setting('session_preload_libraries'),
               current_setting('local_preload_libraries'),
               current_setting('autovacuum'), current_setting('block_size'),
               current_setting('shared_buffers'), current_setting('work_mem'),
               current_setting('max_parallel_workers_per_gather'),
               current_setting('max_parallel_workers'), current_setting('max_worker_processes'),
               current_setting('enable_hashagg'),
               (SELECT extversion FROM pg_extension WHERE extname = 'pgfincore');
    """
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path = csv_path.with_name(f"{csv_path.stem}_files.csv")
    residency_path = csv_path.with_name(f"{csv_path.stem}_residency.csv")
    failures = 0
    done = 0
    total = len(queries) * len(levels)
    with Path(f"/tmp/{TEST_NAME}-{os.getuid()}.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        settings = psql(settings_sql).split("|")
        if settings[:7] != ["120022", str(PG_DATA), "", "", "", "off", "8192"] or settings[-1] != "1.4":
            raise RuntimeError(f"unexpected native PostgreSQL settings or missing pgfincore 1.4: {settings}")
        if psql(active_sql) != "0":
            raise RuntimeError("another PostgreSQL client is connected")
        subprocess.run(["sudo", "-n", "true"], check=True)
        print(f"PostgreSQL settings in SELECT order: {settings}", flush=True)
        print(f"Guest kernel {os.uname().release}, MemTotal {mem_kib} KiB", flush=True)
        print(f"SQL SHA256: {json.dumps(hashes)}", flush=True)
        if filter_only:
            host_rows, files, host_snapshots, references = paired_host_inputs(host_csv, hashes, levels, settings)
            print(f"Frozen HOST CSV SHA256: {hashlib.sha256(host_csv.read_bytes()).hexdigest()}", flush=True)
            modes = subprocess.check_output(
                psql_cmd, input="SELECT current_setting('pg_sina_csd.exe_path'), "
                "current_setting('pg_sina_csd.hpcp'), current_setting('pg_sina_csd.dcp');",
                env=measured_env, text=True,
            ).strip()
            if modes != "filter-only|off|off":
                raise RuntimeError(f"unexpected offload settings: {modes}")
        else:
            plans, files = collect_files(psql, sql_files, manifest_path)
        print(f"Resolved {sum(map(len, files.values()))} query-file entries", flush=True)
        with tempfile.TemporaryDirectory(prefix=f"{TEST_NAME}-") as tmp, csv_path.open(
            "w", newline="",
        ) as destination, residency_path.open("w", newline="") as residency:
            probe = Path(tmp) / "page_cache.bt"
            probe.write_text(PAGE_CACHE_PROBE)
            writer = csv.DictWriter(destination, fieldnames=FILTER_FIELDS if filter_only else FIELDS)
            file_writer = csv.writer(residency)
            writer.writeheader()
            file_writer.writerow((
                "query", "target_residency", "path", "os_pages",
                "target_os_pages", "resident_pages_before", "resident_pages_after",
            ))
            for query in queries:
                baseline = Decimal(host_rows[f"Q{query}", "0.0"]["latency_ms"]) if filter_only else None
                baseline_result = references[query] if filter_only else None
                for level in levels:
                    fraction = f"{level / 10:.1f}"
                    if psql(active_sql) != "0":
                        raise RuntimeError(f"Q{query}: another PostgreSQL client is connected")
                    subprocess.run([*pg_ctl, "-m", "fast", "stop"], env=env, check=True)
                    try:
                        os.sync()
                        subprocess.run(
                            ["sudo", "-n", "sh", "-c", "echo 3 > /proc/sys/vm/drop_caches"], check=True,
                        )
                    finally:
                        subprocess.run(start_pg, env=env, check=True)
                    targets, resident, setup_ms = prepare_cache(psql, files[query], level)
                    postmaster = (PG_DATA / "postmaster.pid").read_text().splitlines()[0]
                    if filter_only:
                        # Verify the same files after CSD session initialization, immediately
                        # before the timed SQL. This connection reads only cache metadata.
                        snapshots = []
                        with tempfile.TemporaryFile(mode="w+t") as messages:
                            output, nvme_before, nvme_after, counters = run_sample(
                                psql_cmd, measured_env, probe, postmaster, device_id, nvme_stat, sql_files[query],
                                verbose=True, messages=messages,
                                before_sample=lambda: snapshots.append(cache_snapshot(psql, files[query])),
                            )
                            messages.seek(0)
                            diagnostics = messages.read()
                        resident, = snapshots
                        metrics, result = filter_metrics(
                            output, diagnostics, sql_files[query], nvme_before, nvme_after,
                        )
                        fdm = subprocess.run(
                            ["sudo", "-n", "nvme", "admin-passthru", "/dev/nvme0", "--opcode=0xc0", "--cdw10=2"],
                            env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        )
                        fdm_clean = fdm.returncode == 0 and "result: 0x00000000" in fdm.stdout
                        metrics["fdm_clean"] = "yes" if fdm_clean else "no"
                        if not fdm_clean and metrics["status"] == "ok":
                            metrics["status"] = "fdm_not_clean"
                        key = f"Q{query}", fraction
                        cache_matches = resident == targets == host_snapshots[key]
                        metrics["host_latency_ms"] = Decimal(host_rows[key]["latency_ms"])
                        latency = metrics.get("filter_latency_ms")
                    else:
                        output, nvme_before, nvme_after, counters = run_sample(
                            psql_cmd, env, probe, postmaster, device_id, nvme_stat, sql_files[query],
                            verbose=True,
                        )
                        runtime_plans = []
                        metrics, result = measure_sample(
                            output, sql_files[query], nvme_before, nvme_after,
                            inspect_plan=lambda plan: runtime_plans.append(plan_relations(plan)),
                        )
                        cache_matches = resident == targets
                        latency = metrics.get("latency_ms")
                    after = cache_snapshot(psql, files[query])
                    pages = counters.get(("pages", 1), 0)
                    hits = counters.get(("hits", 1), 0)
                    if counters.get(("bad", 1), 0) or not 0 <= hits <= pages or (
                        pages < PG_BLOCK_PAGES * metrics.get("shared_miss_blocks", 0)
                    ):
                        if metrics["status"] == "ok":
                            metrics["status"] = "missing_page_cache_metrics"
                    else:
                        metrics.update(
                            page_cache_access_pages=pages, page_cache_hit_pages=hits,
                            page_cache_miss_pages=pages - hits, page_cache_hit_pct=percent(hits, pages),
                        )
                    plan_matches = filter_only or runtime_plans == [plans[query]]
                    if not filter_only and level == 0 and result is not None:
                        baseline_result = result
                        if metrics["status"] == "ok" and plan_matches and resident == targets:
                            baseline = metrics["latency_ms"]
                    result_matches = result is not None and result == baseline_result
                    valid = metrics["status"] == "ok" and plan_matches and result_matches and cache_matches
                    total_pages = sum(file["os_pages"] for file in files[query])
                    # Only the measured SQL goes into latency and speedup. The
                    # separate mincore snapshots measure occupancy of planned files.
                    row = {
                        "query": f"Q{query}", "target_residency": fraction,
                        "target_os_pages": sum(targets.values()), "relation_os_pages": total_pages,
                        "resident_pages_before": sum(resident.values()),
                        "resident_pages_after": sum(after.values()),
                        "actual_residency_before": f"{sum(resident.values()) / total_pages:.9f}",
                        "cache_setup_ms": f"{setup_ms:.3f}",
                        "cache_status": "ok" if cache_matches else "off_target",
                        "results_match": "yes" if result_matches else "no",
                        "speedup": f"{baseline / latency:.9f}" if valid and baseline and latency else "",
                        **metrics,
                    }
                    if filter_only:
                        row["host_over_filter"] = (
                            f"{metrics['host_latency_ms'] / latency:.9f}" if valid and latency else ""
                        )
                    else:
                        row["plan_matches"] = "yes" if plan_matches else "no"
                    if not valid:
                        failures += 1
                        if filter_only:
                            output = f"{output}\n{diagnostics}\n{fdm.stdout}"
                        csv_path.with_name(f"{csv_path.stem}-Q{query}-{fraction}.log").write_text(output)
                    writer.writerow(row)
                    file_writer.writerows(
                        (f"Q{query}", fraction, file["path"], file["os_pages"],
                         targets[file["path"]], resident[file["path"]], after[file["path"]])
                        for file in files[query]
                    )
                    destination.flush()
                    residency.flush()
                    done += 1
                    print(f"Q{query} residency {fraction} recorded ({done}/{total})", flush=True)
    if failures:
        raise RuntimeError(f"{failures} cells failed validation; see {csv_path}")


def cold_filter_boundaries(host_rows, filter_rows, queries, levels):
    """Compare each HOST column with one measured cold filter latency."""
    host = {(row["query"], Decimal(row["target_residency"])): row for row in host_rows}
    cold = {}
    for row in filter_rows:
        query = row["query"]
        if query not in queries or Decimal(row["target_residency"]) != 0:
            continue
        if query in cold:
            raise ValueError(f"{query}: duplicate cold filter-only sample")
        cold[query] = row

    boundaries = {}
    for query in queries:
        row = cold.get(query)
        if row is None or (
            row["status"], row["cache_status"], row["results_match"], row["fdm_clean"], row["actual_path"],
        ) != ("ok", "ok", "yes", "yes", "filter-only"):
            raise ValueError(f"{query}: missing a validated cold filter-only sample")
        host_times = [Decimal(host[query, level]["latency_ms"]) for level in levels]
        filter_ms = Decimal(row["filter_latency_ms"])
        if filter_ms <= 0 or host_times[0] != Decimal(row["host_latency_ms"]):
            raise ValueError(f"{query}: invalid filter-only latency or mismatched cold HOST baseline")
        # The top and bottom edges also represent thresholds when one path
        # wins across the entire measured range.
        first_faster = next((i for i, latency in enumerate(host_times) if latency < filter_ms), len(levels))
        boundaries[query] = first_faster, host_times[0] / filter_ms
    return boundaries


def plot(csv_path=CSV_PATH, filter_csv=None):
    """Render paired cold-baseline speedups directly from the experiment CSV."""
    import matplotlib.pyplot as plt
    from matplotlib import rc_context
    from matplotlib.colors import LinearSegmentedColormap, Normalize
    from matplotlib.lines import Line2D

    with csv_path.open(newline="") as source:
        rows = list(csv.DictReader(source))
    queries = sorted({row["query"] for row in rows}, key=lambda query: int(query[1:]))
    levels = sorted({Decimal(row["target_residency"]) for row in rows})
    values = {
        (row["query"], Decimal(row["target_residency"])): float(row["speedup"] or "nan")
        for row in rows
    }
    if len(values) != len(rows) or len(values) != len(queries) * len(levels) or not levels or levels[0] != 0:
        raise ValueError("expected a complete grid with one 0-residency baseline per query")
    if any(values[query, Decimal(0)] != 1 for query in queries):
        raise ValueError("each query needs a validated baseline with speedup 1")
    if filter_csv is not None:
        with filter_csv.open(newline="") as source:
            boundaries = cold_filter_boundaries(rows, list(csv.DictReader(source)), queries, levels)
    matrix = [[values[query, level] for query in queries] for level in levels]
    upper = max(1.01, *(value for value in values.values() if math.isfinite(value)))
    cmap = LinearSegmentedColormap.from_list(
        "cache_speedup", ["#ffffff", "#c9e1f2", "#6daed5", "#155384", "#092d51"],
    )
    cmap.set_under("#f6d8b9")
    cmap.set_bad("#d8dde1")
    norm = Normalize(vmin=1, vmax=upper)

    with rc_context({
        "font.family": "sans-serif", "font.sans-serif": ["DejaVu Sans"], "font.size": 10,
        "svg.fonttype": "none", "pdf.fonttype": 42,
    }):
        figure, axes = plt.subplots(figsize=(13.8, 6.4 if filter_csv is None else 7.2), layout="constrained")
        grid = axes.pcolormesh(
            [x - 0.5 for x in range(len(queries) + 1)],
            [y - 0.5 for y in range(len(levels) + 1)],
            matrix, cmap=cmap, norm=norm, shading="flat",
            edgecolors="#dce5eb", linewidth=0.28,
        )
        axes.set_xticks(range(len(queries)), queries)
        axes.set_yticks(range(len(levels)), [f"{level:.1f}" for level in levels])
        axes.set_xlabel("Query")
        axes.set_ylabel("Target Guest page-cache residency")
        axes.tick_params(length=0, pad=5)
        for spine in axes.spines.values():
            spine.set_color("#b5c0c8")
        for y, row in enumerate(matrix):
            for x, value in enumerate(row):
                if math.isfinite(value):
                    rgba = cmap(norm(value))
                    lightness = 0.2126 * rgba[0] + 0.7152 * rgba[1] + 0.0722 * rgba[2]
                    axes.text(
                        x, y, f"{value:.2f}", ha="center", va="center", fontsize=8,
                        color="#122f40" if lightness > 0.55 else "white",
                    )
        output = csv_path
        if filter_csv is not None:
            color = "#b71f36"
            for x, query in enumerate(queries):
                first_faster, speedup = boundaries[query]
                y = first_faster - 0.5
                axes.plot(
                    [x - 0.43, x + 0.43], [y, y], color=color, linewidth=2, gid=f"cold-filter-{query}",
                    solid_capstyle="butt",
                )
                axes.text(
                    x, y + 0.08, f"{speedup:.2f}×", ha="center", va="bottom",
                    fontsize=8, fontweight="bold", color="white",
                    bbox={"boxstyle": "square,pad=0.16", "facecolor": color, "edgecolor": "none"},
                )
            axes.set_ylim(-0.6, len(levels) + 0.08)
            axes.legend(
                handles=[Line2D([], [], color=color, linewidth=2,
                                label="Cold filter offload latency threshold (speedup vs. cold HOST)")],
                loc="lower left", bbox_to_anchor=(0, 1), frameon=False, handlelength=1.8,
            )
            output = csv_path.with_name(f"{csv_path.stem}_filter_boundary")
        slower = any(math.isfinite(value) and value < 1 for value in values.values())
        scale = figure.colorbar(grid, ax=axes, extend="min" if slower else "neither", pad=0.02)
        scale.solids.set_rasterized(False)
        scale.set_label("Total-latency speedup vs. 0 residency (×)")
        for suffix in (".svg", ".pdf"):
            figure.savefig(output.with_suffix(suffix))
        figure.savefig(output.with_suffix(".png"), dpi=300)
        plt.close(figure)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(dest="mode", required=True)
    tests = modes.add_parser("test")
    tests.add_argument("--query", action="append", type=int, choices=QUERIES)
    tests.add_argument("--filter-only", action="store_true")
    tests.add_argument("--host-csv", type=Path, default=CSV_PATH,
                       help="paired HOST measurements for --filter-only")
    tests.add_argument("--residency", action="append", type=Decimal,
                       choices=[Decimal(level) / 10 for level in LEVELS])
    plots = modes.add_parser("plot")
    plots.add_argument("--filter-csv", type=Path)
    for mode in (tests, plots):
        mode.add_argument("--csv", type=Path)
    args = parser.parse_args()
    if args.mode == "plot":
        plot(args.csv or CSV_PATH, args.filter_csv)
    else:
        levels = sorted({int(level * 10) for level in args.residency}) if args.residency else None
        if levels and levels[0] != 0 and not args.filter_only:
            tests.error("include the 0-residency baseline")
        test(
            args.csv,
            sorted(set(args.query)) if args.query else QUERIES, levels,
            filter_only=args.filter_only, host_csv=args.host_csv,
        )


if __name__ == "__main__":
    main()
