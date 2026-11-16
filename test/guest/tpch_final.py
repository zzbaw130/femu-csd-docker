#!/usr/bin/env python3
"""Run the final SF8 selectivity sweep using the standard Guest entry points.

Each sel*/{cold,warm,repeat}.csv contains, in order: id (query and
repetition), five raw SQL script times (ms), five ExecutorEnd maintenance
times (ms), five pre-execution cache snapshots for warm/repeat (percent
shared, file and observed coverage), per-path correctness, and FDM cleanup
status. Path order is HOST, Filter, CSD without filling, CSD with filling,
SQLerate. Raw time counts all three Q15 statements and excludes repeat pauses.

Each *-sqlerate.csv contains: id, planning, execution, raw script and
ExecutorEnd maintenance times (all ms), cache snapshot (as above), actual
placement, and status.

After the sweep, the existing RQ1 summarizer produces RQ1-tpch-performance.csv.
Its fields are target selectivity, cache state, id, five path times (ms after
subtracting ExecutorEnd), SQLerate placement class, planning time (ms),
execution time (ms), raw script time (ms), FDM cleanup and per-path checks.
"""

import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import sys


PROJECT_ROOT = Path(__file__).resolve().parents[2]
OUTPUT = PROJECT_ROOT / "results" / Path(__file__).stem
SELECTIVITIES = ("0.1", "0.2", "0.5", "0.8")
RUNS = {"cold": 1, "warm": 1, "repeat": 3}


def test(output):
    guest_tests = Path("/home/femu/tests")
    output.mkdir(parents=True, exist_ok=False)
    with (output / "queries.sha256").open("w") as manifest:
        for selectivity in SELECTIVITIES:
            for query in range(1, 23):
                sql = guest_tests / "queries" / f"sf8_sel{selectivity}" / f"{query}.sql"
                manifest.write(f"{hashlib.sha256(sql.read_bytes()).hexdigest()}  {sql}\n")

    env = os.environ.copy()
    for name in tuple(env):
        if name.startswith("TPCH_") or name in ("PGOPTIONS", "SNIA_FEMU_PROFILE"):
            del env[name]
    env.update(PGDATA="/data/tpch_s8", PGDATABASE="tpch_s8", TPCH_COMPARE_PROFILE="1")
    failed = False
    for selectivity in SELECTIVITIES:
        destination = output / f"sel{selectivity}"
        destination.mkdir()
        for mode, runs in RUNS.items():
            print(f"sel{selectivity} {mode}: started", flush=True)
            command = [
                "sh", str(guest_tests / f"tpch-{mode}.sh"), "-s", selectivity,
                "-n", str(runs), "-o", str(destination),
            ]
            with (destination / f"{mode}.log").open("w") as log:
                with subprocess.Popen(command, env=env, stdout=subprocess.PIPE,
                                      stderr=subprocess.STDOUT, text=True) as batch:
                    for line in batch.stdout:
                        log.write(line)
                        print(line, end="", flush=True)
                    status = batch.wait()
            (destination / f"{mode}.exit").write_text(f"{status}\n")
            print(f"sel{selectivity} {mode}: exit={status}", flush=True)
            failed |= status != 0

    subprocess.run(
        ["sh", str(guest_tests / "RQ1-tpch-performance.sh"), str(output),
         str(output / "RQ1-tpch-performance")], check=True,
    )
    return int(failed)


def plot(output):
    subprocess.run(
        [sys.executable, str(PROJECT_ROOT / "paper/figures/plot_experiments.py"),
         "--results", str(output), "--output", str(output),
         "RQ1-tpch-performance", "RQ1-repeat-paths"], check=True,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("test", "plot"))
    parser.add_argument("--output", type=Path, default=OUTPUT)
    args = parser.parse_args()
    return test(args.output.resolve()) if args.action == "test" else plot(args.output.resolve())


if __name__ == "__main__":
    sys.exit(main())
