#!/usr/bin/env python3
"""Build and run the v1.5 Claude, Gemini, and ChatGPT SOP solvers."""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_INSTANCES = ROOT / "results" / "llm-env-resultsv1.5" / "instances2.txt"
DEFAULT_OUTPUT = ROOT / "results" / "llm-env-resultsv1.5"
GEMINI_DIR = ROOT / "llm-algorithms" / "v1.5" / "v1.5-gemini"
CHATGPT_DIR = ROOT / "llm-algorithms" / "v1.5" / "v1.5-chatgpt"
CLAUDE_ARCHIVE = ROOT / "llm-algorithms" / "v1.5" / "v1.5-claude" / "sop_solver.tar.gz"
HEADER = ["Instance", "Instance Size", "Time", "Enumerated Nodes", "Best Cost", "Best Path", "Optimal"]


@dataclass(frozen=True)
class Solver:
    name: str
    source_root: Path
    include_dir: Path | None
    native: bool


def read_instances(path: Path) -> list[Path]:
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


def execute(command: list[str], cwd: Path, stdout: Path, stderr: Path,
            timeout: float | None) -> tuple[str, int, float]:
    started = time.monotonic()
    with stdout.open("w") as out, stderr.open("w") as err:
        err.write("$ " + shlex.join(command) + "\n\n")
        err.flush()
        try:
            result = subprocess.run(command, cwd=cwd, stdout=out, stderr=err,
                                    timeout=timeout, check=False)
            status = "ok" if result.returncode == 0 else "failed"
            code = result.returncode
        except subprocess.TimeoutExpired:
            err.write(f"\nExternal timeout after {timeout:g} seconds.\n")
            status, code = "external_timeout", 124
        except OSError as exc:
            err.write(f"\n{exc}\n")
            status, code = "failed", 127
    return status, code, time.monotonic() - started


def sources_for(solver: Solver) -> list[Path]:
    return sorted(solver.source_root.glob("*.cpp"))


def build_command(solver: Solver, compiler: str, binary: Path) -> list[str]:
    flags = ["-O3", "-std=c++17", "-pthread"]
    if solver.native:
        flags.insert(1, "-march=native")
    command = [compiler, *flags]
    if solver.include_dir is not None:
        command += ["-I", str(solver.include_dir)]
    command += [str(source) for source in sources_for(solver)]
    command += ["-o", str(binary)]
    return command


def extract_claude(destination: Path) -> Path:
    with tarfile.open(CLAUDE_ARCHIVE, "r:gz") as archive:
        archive.extractall(destination)
    source_root = destination / "sop_solver"
    if not source_root.is_dir():
        raise ValueError(f"Claude archive has no sop_solver directory: {CLAUDE_ARCHIVE}")
    return source_root


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
                        help="solver timeout per instance in seconds")
    parser.add_argument("--threads", type=int, default=32)
    parser.add_argument("--timeout-grace", type=float, default=60,
                        help="extra seconds allowed for process shutdown")
    parser.add_argument("--compiler", default="g++")
    parser.add_argument("--dry-run", action="store_true",
                        help="validate inputs and print commands without compiling or running")
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
    if not GEMINI_DIR.is_dir() or not CHATGPT_DIR.is_dir() or not CLAUDE_ARCHIVE.is_file():
        parser.error("missing one or more v1.5 solver sources")
    try:
        instances = read_instances(args.instances.resolve())
    except (OSError, ValueError) as exc:
        parser.error(str(exc))

    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    run_dir = (args.output_dir / f"run-{stamp}").resolve()
    print(f"{len(instances)} instances x 3 solvers; {args.threads} threads; {args.time_limit:g}s solver limit")
    print(f"Results: {run_dir}", flush=True)

    with tempfile.TemporaryDirectory(prefix="v1.5-claude-") as temp:
        claude_root = extract_claude(Path(temp))
        solvers = [
            Solver("claude", claude_root / "src", None, True),
            Solver("gemini", GEMINI_DIR, None, True),
            Solver("chatgpt", CHATGPT_DIR / "src", CHATGPT_DIR / "include", False),
        ]
        if args.dry_run:
            for solver in solvers:
                binary = run_dir / solver.name / ("sop_exact" if solver.name == "chatgpt" else "sop_solver")
                print("BUILD", shlex.join(build_command(solver, compiler, binary)))
            return 0

        run_dir.mkdir(parents=True, exist_ok=False)
        (run_dir / "instances.txt").write_text("".join(f"{path}\n" for path in instances))
        failed = False
        for solver in solvers:
            solver_dir = run_dir / solver.name
            solver_dir.mkdir()
            binary = solver_dir / ("sop_exact" if solver.name == "chatgpt" else "sop_solver")
            print(f"[{solver.name}] building", flush=True)
            status, code, _ = execute(
                build_command(solver, compiler, binary), ROOT,
                solver_dir / "build.stdout.log", solver_dir / "build.stderr.log", 600)
            if status != "ok":
                print(f"[{solver.name}] build failed (exit {code}); see {solver_dir / 'build.stderr.log'}",
                      file=sys.stderr)
                failed = True
                continue

            with (run_dir / f"{solver.name}.csv").open("w", newline="") as aggregate:
                writer = csv.writer(aggregate)
                writer.writerow(HEADER)
                for index, instance in enumerate(instances, 1):
                    prefix = solver_dir / f"{index:03d}-{instance.name}"
                    output = Path(str(prefix) + ".csv")
                    command = [str(binary), str(instance), "--time-limit", str(args.time_limit),
                               "--threads", str(args.threads), "--output", str(output)]
                    print(f"[{solver.name}] {index}/{len(instances)}: {instance.name}", flush=True)
                    status, code, elapsed = execute(
                        command, ROOT, Path(str(prefix) + ".stdout.log"),
                        Path(str(prefix) + ".stderr.log"), args.time_limit + args.timeout_grace)
                    if status != "ok":
                        print(f"[{solver.name}] {instance.name}: {status} (exit {code}, {elapsed:.1f}s)",
                              file=sys.stderr)
                        failed = True
                        continue
                    try:
                        writer.writerow(read_result(output, instance))
                        aggregate.flush()
                    except (OSError, ValueError, csv.Error) as exc:
                        print(f"[{solver.name}] invalid output for {instance.name}: {exc}", file=sys.stderr)
                        failed = True
        return 1 if failed else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
