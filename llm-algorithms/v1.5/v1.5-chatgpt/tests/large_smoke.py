#!/usr/bin/env python3
"""Exercise a non-unique 1000-node instance with a deadline and independent validation."""
import argparse
import csv
import io
from pathlib import Path
import random
import subprocess
import tempfile

from integration import validate, write_instance


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--solver", type=Path, required=True)
    parser.add_argument("--seconds", type=float, default=3)
    args = parser.parse_args()
    rng = random.Random(10002026)
    n = 1000
    matrix = [[0 if i == j else rng.randrange(1, 1001) for j in range(n)] for i in range(n)]
    order = list(range(n)); rng.shuffle(order)
    for chain in [order[:500], order[500:]]:
        for a, b in zip(chain, chain[1:]):
            matrix[b][a] = -5
    with tempfile.TemporaryDirectory(prefix="sop_large_") as directory:
        file = Path(directory) / "two_chains_1000.sop"
        write_instance(file, matrix, eof=False, wrap=53)
        proc = subprocess.run([str(args.solver.resolve()), str(file), "--threads", "4", "--history-mb", "16",
                               "--time-limit", str(args.seconds)], capture_output=True, text=True, timeout=args.seconds + 60)
        assert proc.returncode == 0, proc.stderr
        row = list(csv.DictReader(io.StringIO(proc.stdout)))[0]
        validate(matrix, row)
        assert int(row["Best Cost"]) >= 0, row
        print(proc.stdout, end="")
        print(proc.stderr, end="", file=__import__("sys").stderr)


if __name__ == "__main__":
    main()
