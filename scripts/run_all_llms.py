#!/usr/bin/env python3
"""Build and run every solver under llm-algorithms on one SOP instance list.

Each solver gets its own CSV and log under results/llm-env-resultsv1.1/llm-runs
by default.  Solver-specific optional dependencies are allowed to fail; the
remaining solvers are still run and the summary reports the failure.
"""

from __future__ import annotations

import argparse
import csv
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Optional


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_INSTANCES = ROOT / "results" / "llm-env-resultsv1.1" / "instances.txt"
DEFAULT_OUTPUT = ROOT / "results" / "llm-env-resultsv1.1" / "llm-runs"


@dataclass
class SolverResult:
    name: str
    status: str
    detail: str


def resolve_instance(raw: str) -> Optional[Path]:
    """Resolve an instance-list entry, including the list's known typos."""
    raw = raw.strip()
    if not raw or raw.startswith("#"):
        return None

    repaired = raw.replace("LLM_SOPtsplib", "LLM_SOP/tsplib")
    repaired = repaired.replace("LLM_SOPsoplib", "LLM_SOP/soplib")
    candidates = [Path(os.path.expanduser(raw)), Path(os.path.expanduser(repaired))]
    if not repaired.endswith(".sop"):
        candidates.append(Path(os.path.expanduser(repaired + ".sop")))
    if not Path(repaired).is_absolute():
        candidates.extend(ROOT / candidate for candidate in list(candidates))

    seen: set[Path] = set()
    for candidate in candidates:
        candidate = candidate.resolve()
        if candidate in seen:
            continue
        seen.add(candidate)
        if candidate.is_file():
            return candidate
    return None


def read_instances(path: Path) -> tuple[list[Path], list[str]]:
    instances: list[Path] = []
    missing: list[str] = []
    for line in path.read_text().splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        resolved = resolve_instance(line)
        if resolved is None:
            missing.append(line.strip())
        else:
            instances.append(resolved)
    return instances, missing


def display_command(command: list[str]) -> str:
    return " ".join(shlex.quote(part) for part in command)


def run_process(
    command: list[str],
    *,
    cwd: Path,
    stdout_path: Path,
    stderr_path: Path,
    timeout: Optional[float] = None,
) -> int:
    if stdout_path == stderr_path:
        with stdout_path.open("w") as stream:
            stream.write("$ " + display_command(command) + "\n\n")
            stream.flush()
            try:
                completed = subprocess.run(
                    command, cwd=cwd, stdout=stream, stderr=stream,
                    text=True, timeout=timeout, check=False,
                )
                return completed.returncode
            except subprocess.TimeoutExpired:
                stream.write(f"\nExternal timeout after {timeout:.1f} seconds.\n")
                return 124
    with stdout_path.open("w") as stdout, stderr_path.open("w") as stderr:
        stdout.write("$ " + display_command(command) + "\n\n")
        stdout.flush()
        try:
            completed = subprocess.run(
                command, cwd=cwd, stdout=stdout, stderr=stderr,
                text=True, timeout=timeout, check=False,
            )
            return completed.returncode
        except subprocess.TimeoutExpired:
            stderr.write(f"\nExternal timeout after {timeout:.1f} seconds.\n")
            return 124


def build_cpp(
    name: str,
    source: Path,
    output_dir: Path,
    *,
    extra: Optional[list[str]] = None,
) -> tuple[Optional[Path], Optional[str]]:
    compiler = shutil.which("g++") or shutil.which("c++")
    if compiler is None:
        return None, "no C++ compiler (g++ or c++) found"
    binary = output_dir / f"{name}.bin"
    command = [compiler, "-O2", "-std=c++17", "-pthread", str(source), "-o", str(binary)]
    if extra:
        command[1:1] = extra
    log = output_dir / f"{name}.build.log"
    rc = run_process(command, cwd=ROOT, stdout_path=log, stderr_path=log)
    if rc != 0:
        return None, f"build failed; see {log}"
    return binary, None


def write_paths(path: Path, instances: Iterable[Path]) -> None:
    path.write_text("".join(str(instance) + "\n" for instance in instances))


def run_standard_solvers(
    instances: list[Path],
    output_dir: Path,
    seconds: float,
    *,
    claude1_heuristic_seconds: float,
    claude1_bb_seconds: float,
) -> list[SolverResult]:
    paths = [str(instance) for instance in instances]
    results: list[SolverResult] = []

    cpp_specs = [
        ("chatgpt", ROOT / "llm-algorithms/chatgpt/sop_solver.cpp", ["--time-limit", str(seconds), "--output"]),
        ("claude-iter-2", ROOT / "llm-algorithms/claude-iter-2/sop_solver.cpp", ["-t", str(seconds), "-o"]),
        ("deepseek", ROOT / "llm-algorithms/deepseek/deepseek_cpp_20261013_f2f3ce.cpp", None),
    ]
    for name, source, options in cpp_specs:
        binary, error = build_cpp(name, source, output_dir)
        if error:
            print(f"[{name}] {error}", file=sys.stderr)
            results.append(SolverResult(name, "failed", error))
            continue
        csv_path = output_dir / f"{name}.csv"
        if name == "deepseek":
            command = [str(binary), *paths, f"--time={seconds}", f"--out={csv_path}"]
        else:
            command = [str(binary), *options[: -1], options[-1], str(csv_path), *paths]
        log = output_dir / f"{name}.log"
        rc = run_process(
            command,
            cwd=ROOT,
            stdout_path=output_dir / f"{name}.stdout.log",
            stderr_path=log,
            timeout=max(60.0, seconds * len(instances) + 60.0),
        )
        results.append(SolverResult(name, "ok" if rc == 0 else "failed", str(csv_path)))

    python_specs = [
        ("gemini", ROOT / "llm-algorithms/gemini/gemini-code-1791911799685.py", ["-t", "--time-limit", "-o", "--output"]),
        ("grok", ROOT / "llm-algorithms/grok/sop_solver.py", ["--time", "--out"]),
        ("metaai", ROOT / "llm-algorithms/metaai/Sop-Solver.py", ["--time-limit", "--output"]),
    ]
    for name, source, options in python_specs:
        csv_path = output_dir / f"{name}.csv"
        if name == "gemini":
            command = [sys.executable, str(source), options[0], str(seconds), options[2], str(csv_path), *paths]
        elif name == "grok":
            command = [sys.executable, str(source), options[0], str(seconds), options[1], str(csv_path), *paths]
        else:
            command = [sys.executable, str(source), options[0], str(seconds), options[1], str(csv_path), *paths]
        rc = run_process(
            command,
            cwd=ROOT,
            stdout_path=output_dir / f"{name}.stdout.log",
            stderr_path=output_dir / f"{name}.log",
            timeout=max(60.0, seconds * len(instances) + 60.0),
        )
        results.append(SolverResult(name, "ok" if rc == 0 else "failed", str(csv_path)))

    # Claude iteration 1 has an intermediate inst.txt and a separate B&B CLI.
    name = "claude-iter-1"
    source_dir = ROOT / "llm-algorithms/claude-iter-1"
    binary, error = build_cpp(name, source_dir / "ils.cpp", output_dir)
    if error:
        print(f"[{name}] {error}", file=sys.stderr)
        results.append(SolverResult(name, "failed", error))
    else:
        log_path = output_dir / f"{name}.log"
        with log_path.open("w") as log:
            log.write(f"built {binary}\n")
            with tempfile.TemporaryDirectory(prefix="claude-iter-1-", dir="/tmp") as temp:
                temp_dir = Path(temp)
                failed = False
                best_re = re.compile(r"^BEST\s+(-?\d+)\b", re.MULTILINE)
                for instance in instances:
                    log.write(f"\n===== {instance} =====\n")
                    convert = subprocess.run(
                        [sys.executable, str(source_dir / "make_inst.py"), str(instance)],
                        cwd=temp_dir, stdout=log, stderr=log, text=True, check=False,
                    )
                    if convert.returncode != 0:
                        failed = True
                        continue
                    try:
                        ils = subprocess.run(
                            [str(binary), str(claude1_heuristic_seconds)],
                            cwd=temp_dir, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, timeout=claude1_heuristic_seconds + 60.0, check=False,
                        )
                        log.write(ils.stdout)
                    except subprocess.TimeoutExpired as exc:
                        log.write((exc.stdout or "") if isinstance(exc.stdout, str) else "")
                        log.write("ILS external timeout\n")
                        failed = True
                        continue
                    match = best_re.search(ils.stdout)
                    if ils.returncode != 0 or match is None:
                        failed = True
                        continue
                    try:
                        bb = subprocess.run(
                            [sys.executable, str(source_dir / "bb.py"), match.group(1), str(instance), str(claude1_bb_seconds)],
                            cwd=source_dir, stdout=log, stderr=log, text=True,
                            timeout=claude1_bb_seconds + 60.0, check=False,
                        )
                        failed |= bb.returncode != 0
                    except subprocess.TimeoutExpired:
                        log.write("B&B external timeout\n")
                        failed = True
        results.append(SolverResult(name, "failed" if failed else "ok", str(log_path)))

    return results


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--instances", type=Path, default=DEFAULT_INSTANCES)
    parser.add_argument(
        "--time-limit",
        type=float,
        default=3600.0,
        help="seconds per instance for most solvers (default: 3600)",
    )
    parser.add_argument(
        "--claude1-heuristic-time-limit",
        type=float,
        default=180.0,
        help="seconds for Claude iteration 1 ILS heuristic (default: 180)",
    )
    parser.add_argument(
        "--claude1-bb-time-limit",
        type=float,
        default=3600.0,
        help="seconds for Claude iteration 1 branch-and-bound (default: 3600)",
    )
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--dry-run", action="store_true", help="resolve inputs and print planned commands without running")
    args = parser.parse_args()
    if (
        args.time_limit <= 0
        or args.claude1_heuristic_time_limit <= 0
        or args.claude1_bb_time_limit <= 0
    ):
        parser.error("time limits must be positive")
    if not args.instances.is_file():
        parser.error(f"instance list does not exist: {args.instances}")

    instances, missing = read_instances(args.instances.resolve())
    if missing:
        for item in missing:
            print(f"WARNING: unresolved instance: {item}", file=sys.stderr)
    if not instances:
        parser.error("no valid instances found")
    print(f"Resolved {len(instances)} instance(s) from {args.instances}")
    for instance in instances:
        print(f"  {instance}")
    if args.dry_run:
        print("Dry run: no solvers executed.")
        return 0

    args.output_dir.mkdir(parents=True, exist_ok=True)
    results = run_standard_solvers(
        instances,
        args.output_dir.resolve(),
        args.time_limit,
        claude1_heuristic_seconds=args.claude1_heuristic_time_limit,
        claude1_bb_seconds=args.claude1_bb_time_limit,
    )
    summary = args.output_dir / "summary.csv"
    with summary.open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["solver", "status", "artifact"])
        writer.writerows((result.name, result.status, result.detail) for result in results)
    print(f"\nSummary: {summary}")
    for result in results:
        print(f"  {result.name}: {result.status} ({result.detail})")
    return 0 if all(result.status == "ok" for result in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
