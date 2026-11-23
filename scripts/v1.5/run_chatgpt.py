#!/usr/bin/env python3
"""Build and run the v1.5 ChatGPT SOP solver on every listed instance."""

from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[2]
SOLVER_DIR = ROOT / "llm-algorithms" / "v1.5" / "v1.5-chatgpt"
DEFAULT_INSTANCES = ROOT / "results" / "llm-env-resultsv1.5" / "instances.txt"
DEFAULT_OUTPUT = ROOT / "results" / "llm-env-resultsv1.5"
HEADER = ["Instance", "Instance Size", "Time", "Enumerated Nodes",
          "Best Cost", "Best Path", "Optimal"]


def read_instances(path: Path) -> list[Path]:
    """Read directory-switching instance lists and resolve every .sop file."""
    current_dir: Path | None = None
    instances: list[Path] = []
    errors: list[str] = []

    for line_number, line in enumerate(path.read_text().splitlines(), 1):
        raw = line.strip()
        if not raw or raw.startswith("#"):
            continue
        if raw.startswith("/"):
            current_dir = Path(raw).expanduser()
            if not current_dir.is_dir():
                errors.append(f"{path}:{line_number}: directory not found: {current_dir}")
            continue
        if current_dir is None:
            errors.append(f"{path}:{line_number}: instance appears before a directory: {raw}")
            continue
        candidate = current_dir / raw
        if candidate.suffix != ".sop":
            candidate = Path(str(candidate) + ".sop")
        if not candidate.is_file():
            errors.append(f"{path}:{line_number}: instance not found: {candidate}")
        else:
            instances.append(candidate.resolve())

    if errors:
        raise ValueError("\n".join(errors))
    if not instances:
        raise ValueError(f"No instances found in {path}")
    return instances


def run(command: list[str], cwd: Path, stdout: Path, stderr: Path,
        timeout: float | None) -> tuple[str, int, float]:
    started = time.monotonic()
    with stdout.open("w") as out, stderr.open("w") as err:
        err.write("$ " + shlex.join(command) + "\n\n")
        err.flush()
        try:
            completed = subprocess.run(command, cwd=cwd, stdout=out, stderr=err,
                                       timeout=timeout, check=False)
            status = "ok" if completed.returncode == 0 else "failed"
            code = completed.returncode
        except subprocess.TimeoutExpired:
            err.write(f"\nExternal timeout after {timeout:g} seconds.\n")
            status, code = "external_timeout", 124
        except OSError as exc:
            err.write(f"\n{exc}\n")
            status, code = "failed", 127
    return status, code, time.monotonic() - started


def build_command(compiler: str, binary: Path) -> list[str]:
    sources = sorted((SOLVER_DIR / "src").glob("*.cpp"))
    return [compiler, "-O3", "-DNDEBUG", "-std=c++17", "-pthread",
            "-I", str(SOLVER_DIR / "include"), *map(str, sources),
            "-o", str(binary)]


def read_result(path: Path, instance: Path) -> list[str]:
    with path.open(newline="") as stream:
        rows = list(csv.reader(stream))
    if len(rows) != 2 or rows[0] != HEADER or len(rows[1]) != len(HEADER):
        raise ValueError(f"expected one standard CSV result in {path}")
    if rows[1][0] != instance.name:
        raise ValueError(f"result instance {rows[1][0]!r} does not match {instance.name!r}")
    return rows[1]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--instances", type=Path, default=DEFAULT_INSTANCES)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--time-limit", type=float, default=3600,
                        help="solver timeout per instance in seconds (default: 3600)")
    parser.add_argument("--threads", type=int, default=32,
                        help="solver worker threads (default: 32)")
    parser.add_argument("--timeout-grace", type=float, default=60,
                        help="extra seconds allowed for process shutdown")
    parser.add_argument("--compiler", default="g++")
    parser.add_argument("--dry-run", action="store_true",
                        help="validate and print commands without compiling or running")
    args = parser.parse_args()

    if args.time_limit <= 0 or args.timeout_grace < 0:
        parser.error("--time-limit must be positive and --timeout-grace cannot be negative")
    if not 1 <= args.threads <= 1024:
        parser.error("--threads must be between 1 and 1024")
    compiler = shutil.which(args.compiler)
    if compiler is None:
        parser.error(f"C++ compiler not found: {args.compiler}")
    if not args.instances.is_file():
        parser.error(f"instance list not found: {args.instances.resolve()}")
    if not SOLVER_DIR.is_dir():
        parser.error(f"solver directory not found: {SOLVER_DIR}")

    try:
        instances = read_instances(args.instances.resolve())
    except (OSError, ValueError) as exc:
        parser.error(str(exc))

    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    run_dir = (args.output_dir / f"run-{stamp}").resolve()
    solver_dir = run_dir / "chatgpt"
    binary = solver_dir / "sop_exact"
    build = build_command(compiler, binary)

    print(f"{len(instances)} instances; {args.threads} threads; "
          f"{args.time_limit:g}s solver timeout")
    print(f"Results: {run_dir}", flush=True)
    if args.dry_run:
        print("BUILD", shlex.join(build))
        for index, instance in enumerate(instances, 1):
            output = solver_dir / f"{index:03d}-{instance.name}.csv"
            command = [str(binary), str(instance), "--time-limit", str(args.time_limit),
                       "--threads", str(args.threads), "--output", str(output)]
            print(shlex.join(command))
        return 0

    run_dir.mkdir(parents=True, exist_ok=False)
    solver_dir.mkdir()
    (run_dir / "instances.txt").write_text("".join(f"{path}\n" for path in instances))

    status, code, _ = run(build, ROOT, solver_dir / "build.stdout.log",
                          solver_dir / "build.stderr.log", 600)
    if status != "ok":
        print(f"build failed (exit {code}); see {solver_dir / 'build.stderr.log'}",
              file=sys.stderr)
        return 1

    failed = False
    aggregate_path = run_dir / "chatgpt.csv"
    with aggregate_path.open("w", newline="") as aggregate:
        writer = csv.writer(aggregate)
        writer.writerow(HEADER)
        for index, instance in enumerate(instances, 1):
            prefix = solver_dir / f"{index:03d}-{instance.name}"
            output = Path(str(prefix) + ".csv")
            command = [str(binary), str(instance), "--time-limit", str(args.time_limit),
                       "--threads", str(args.threads), "--output", str(output)]
            print(f"[chatgpt] {index}/{len(instances)}: {instance.name}", flush=True)
            status, code, elapsed = run(
                command, ROOT, Path(str(prefix) + ".stdout.log"),
                Path(str(prefix) + ".stderr.log"), args.time_limit + args.timeout_grace)
            if status != "ok":
                print(f"[chatgpt] {instance.name}: {status} (exit {code}, "
                      f"{elapsed:.1f}s)", file=sys.stderr)
                failed = True
                continue
            try:
                writer.writerow(read_result(output, instance))
                aggregate.flush()
            except (OSError, ValueError, csv.Error) as exc:
                print(f"[chatgpt] invalid output for {instance.name}: {exc}",
                      file=sys.stderr)
                failed = True

    return 1 if failed else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
