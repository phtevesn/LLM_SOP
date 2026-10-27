#!/usr/bin/env python3
"""Independent end-to-end oracle, format, deadline and concurrent append tests."""
import argparse
import csv
import io
import itertools
import math
from pathlib import Path
import random
import signal
import subprocess
import tempfile


def matrix_text(matrix, *, eof=True, wrap=7):
    n = len(matrix)
    values = [str(value) for row in matrix for value in row]
    lines = ["NAME : generated.sop", "COMMENT: free text DIMENSION: 999 EOF -1",
             "TYPE: SOP", f"DIMENSION : {n}", "EDGE_WEIGHT_TYPE: EXPLICIT",
             "EDGE_WEIGHT_FORMAT: FULL_MATRIX", "EDGE_WEIGHT_SECTION", str(n)]
    lines += [" \t".join(values[i:i + wrap]) for i in range(0, len(values), wrap)]
    if eof:
        lines += ["EOF"]
    return "\n".join(lines) + "\n\n"


def brute_force(matrix):
    n = len(matrix)
    predecessors = [{j for j in range(n) if i != j and matrix[i][j] < 0} for i in range(n)]
    best = math.inf

    def visit(path, seen, cost):
        nonlocal best
        if cost >= best:
            return
        if len(path) == n:
            best = cost
            return
        for vertex in range(n):
            if vertex not in seen and predecessors[vertex] <= seen:
                visit(path + [vertex], seen | {vertex}, cost + (matrix[path[-1]][vertex] if path else 0))
    visit([], set(), 0)
    return -1 if math.isinf(best) else best


def validate(matrix, row):
    path = [int(x) for x in row["Best Path"].split()]
    cost = int(row["Best Cost"])
    if cost == -1:
        assert not path and row["Optimal"] == "no", row
        return
    assert sorted(path) == list(range(len(matrix))), row
    pos = {node: i for i, node in enumerate(path)}
    for i in range(len(matrix)):
        for j in range(len(matrix)):
            if i != j and matrix[i][j] < 0:
                assert pos[j] < pos[i], (matrix, row)
    assert sum(matrix[a][b] for a, b in zip(path, path[1:])) == cost, row


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary")
    parser.add_argument("--cases", type=int, default=180)
    args = parser.parse_args()
    binary = str(Path(args.binary).resolve())
    rng = random.Random(937821)
    runs = 0
    with tempfile.TemporaryDirectory(prefix="sop-tests-") as directory:
        directory = Path(directory)

        def run(path, *flags, success=True):
            nonlocal runs
            process = subprocess.run([binary, str(path), *map(str, flags)], text=True,
                                     capture_output=True, timeout=60)
            runs += 1
            assert (process.returncode == 0) == success, (process.args, process.stdout, process.stderr)
            return process

        for case in range(args.cases):
            n = rng.randint(1, 9)
            scale = 10**10 if case % 11 == 0 else 1
            matrix = [[0 if i == j else rng.randint(0, 80) * scale for j in range(n)] for i in range(n)]
            order = list(range(n)); rng.shuffle(order)
            density = [0, 0.1, 0.35, 0.7, 1][case % 5]
            for i, before in enumerate(order):
                for after in order[i + 1:]:
                    if rng.random() < density:
                        matrix[after][before] = -rng.randint(1, 9)
            expected = brute_force(matrix)
            path = directory / f'case,{case}".sop'
            path.write_text(matrix_text(matrix, eof=case % 2 == 0, wrap=rng.randint(1, 19)))
            for threads in (1, 4):
                process = run(path, "--threads", threads, "--history-mb", 0 if case % 3 == 0 else 4,
                              "--connectivity", ["off", "auto", "always"][case % 3])
                rows = list(csv.DictReader(io.StringIO(process.stdout)))
                assert len(rows) == 1
                row = rows[0]
                validate(matrix, row)
                assert int(row["Best Cost"]) == expected and row["Optimal"] == "yes", (case, expected, row, process.stderr)
                assert row["Instance"] == path.name
                assert int(row["Instance Size"]) == n and float(row["Time"]) >= 0
                assert "[incumbent] cost=" in process.stderr

        # A path with three possible sources and sinks; optimal endpoints are
        # neither zero nor N-1. Also checks zero-cost arcs and large costs.
        matrix = [[0, 99, 99, 0], [99, 0, 0, 99], [99, 99, 0, 99], [99, 0, 99, 0]]
        path = directory / "endpoints.sop"
        path.write_text(matrix_text(matrix, eof=False))
        row = next(csv.DictReader(io.StringIO(run(path).stdout)))
        validate(matrix, row)
        assert row["Best Path"] == "0 3 1 2" and int(row["Best Cost"]) == 0

        # Cycle is a valid but infeasible input; no optimum path is reported.
        cycle = [[0, -1, 3], [4, 0, -1], [-1, 5, 0]]
        bad = directory / "cyclic.sop"; bad.write_text(matrix_text(cycle))
        process = run(bad)
        row = next(csv.DictReader(io.StringIO(process.stdout)))
        assert row["Best Cost"] == "-1" and row["Optimal"] == "no" and not row["Best Path"]
        assert "[infeasible]" in process.stderr

        # Zero deadline includes parsing and cannot claim a proof.
        row = next(csv.DictReader(io.StringIO(run(path, "--time-limit", 0).stdout)))
        assert row["Optimal"] == "no" and row["Best Cost"] == "-1" and not row["Best Path"]

        output = directory / "results.csv"
        first = run(path, "--output", output)
        second = run(path, "--output", output)
        assert first.stdout.startswith("Instance,") and second.stdout.startswith("Instance,")
        assert len(list(csv.DictReader(output.open()))) == 2
        third = run(path, "--output", output, "--no-header")
        assert not third.stdout.startswith("Instance,")
        assert len(list(csv.DictReader(output.open()))) == 3
        empty = directory / "empty.csv"; empty.touch()
        run(path, "--output", empty)
        assert empty.read_text().startswith("Instance,")
        no_header = directory / "no_header.csv"
        run(path, "--output", no_header, "--no-header")
        assert not no_header.read_text().startswith("Instance,")

        concurrent = directory / "concurrent.csv"
        processes = [subprocess.Popen([binary, str(path), "--output", str(concurrent)],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) for _ in range(8)]
        for process in processes:
            process.communicate(timeout=30)
            assert process.returncode == 0
        assert len(list(csv.DictReader(concurrent.open()))) == 8
        assert concurrent.read_text().count("Instance,Instance Size") == 1
        without_newline = directory / "without_newline.csv"
        without_newline.write_text(first.stdout.rstrip("\n"))
        run(path, "--output", without_newline)
        assert len(list(csv.DictReader(without_newline.open()))) == 2
        before = path.read_bytes()
        run(path, "--output", path, success=False)
        assert path.read_bytes() == before

        valid = matrix_text(matrix)
        invalid = [valid.replace("DIMENSION : 4", "DIMENSION : 1001"),
                   valid.replace("EDGE_WEIGHT_SECTION\n4", "EDGE_WEIGHT_SECTION\n5"),
                   valid.replace("FULL_MATRIX", "UPPER_ROW"),
                   valid.replace("EDGE_WEIGHT_SECTION", "WRONG_SECTION"),
                   valid.replace("0 \t99", "x \t99", 1),
                   valid.replace("0 \t99", "1 \t99", 1),
                   valid + "42\n", valid.replace("EOF", "9"),
                   valid[:valid.index("EDGE_WEIGHT_SECTION")] + "EDGE_WEIGHT_SECTION\n4\n0 1 2\n"]
        for text in invalid:
            bad.write_text(text); assert not run(bad, success=False).stdout
        for flags in [("--threads", "0"), ("--threads", "-1"), ("--time-limit", "nan"),
                      ("--time-limit", "-1"), ("--time-limit", "inf"), ("--unknown",),
                      ("--pin", "bad"), ("--threads",), ("--history-mb", "-1")]:
            run(path, *flags, success=False)

        # N=1000, reversed mandatory chain, costs >2^31, no EOF, deeply derived
        # endpoints. The matching certificate should prove this without branching.
        n = 1000
        large = [[0 if i == j else 10**10 for j in range(n)] for i in range(n)]
        for i in range(n - 1): large[i][i + 1] = -1
        large_path = directory / "large.sop"; large_path.write_text(matrix_text(large, eof=False, wrap=113))
        row = next(csv.DictReader(io.StringIO(run(large_path, "--threads", 4, "--time-limit", 20).stdout)))
        validate(large, row)
        assert row["Optimal"] == "yes" and int(row["Best Cost"]) == 999 * 10**10

        # Deep actual search (not merely parsing): 994 forced vertices and an
        # unconstrained six-node tail whose relaxed assignment has zero cycles.
        for i in range(n):
            for j in range(n): large[i][j] = 0 if i == j else 100
        for i in range(993):
            large[i + 1][i] = -1
            large[i][i + 1] = 1
        for i in range(994, n):
            large[i][993] = -1
            large[993][i] = 0
            for j in range(994, n):
                if i != j: large[i][j] = 0 if (i - 994) // 2 == (j - 994) // 2 else 5
        large_path.write_text(matrix_text(large, wrap=127))
        row = next(csv.DictReader(io.StringIO(run(large_path, "--connectivity", "off", "--heuristic-seconds", 0,
                                                   "--history-mb", 4, "--time-limit", 30).stdout)))
        validate(large, row)
        assert row["Optimal"] == "yes" and int(row["Best Cost"]) == 1003
        assert int(row["Enumerated Nodes"]) >= 990

        # Exercise the concurrent heuristic, queues and all 32 requested workers
        # on larger inputs with an independently enumerated 2^12 solution space.
        for repeat in range(3):
            n = 24
            labels = list(range(n)); rng.shuffle(labels)
            layered = [[0 if i == j else rng.randint(0, 100) for j in range(n)] for i in range(n)]
            for layer in range(1, n // 2):
                for after in labels[2 * layer:2 * layer + 2]:
                    for previous in labels[2 * layer - 2:2 * layer]: layered[after][previous] = -1
            expected = brute_force(layered)
            path.write_text(matrix_text(layered))
            row = next(csv.DictReader(io.StringIO(run(path, "--threads", [1, 4, 32][repeat],
                                                       "--history-mb", 4, "--heuristic-seconds", 0.03).stdout)))
            validate(layered, row)
            assert row["Optimal"] == "yes" and int(row["Best Cost"]) == expected
            for limit in (0.00001, 0.001, 0.01):
                row = next(csv.DictReader(io.StringIO(run(path, "--threads", 4, "--time-limit", limit).stdout)))
                validate(layered, row)
                if row["Optimal"] == "yes": assert int(row["Best Cost"]) == expected

        # Controlled SIGTERM must preserve a checked incumbent and CSV output.
        n = 60
        hard = [[0 if i == j else rng.randint(1, 10000) for j in range(n)] for i in range(n)]
        path.write_text(matrix_text(hard))
        process = subprocess.Popen([binary, str(path), "--threads", "4"], text=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        first_log = process.stderr.readline()
        assert "[incumbent]" in first_log
        process.send_signal(signal.SIGTERM)
        stdout, stderr = process.communicate(timeout=30)
        assert process.returncode == 0, stderr
        row = next(csv.DictReader(io.StringIO(stdout)))
        validate(hard, row)
        assert row["Optimal"] == "no" and int(row["Best Cost"]) >= 0
    print(f"Solver oracle/CLI: {args.cases} independent instances, {runs} executions passed")


if __name__ == "__main__":
    main()
