#!/usr/bin/env python3
"""Run all three v1.5 LLM SOP solvers on the same instance list.

This delegates to the existing ChatGPT and Claude/Gemini runners so that each
solver keeps its established build, logging, timeout, and CSV validation
behaviour.  The two runner invocations use the same instance list and output
directory, but create separate timestamped run directories.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_INSTANCES = ROOT / "results" / "llm-env-resultsv1.5" / "instances2.txt"
DEFAULT_OUTPUT = ROOT / "results" / "llm-env-resultsv1.5"
PAIR_RUNNER = Path(__file__).with_name("run_all_llms.py")
CHATGPT_RUNNER = Path(__file__).with_name("run_chatgpt.py")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--instances", type=Path, default=DEFAULT_INSTANCES,
                        help="directory-switching instance list")
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--time-limit", type=float, default=3600,
                        help="solver timeout per instance in seconds")
    parser.add_argument("--threads", type=int, default=32)
    parser.add_argument("--timeout-grace", type=float, default=60,
                        help="extra seconds allowed for process shutdown")
    parser.add_argument("--compiler", default="g++")
    parser.add_argument("--dry-run", action="store_true",
                        help="validate inputs and print each delegated command")
    args = parser.parse_args()

    if not args.instances.is_file():
        parser.error(f"instance list not found: {args.instances.resolve()}")
    if args.time_limit <= 0 or args.timeout_grace < 0:
        parser.error("--time-limit must be positive and --timeout-grace cannot be negative")
    if not 1 <= args.threads <= 4096:
        parser.error("--threads must be between 1 and 4096")

    shared = [
        "--instances", str(args.instances.resolve()),
        "--output-dir", str(args.output_dir.resolve()),
        "--time-limit", str(args.time_limit),
        "--threads", str(args.threads),
        "--timeout-grace", str(args.timeout_grace),
        "--compiler", args.compiler,
    ]
    if args.dry_run:
        shared.append("--dry-run")

    commands = [
        [sys.executable, str(PAIR_RUNNER), *shared],
        [sys.executable, str(CHATGPT_RUNNER), *shared],
    ]

    failed = False
    for command in commands:
        print("\n$ " + " ".join(command), flush=True)
        completed = subprocess.run(command, cwd=ROOT, check=False)
        if completed.returncode != 0:
            failed = True
            print(f"runner failed with exit code {completed.returncode}: {command[1]}",
                  file=sys.stderr)

    return 1 if failed else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
