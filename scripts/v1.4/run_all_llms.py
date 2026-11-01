#!/usr/bin/env python3
"""Build and run the three v1.4 SOP solvers on an instance list.

The instance-list format is a directory on a line beginning with '/', followed
by instance stems (the ``.sop`` suffix is added automatically).  A later
absolute directory line changes the directory used for following stems.
"""

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


# This file lives in scripts/v1.4/, so the repository root is two parents up.
ROOT = Path(__file__).resolve().parents[2]
DEFAULT_INSTANCES = ROOT / "results/llm-env-resultsv1.4/instances.txt"
DEFAULT_OUTPUT = ROOT / "results/llm-env-resultsv1.4"
HEADER = ["Instance", "Instance Size", "Time", "Enumerated Nodes",
          "Best Cost", "Best Path", "Optimal"]

SOLVERS = {
    "chatgpt": ROOT / "llm-algorithms/v1.4/v1.4-chatgpt",
    "claude": ROOT / "llm-algorithms/v1.4/v1.4-claude",
    "gemini": ROOT / "llm-algorithms/v1.4/v1.4-gemini",
}


def read_instances(list_path: Path) -> list[Path]:
    """Read directory-switching instance lists and resolve every .sop file."""
    current_dir: Path | None = None
    instances: list[Path] = []
    errors: list[str] = []

    for line_number, line in enumerate(list_path.read_text().splitlines(), 1):
        raw = line.strip()
        if not raw or raw.startswith("#"):
            continue
        if raw.startswith("/"):
            current_dir = Path(raw).expanduser()
            if not current_dir.is_dir():
                errors.append(f"{list_path}:{line_number}: directory not found: {current_dir}")
            continue
        if current_dir is None:
            errors.append(f"{list_path}:{line_number}: instance appears before a directory: {raw}")
            continue

        candidate = current_dir / raw
        if candidate.suffix != ".sop":
            candidate = Path(str(candidate) + ".sop")
        if not candidate.is_file():
            errors.append(f"{list_path}:{line_number}: instance not found: {candidate}")
        else:
            instances.append(candidate.resolve())

    if errors:
        raise ValueError("\n".join(errors))
    if not instances:
        raise ValueError(f"No instances found in {list_path}")
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


def source_files(name: str) -> list[Path]:
    directory = SOLVERS[name]
    if name == "chatgpt":
        return sorted((directory / "src").glob("*.cpp"))
    if name == "claude":
        return sorted((directory / "src").glob("*.cpp"))
    return sorted(directory.glob("*.cpp"))


def build_command(name: str, compiler: str, binary: Path) -> list[str]:
    directory = SOLVERS[name]
    command = [compiler, "-O3", "-DNDEBUG", "-std=c++17", "-pthread", "-o", str(binary)]
    if name == "chatgpt":
        command += ["-I", str(directory / "include")]
    command += [str(path) for path in source_files(name)]
    return command


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
                        help="seconds per solver/instance (default: 3600)")
    parser.add_argument("--threads", type=int, default=32)
    parser.add_argument("--compiler", default="g++")
    parser.add_argument("--chatgpt-history-mb", type=int, default=256)
    parser.add_argument("--claude-memory-gb", type=float, default=1.0)
    parser.add_argument("--timeout-grace", type=float, default=60)
    parser.add_argument("--dry-run", action="store_true",
                        help="validate and print commands without compiling or running")
    args = parser.parse_args()

    if args.time_limit <= 0 or args.timeout_grace < 0:
        parser.error("--time-limit must be positive and --timeout-grace cannot be negative")
    if not 1 <= args.threads <= 4096:
        parser.error("--threads must be between 1 and 4096")
    if args.chatgpt_history_mb < 0 or args.claude_memory_gb <= 0:
        parser.error("memory settings are out of range")

    compiler = shutil.which(args.compiler)
    if compiler is None:
        parser.error(f"C++ compiler not found: {args.compiler}")
    try:
        instances = read_instances(args.instances.resolve())
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    for name in SOLVERS:
        files = source_files(name)
        if not files:
            parser.error(f"no C++ sources found for {name}")

    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    run_dir = (args.output_dir / f"run-{stamp}").resolve()
    print(f"{len(instances)} instances x {len(SOLVERS)} solvers")
    print(f"Results: {run_dir}", flush=True)
    if args.dry_run:
        run_dir.mkdir(parents=True, exist_ok=True)
    else:
        run_dir.mkdir(parents=True, exist_ok=False)
        (run_dir / "instances.txt").write_text("".join(f"{path}\n" for path in instances))

    failed = False
    for name in SOLVERS:
        solver_dir = run_dir / name
        solver_dir.mkdir()
        binary = solver_dir / "sop_solver"
        build = build_command(name, compiler, binary)
        print("BUILD", shlex.join(build), flush=True)
        if args.dry_run:
            for instance in instances:
                output = solver_dir / f"{instance.name}.csv"
                command = [str(binary), str(instance), "--time-limit", str(args.time_limit),
                           "--threads", str(args.threads), "--output", str(output)]
                if name == "chatgpt":
                    command += ["--history-mb", str(args.chatgpt_history_mb)]
                elif name == "claude":
                    command += ["--memory-gb", str(args.claude_memory_gb)]
                print(shlex.join(command))
            continue

        status, code, elapsed = run(build, ROOT, solver_dir / "build.stdout.log",
                                    solver_dir / "build.stderr.log", 300)
        if status != "ok":
            print(f"[{name}] build failed (exit {code}); see {solver_dir}/build.stderr.log",
                  file=sys.stderr)
            failed = True
            continue

        aggregate_path = run_dir / f"{name}.csv"
        with aggregate_path.open("w", newline="") as aggregate:
            writer = csv.writer(aggregate)
            writer.writerow(HEADER)
            for index, instance in enumerate(instances, 1):
                output = solver_dir / f"{index:03d}-{instance.name}.csv"
                prefix = solver_dir / f"{index:03d}-{instance.name}"
                command = [str(binary), str(instance), "--time-limit", str(args.time_limit),
                           "--threads", str(args.threads), "--output", str(output)]
                if name == "chatgpt":
                    command += ["--history-mb", str(args.chatgpt_history_mb)]
                elif name == "claude":
                    command += ["--memory-gb", str(args.claude_memory_gb)]
                print(f"[{name}] {index}/{len(instances)}: {instance.name}", flush=True)
                status, code, elapsed = run(command, ROOT, prefix.with_suffix(".stdout.log"),
                                            prefix.with_suffix(".stderr.log"),
                                            args.time_limit + args.timeout_grace)
                if status == "ok":
                    try:
                        writer.writerow(read_result(output, instance))
                        aggregate.flush()
                    except (OSError, ValueError, csv.Error) as exc:
                        print(f"[{name}] invalid output for {instance.name}: {exc}", file=sys.stderr)
                        failed = True
                else:
                    print(f"[{name}] {instance.name}: {status} (exit {code}, {elapsed:.1f}s)",
                          file=sys.stderr)
                    failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
