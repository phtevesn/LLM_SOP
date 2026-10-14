#!/usr/bin/env python3
"""Run the Claude heuristic and branch-and-bound solver on an instance list."""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import List, Optional


SOLVER_DIR = Path(__file__).resolve().parent
REPO_ROOT = SOLVER_DIR.parents[1]
DEFAULT_INSTANCES = REPO_ROOT / "results" / "llm-env-resultsv1.1" / "instances.txt"
BEST_RE = re.compile(r"^BEST\s+(-?\d+)\b", re.MULTILINE)


def resolve_instance(raw: str) -> Optional[Path]:
    """Resolve an instance path, including the known list-file typos."""
    original = raw.strip()
    if not original or original.startswith("#"):
        return None

    candidates = [Path(os.path.expanduser(original))]

    # The current instances.txt contains a few missing separators and suffixes.
    repaired = original.replace("LLM_SOPtsplib", "LLM_SOP/tsplib")
    repaired = repaired.replace("LLM_SOPsoplib", "LLM_SOP/soplib")
    repaired_path = Path(os.path.expanduser(repaired))
    candidates.append(repaired_path)
    if repaired_path.suffix == "":
        candidates.append(Path(str(repaired_path) + ".sop"))

    for candidate in candidates:
        if candidate.is_file():
            if str(candidate) != original:
                print(f"Resolved {original} -> {candidate}")
            return candidate.resolve()

    print(f"SKIP missing instance: {original}", file=sys.stderr)
    return None


def run_checked(command: List[str], *, timeout: Optional[float] = None) -> subprocess.CompletedProcess:
    return subprocess.run(
        command,
        cwd=SOLVER_DIR,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout,
        check=False,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--instances",
        type=Path,
        default=DEFAULT_INSTANCES,
        help=f"instance-list file (default: {DEFAULT_INSTANCES})",
    )
    parser.add_argument("--ils-seconds", type=float, default=60.0)
    parser.add_argument("--bb-seconds", type=float, default=3600.0)
    args = parser.parse_args()

    if args.ils_seconds <= 0 or args.bb_seconds <= 0:
        parser.error("time limits must be positive")
    if not args.instances.is_file():
        print(f"Cannot open instance list: {args.instances}", file=sys.stderr)
        return 1

    build = run_checked(["g++", "-O2", "-o", "ils", "ils.cpp"])
    if build.returncode != 0:
        print(build.stdout, end="", file=sys.stderr)
        print("Failed to build ils.cpp", file=sys.stderr)
        return build.returncode or 1

    ran = 0
    for line in args.instances.read_text().splitlines():
        instance = resolve_instance(line)
        if instance is None:
            continue
        ran += 1
        print(f"\n===== {instance} =====", flush=True)

        convert = run_checked([sys.executable, "make_inst.py", str(instance)])
        if convert.stdout:
            print(convert.stdout, end="")
        if convert.returncode != 0:
            print("make_inst.py failed; skipping instance", file=sys.stderr)
            continue

        try:
            ils = run_checked(["./ils", str(args.ils_seconds)], timeout=args.ils_seconds + 30)
        except subprocess.TimeoutExpired:
            print("ils.cpp exceeded its external safety timeout; skipping B&B", file=sys.stderr)
            continue
        print(ils.stdout, end="")
        if ils.returncode != 0:
            print("ils.cpp failed; skipping B&B", file=sys.stderr)
            continue

        match = BEST_RE.search(ils.stdout)
        if match is None or re.search(r"\bvalid\s+1\b", ils.stdout) is None:
            print("Could not extract a valid heuristic upper bound; skipping B&B", file=sys.stderr)
            continue
        upper_bound = match.group(1)

        try:
            bb = run_checked(
                [sys.executable, "bb.py", upper_bound, str(instance), str(args.bb_seconds)],
                timeout=args.bb_seconds + 30,
            )
        except subprocess.TimeoutExpired:
            print("bb.py exceeded its external safety timeout", file=sys.stderr)
            continue
        print(bb.stdout, end="")
        if bb.returncode != 0:
            print("bb.py failed", file=sys.stderr)

    if ran == 0:
        print("No valid instances were found.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
