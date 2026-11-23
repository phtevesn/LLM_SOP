#!/usr/bin/env python3
"""Run instances sequentially, retaining CSV rows, stderr, and reproducibility metadata."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("instances", type=Path, nargs="+")
    parser.add_argument("--solver", type=Path, default=Path("build/sop_exact"))
    parser.add_argument("--threads", type=int, nargs="+", default=[1, 4])
    parser.add_argument("--time-limit", type=float, default=30)
    parser.add_argument("--history-mb", type=int, default=256)
    parser.add_argument("--directory", type=Path, default=Path("benchmarks/local"))
    args = parser.parse_args()
    if args.directory.exists():
        raise SystemExit("choose a new benchmark directory (existing results are never overwritten)")
    args.directory.mkdir(parents=True)
    solver = args.solver.resolve()
    manifest = {"started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                "platform": platform.platform(), "compiler": subprocess.run(["g++", "--version"], text=True,
                capture_output=True).stdout.splitlines()[0], "cpu_affinity": sorted(os.sched_getaffinity(0))
                if hasattr(os, "sched_getaffinity") else None,
                "binary_sha256": hashlib.sha256(solver.read_bytes()).hexdigest(), "runs": []}
    for instance in args.instances:
        for threads in args.threads:
            command = [str(solver), str(instance), "--threads", str(threads), "--time-limit", str(args.time_limit),
                       "--history-mb", str(args.history_mb), "--output", str(args.directory / "results.csv")]
            print("running", instance.name, "threads", threads, flush=True)
            proc = subprocess.run(command, text=True, capture_output=True,
                                  timeout=max(60, args.time_limit + 60))
            stem = f"{len(manifest['runs']):02d}_{instance.stem}_t{threads}"
            (args.directory / (stem + ".stderr.txt")).write_text(proc.stderr)
            (args.directory / (stem + ".stdout.csv")).write_text(proc.stdout)
            manifest["runs"].append({"command": command, "returncode": proc.returncode,
                "input_sha256": hashlib.sha256(instance.read_bytes()).hexdigest(), "log": stem + ".stderr.txt"})
            (args.directory / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
            if proc.returncode:
                raise SystemExit(f"run failed; see {stem}.stderr.txt")
    print("results:", args.directory / "results.csv")


if __name__ == "__main__":
    main()
