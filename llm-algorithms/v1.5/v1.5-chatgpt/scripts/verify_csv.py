#!/usr/bin/env python3
"""Independently validate a CSV path against the original .sop matrix.

This checks feasibility and cost, not the search's optimality proof.
"""
import argparse
import csv
from pathlib import Path


def read_matrix(path):
    header, section = {}, []
    lines = path.read_text(encoding="utf-8-sig").splitlines()
    for index, line in enumerate(lines):
        line = line.strip()
        if not line:
            continue
        fields = line.replace(":", " ", 1).split(None, 1)
        key = fields[0].upper()
        if key == "EDGE_WEIGHT_SECTION":
            section = " ".join(lines[index + 1:]).split()
            break
        if key != "COMMENT":
            header[key] = fields[1] if len(fields) > 1 else ""
    n = int(header["DIMENSION"])
    if not 1 <= n <= 1000 or not section or int(section[0]) != n:
        raise ValueError("invalid dimension")
    values = list(map(int, section[1:1 + n * n]))
    if len(values) != n * n or section[1 + n * n:] not in ([], ["EOF"], ["eof"]):
        raise ValueError("invalid matrix size or trailing content")
    matrix = [values[i * n:(i + 1) * n] for i in range(n)]
    if any(matrix[i][i] != 0 for i in range(n)):
        raise ValueError("nonzero diagonal")
    return matrix


def check(matrix, row):
    n = len(matrix)
    if int(row["Instance Size"]) != n:
        raise ValueError("instance size mismatch")
    path = [int(x) for x in row["Best Path"].split()]
    if not path:
        if int(row["Best Cost"]) != -1 or row["Optimal"] != "no":
            raise ValueError("inconsistent empty result")
        return "no feasible path reported"
    if sorted(path) != list(range(n)):
        raise ValueError("path is not a permutation")
    position = [0] * n
    for i, v in enumerate(path):
        position[v] = i
    for i in range(n):
        for j in range(n):
            if matrix[i][j] < 0 and position[j] >= position[i]:
                raise ValueError(f"precedence violated: {j} before {i}")
    total = sum(matrix[a][b] for a, b in zip(path, path[1:]))
    if total != int(row["Best Cost"]):
        raise ValueError(f"cost mismatch: recomputed {total}")
    return f"valid path, independently recomputed cost {total}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("instance", type=Path)
    parser.add_argument("csv", type=Path)
    args = parser.parse_args()
    matrix = read_matrix(args.instance)
    count = 0
    with args.csv.open(newline="") as stream:
        for index, row in enumerate(csv.DictReader(stream), start=1):
            if row["Instance"] == args.instance.name:
                print(f"row {index}: {check(matrix, row)}; Optimal field is a solver claim, not verified here")
                count += 1
    if not count:
        raise SystemExit("no matching instance filename in CSV")


if __name__ == "__main__":
    main()
