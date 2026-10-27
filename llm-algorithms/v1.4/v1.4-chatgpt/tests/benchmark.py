#!/usr/bin/env python3
"""Run the supplied instances sequentially and retain independently checked CSV/logs."""
import argparse
import csv
import datetime
import hashlib
import io
import json
from pathlib import Path
import platform
import subprocess
from test_solver import validate


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary")
    parser.add_argument("--threads", type=int, default=8)
    parser.add_argument("--time-limit", type=float, default=30)
    parser.add_argument("--history-mb", type=int, default=256)
    parser.add_argument("--output-dir", type=Path, default=Path("benchmark-results"))
    parser.add_argument("--instances", nargs="*", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    binary = Path(args.binary).resolve()
    instances = args.instances or [root / "examples" / name for name in
                                  ("prob.42.sop", "rbg048a.sop", "R.200.1000.1.sop")]
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    result_path = output / "results.csv"
    if result_path.exists():
        raise SystemExit(f"Refusing to mix runs: {result_path} already exists. Choose a new output directory.")
    metadata = {"started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                "platform": platform.platform(), "threads": args.threads,
                "time_limit_seconds": args.time_limit, "history_mib": args.history_mb,
                "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                "runs": []}
    try:
        metadata["lscpu"] = subprocess.check_output(["lscpu"], text=True)
    except (OSError, subprocess.CalledProcessError):
        pass
    for path in instances:
        command = [str(binary), str(path.resolve()), "--threads", str(args.threads),
                   "--time-limit", str(args.time_limit), "--history-mb", str(args.history_mb),
                   "--output", str(result_path)]
        completed = subprocess.run(command, text=True, capture_output=True, timeout=args.time_limit + 120)
        (output / (path.name + ".log")).write_text(completed.stderr)
        if completed.returncode:
            raise RuntimeError(f"{path.name} failed: {completed.stderr}")
        row = next(csv.DictReader(io.StringIO(completed.stdout)))
        lines = path.read_text().splitlines()
        section = next(i for i, line in enumerate(lines) if line.strip() == "EDGE_WEIGHT_SECTION")
        tokens = " ".join(lines[section + 1:]).split()
        n = int(tokens[0])
        values = list(map(int, tokens[1:1 + n * n]))
        matrix = [values[i * n:(i + 1) * n] for i in range(n)]
        validate(matrix, row)
        metadata["runs"].append({"instance": path.name,
                                 "input_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                                 "result": row, "independent_path_validation": "passed"})
        (output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
        print(f"{path.name}: cost={row['Best Cost']}, optimal={row['Optimal']}, seconds={row['Time']}, nodes={row['Enumerated Nodes']}", flush=True)


if __name__ == "__main__":
    main()
