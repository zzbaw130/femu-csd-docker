#!/usr/bin/env python3
"""Check Q1 under the standard SF8 cold and HOST-warmed protocols.

Each raw file from tpch-{cold,warm}.sh records id, five SQL script
latencies (ms), five ExecutorEnd cache maintenance times (ms),
pre-execution residency for each path (warm only, percent), path checks,
and FDM status. Its SQLerate companion records id, planning, execution,
raw latency, ExecutorEnd maintenance (ms), residency, placement, and status.
Paths are HOST, Filter, CSD without filling, CSD with filling, SQLerate.

comparison.csv records target selectivity, cache protocol, path, old and new
latency (ms after ExecutorEnd), relative latency change (%), actual SQLerate
placement, and estimated HOST and CSD costs (ms). The last three fields
apply to the SQLerate row. Baseline samples come from results/tpch_final.
"""

import argparse
import csv
from decimal import Decimal
import os
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[2]
OUTPUT = ROOT / "results" / Path(__file__).stem
BASELINE = ROOT / "results/tpch_final"
TESTS = Path("/home/femu/tests")
SELECTIVITIES = ("0.1", "0.2", "0.5", "0.8")
PROTOCOLS = ("cold", "warm")
MODES = ("host-only", "filter-only", "csd-without-hpcp", "csd-with-hpcp", "SQLerate")
COLUMNS = ("selectivity", "protocol", "path", "baseline_ms", "current_ms",
           "delta_pct", "placement", "host_estimate_ms", "csd_estimate_ms")


def single_record(path):
    with path.open(newline="") as stream:
        return next(row for row in csv.DictReader(stream) if row["id"] == "Q1-1")


def test(output):
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("TPCH_") and key not in ("PGOPTIONS", "SNIA_FEMU_PROFILE")}
    env.update(PGDATA="/data/tpch_s8", PGDATABASE="tpch_s8", TPCH_COMPARE_PROFILE="1")
    output.mkdir(parents=True, exist_ok=True)
    rows = []
    for selectivity in SELECTIVITIES:
        case = f"sel{selectivity}"
        directory = output / case
        for protocol in PROTOCOLS:
            csv_name = f"{protocol}.csv"
            subprocess.run(["sh", str(TESTS / f"tpch-{protocol}.sh"), "-s",
                            selectivity, "-n", "1", "-o", str(directory), "1"],
                           env=env, check=True)
            previous = single_record(BASELINE / case / csv_name)
            current = single_record(directory / csv_name)
            detail = single_record(directory / f"{protocol}-sqlerate.csv")
            assert current["fdm_clean"] == "yes" and detail["status"] == "ok"
            checks = dict(item.split("=") for item in current["checks"].split(";"))
            placement = detail["path"].split("; query=", 1)[0]
            host, csd = re.search(r"original HOST=([\d.]+)ms original CSD=([\d.]+)ms",
                                  detail["path"]).groups()
            for mode in MODES:
                assert checks[mode] == "ok"
                field = mode.lower().replace("-", "_")
                before, after = [
                    Decimal(record[f"{field}_ms"]) - Decimal(record[f"{field}_executor_end_ms"])
                    for record in (previous, current)
                ]
                extra = (placement, host, csd) if mode == "SQLerate" else ("", "", "")
                rows.append((selectivity, protocol, mode, before, after,
                             round((after / before - 1) * 100, 3), *extra))
    with (output / "comparison.csv").open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(COLUMNS)
        writer.writerows(rows)


def plot(output):
    import matplotlib.pyplot as plt

    with (output / "comparison.csv").open(newline="") as stream:
        rows = [row for row in csv.DictReader(stream) if row["path"] == "SQLerate"]
    fig, axes = plt.subplots(1, 2, sharey=True, figsize=(6, 2.8), layout="constrained")
    for ax, protocol in zip(axes, PROTOCOLS):
        samples = [row for row in rows if row["protocol"] == protocol]
        x = [float(row["selectivity"]) for row in samples]
        for field, label, marker in (("baseline_ms", "Before", "o"), ("current_ms", "After", "s")):
            ax.plot(x, [float(row[field]) / 1000 for row in samples], label=label, marker=marker)
        ax.set(xlabel="Target selectivity", title=protocol.capitalize(), xticks=x)
    axes[0].set_ylabel("SQLerate latency (s)")
    axes[0].legend(frameon=False)
    for suffix in ("svg", "pdf"):
        fig.savefig(output / f"{Path(__file__).stem}.{suffix}")
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("test", "plot"))
    parser.add_argument("--output", type=Path, default=OUTPUT)
    args = parser.parse_args()
    (test if args.action == "test" else plot)(args.output.resolve())


if __name__ == "__main__":
    main()
