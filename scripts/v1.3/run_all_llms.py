#!/usr/bin/env python3
"""Compile and run every v1.3 SOP solver on a list of instances."""

from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
import math
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[2]
OUTPUT = ROOT / "results/llm-env-resultsv1.3"
SOURCES = {
    "chatgpt": ROOT / "llm-algorithms/v1.3/v1.3-chatgpt/sop.cpp",
    "claude": ROOT / "llm-algorithms/v1.3/v1.3-claude/sop_solver.cpp",
    "gemini": ROOT / "llm-algorithms/v1.3/v1.3-gemini/sop_solver.cpp",
}
HEADER = ["Instance", "Instance Size", "Time", "Enumerated Nodes",
          "Best Cost", "Best Path", "Optimal"]


def read_instances(path: Path) -> list[Path]:
    instances: list[Path] = []
    errors: list[str] = []
    for number, line in enumerate(path.read_text().splitlines(), 1):
        raw = line.strip()
        if not raw or raw.startswith("#"):
            continue
        entry = Path(raw).expanduser()
        candidates = [entry] if entry.is_absolute() else [path.parent / entry, ROOT / entry]
        expanded = []
        for candidate in candidates:
            expanded.append(candidate)
            if candidate.suffix != ".sop":
                expanded.append(Path(str(candidate) + ".sop"))
        resolved = next((candidate.resolve() for candidate in expanded if candidate.is_file()), None)
        if resolved is None:
            errors.append(f"{path}:{number}: unresolved instance: {raw}")
        else:
            instances.append(resolved)
    if errors:
        raise ValueError("\n".join(errors))
    if not instances:
        raise ValueError(f"No instances in {path}")
    return instances


def cxx_standard(solver: str, compiler: str) -> str:
    if solver != "gemini":
        return "c++17"
    # GCC 9 accepts the C++20 draft spelling, c++2a, but not c++20.
    if Path(compiler).name.startswith("g++"):
        try:
            version = subprocess.run(
                [compiler, "-dumpfullversion", "-dumpversion"],
                capture_output=True, text=True, check=False,
            ).stdout.strip().splitlines()[0]
            if int(version.split(".", 1)[0]) < 10:
                return "c++2a"
        except (IndexError, ValueError, OSError):
            pass
    return "c++20"


def execute(command: list[str], stdout: Path, stderr: Path,
            timeout: float) -> tuple[str, int | str, float]:
    started = time.monotonic()
    with stdout.open("w") as out, stderr.open("w") as err:
        err.write("$ " + shlex.join(command) + "\n\n")
        err.flush()
        try:
            result = subprocess.run(command, cwd=ROOT, stdout=out, stderr=err,
                                    timeout=timeout, check=False)
            status, code = ("ok" if result.returncode == 0 else "failed"), result.returncode
        except subprocess.TimeoutExpired:
            err.write(f"\nExternal timeout after {timeout:g} seconds.\n")
            status, code = "external_timeout", 124
        except OSError as exc:
            err.write(f"\n{exc}\n")
            status, code = "failed", ""
    return status, code, time.monotonic() - started


def result_row(path: Path, instance: Path) -> list[str]:
    with path.open(newline="") as stream:
        rows = list(csv.reader(stream))
    if len(rows) != 2 or rows[0] != HEADER or len(rows[1]) != len(HEADER):
        raise ValueError("expected the standard header and exactly one result row")
    if rows[1][0] != instance.name:
        raise ValueError(f"result instance {rows[1][0]!r} does not match {instance.name!r}")
    return rows[1]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--instances", type=Path,
                        help="instance list; defaults to results/instances.txt, then the v1.3 or v1.1 list")
    parser.add_argument("--output-dir", type=Path, default=OUTPUT,
                        help="directory for a new run-* result directory")
    parser.add_argument("--time-limit", type=float, default=3600,
                        help="seconds per solver/instance (default: 3600)")
    parser.add_argument("--threads", type=int, default=32,
                        help="workers per solver (default: 32)")
    parser.add_argument("--timeout-grace", type=float, default=60,
                        help="extra seconds before forcibly stopping a solver (default: 60)")
    parser.add_argument("--claude-memory-mb", type=int, default=512,
                        help="Claude history-table budget in MiB (default: 512)")
    parser.add_argument("--compiler", default="g++", help="C++ compiler (default: g++)")
    parser.add_argument("--dry-run", action="store_true",
                        help="validate inputs and print commands without compiling or running")
    args = parser.parse_args()
    for name in ("time_limit", "timeout_grace"):
        if not math.isfinite(getattr(args, name)) or getattr(args, name) <= 0:
            parser.error(f"--{name.replace('_', '-')} must be finite and positive")
    if not 1 <= args.threads <= 4096:
        parser.error("--threads must be 1..4096")
    if args.claude_memory_mb <= 0:
        parser.error("--claude-memory-mb must be positive")

    if args.instances is None:
        choices = [ROOT / "results/instances.txt", OUTPUT / "instances.txt",
                   ROOT / "results/llm-env-resultsv1.1/instances.txt"]
        args.instances = next((path for path in choices if path.is_file()), None)
        if args.instances is None:
            parser.error("no instance list found; pass --instances PATH")
        print(f"Using instance list: {args.instances}", file=sys.stderr)
    try:
        instances = read_instances(args.instances.resolve())
    except (OSError, ValueError) as exc:
        parser.error(str(exc))

    compiler = shutil.which(args.compiler)
    if compiler is None:
        parser.error(f"C++ compiler not found: {args.compiler}")
    for source in SOURCES.values():
        if not source.is_file():
            parser.error(f"missing solver source: {source}")

    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    run_dir = args.output_dir.resolve() / f"run-{stamp}"
    print(f"{len(instances)} instances x {len(SOURCES)} solvers; {args.threads} threads each.")
    print(f"Results: {run_dir}", flush=True)
    if not args.dry_run:
        run_dir.mkdir(parents=True, exist_ok=False)
        (run_dir / "instances.txt").write_text("".join(f"{path}\n" for path in instances))

    failed = False
    summary_stream = None
    try:
        if not args.dry_run:
            summary_stream = (run_dir / "summary.csv").open("w", newline="")
            summary = csv.writer(summary_stream)
            summary.writerow(["solver", "instance", "status", "exit_code", "wall_seconds", "detail"])
            summary_stream.flush()

        def record(name: str, instance: str, status: str, code: int | str,
                   elapsed: float, detail: str) -> None:
            nonlocal failed
            failed |= status != "ok"
            if summary_stream is not None:
                summary.writerow([name, instance, status, code, f"{elapsed:.6f}", detail])
                summary_stream.flush()
            print(f"[{name}] {instance}: {status} {detail}", flush=True)

        for name, source in SOURCES.items():
            solver_dir = run_dir / name
            binary = solver_dir / "solver.bin"
            build = [compiler, "-O3", "-DNDEBUG", f"-std={cxx_standard(name, compiler)}",
                     "-pthread", str(source), "-o", str(binary)]
            if name == "chatgpt":
                build.insert(4, "-fopenmp")
            if args.dry_run:
                print(shlex.join(build))
            else:
                solver_dir.mkdir()
                status, code, elapsed = execute(build, solver_dir / "build.stdout.log",
                                                solver_dir / "build.stderr.log", 300)
                if status != "ok":
                    record(name, "<build>", "build_failed", code, elapsed,
                           str(solver_dir / "build.stderr.log"))
                    continue

            aggregate = None if args.dry_run else (run_dir / f"{name}.csv").open("w", newline="")
            try:
                writer = None
                if aggregate is not None:
                    writer = csv.writer(aggregate)
                    writer.writerow(HEADER)
                    aggregate.flush()
                for index, instance in enumerate(instances, 1):
                    prefix = solver_dir / f"{index:03d}-{instance.name}"
                    output = Path(str(prefix) + ".csv")
                    command = [str(binary), str(instance), "--time-limit", str(args.time_limit),
                               "--threads", str(args.threads), "--output", str(output)]
                    if name == "claude":
                        command += ["--mem-mb", str(args.claude_memory_mb)]
                    if args.dry_run:
                        print(shlex.join(command))
                        continue
                    print(f"[{name}] {index}/{len(instances)}: {instance.name}", flush=True)
                    status, code, elapsed = execute(
                        command, Path(str(prefix) + ".stdout.log"),
                        Path(str(prefix) + ".stderr.log"),
                        args.time_limit + args.timeout_grace,
                    )
                    detail = str(output)
                    if status == "ok":
                        try:
                            assert writer is not None and aggregate is not None
                            writer.writerow(result_row(output, instance))
                            aggregate.flush()
                        except (OSError, ValueError, csv.Error) as exc:
                            status, detail = "invalid_output", str(exc)
                    record(name, instance.name, status, code, elapsed, detail)
            finally:
                if aggregate is not None:
                    aggregate.close()
    finally:
        if summary_stream is not None:
            summary_stream.close()
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\nInterrupted; completed results remain in the run directory.", file=sys.stderr)
        raise SystemExit(130)
