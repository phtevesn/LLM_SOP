#!/usr/bin/env python3
"""Black-box solver checks against an independent raw-matrix subset-DP oracle."""
import argparse
import csv
import functools
import io
import os
from pathlib import Path
import random
import signal
import subprocess
import tempfile
import time


def write_instance(path, matrix, eof=True, wrap=11, crlf=False):
    n = len(matrix)
    tokens = [str(x) for row in matrix for x in row]
    text = (f"NAME : integration\nTYPE: SOP\nCOMMENT: 123 DIMENSION: 999 is free text\n"
            f"DIMENSION : {n}\nEDGE_WEIGHT_TYPE: EXPLICIT\nEDGE_WEIGHT_FORMAT: FULL_MATRIX\n"
            f"EDGE_WEIGHT_SECTION\n{n}\n")
    text += "\n".join(" \t".join(tokens[i:i+wrap]) for i in range(0, len(tokens), wrap))
    text += "\nEOF\n\n" if eof else "\n\n"
    path.write_bytes((text.replace("\n", "\r\n") if crlf else text).encode())


def random_matrix(n, rng, density):
    matrix = [[rng.randrange(100) if i != j else 0 for j in range(n)] for i in range(n)]
    order = list(range(n))
    rng.shuffle(order)
    for a in range(n):
        for b in range(a + 1, n):
            if rng.random() < density:
                matrix[order[b]][order[a]] = -rng.randrange(1, 7)
    return matrix


def oracle(matrix):
    n = len(matrix)
    pred = [sum(1 << j for j, value in enumerate(row) if value < 0) for row in matrix]
    full = (1 << n) - 1

    @functools.lru_cache(None)
    def dp(mask, last):
        if mask == full:
            return 0
        best = float("inf")
        for v in range(n):
            if mask >> v & 1 or pred[v] & ~mask:
                continue
            edge = 0 if last == n else matrix[last][v]
            if edge >= 0:
                best = min(best, edge + dp(mask | 1 << v, v))
        return best
    return dp(0, n)


def two_chain_oracle(matrix, a, b):
    """Independent polynomial recurrence for interleaving two fixed chains."""
    inf = float("inf")
    end_a = [[inf] * (len(b) + 1) for _ in range(len(a) + 1)]
    end_b = [[inf] * (len(b) + 1) for _ in range(len(a) + 1)]
    end_a[1][0] = end_b[0][1] = 0
    for i in range(len(a) + 1):
        for j in range(len(b) + 1):
            if i:
                if i > 1:
                    end_a[i][j] = min(end_a[i][j], end_a[i - 1][j] + matrix[a[i - 2]][a[i - 1]])
                if j:
                    end_a[i][j] = min(end_a[i][j], end_b[i - 1][j] + matrix[b[j - 1]][a[i - 1]])
            if j:
                if j > 1:
                    end_b[i][j] = min(end_b[i][j], end_b[i][j - 1] + matrix[b[j - 2]][b[j - 1]])
                if i:
                    end_b[i][j] = min(end_b[i][j], end_a[i][j - 1] + matrix[a[i - 1]][b[j - 1]])
    return min(end_a[-1][-1], end_b[-1][-1])


def validate(matrix, row):
    n = len(matrix)
    assert int(row["Instance Size"]) == n
    assert float(row["Time"]) >= 0 and int(row["Enumerated Nodes"]) >= 0
    path = [int(v) for v in row["Best Path"].split()]
    if not path:
        assert int(row["Best Cost"]) == -1 and row["Optimal"] == "no"
        return
    assert sorted(path) == list(range(n)), path
    position = {v: i for i, v in enumerate(path)}
    for i in range(n):
        for j in range(n):
            if matrix[i][j] < 0:
                assert position[j] < position[i], (j, i, path)
    assert sum(matrix[a][b] for a, b in zip(path, path[1:])) == int(row["Best Cost"])


def run(solver, file, *options, expected_exit=0):
    proc = subprocess.run([str(solver), str(file), *map(str, options)], text=True, capture_output=True, timeout=45)
    assert proc.returncode == expected_exit, (proc.returncode, proc.stderr, options)
    if expected_exit:
        return proc
    rows = list(csv.DictReader(io.StringIO(proc.stdout)))
    assert len(rows) == 1, proc.stdout
    return rows[0], proc


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--solver", type=Path, required=True)
    parser.add_argument("--cases", type=int, default=180)
    args = parser.parse_args()
    solver = args.solver.resolve()
    rng = random.Random(20261113)
    with tempfile.TemporaryDirectory(prefix="sop_tests_") as directory:
        root = Path(directory)
        file = root / 'quoted,"instance.sop'
        for trial in range(args.cases):
            n = 1 + trial % 11
            matrix = random_matrix(n, rng, (trial % 6) / 10)
            if trial % 17 == 0:
                matrix = [[v * 10**10 if v >= 0 else v for v in row] for row in matrix]
            write_instance(file, matrix, eof=trial % 2 == 0, wrap=1 + trial % 17, crlf=trial % 3 == 0)
            modes = ["--threads", 1 if trial % 3 == 0 else 4, "--tail-dp", 0 if trial % 2 == 0 else 8,
                     "--history-mb", 0 if trial % 4 == 0 else 1, "--heuristic-seconds", 0,
                     "--history-mode", "shared" if trial % 3 else "local",
                     "--component-bound", "off" if trial % 5 == 0 else "on", "--affinity", "none"]
            row, proc = run(solver, file, *modes)
            validate(matrix, row)
            assert row["Instance"] == file.name
            assert row["Optimal"] == "yes", proc.stderr
            assert int(row["Best Cost"]) == oracle(matrix), (trial, matrix, row, proc.stderr)
        # All starts/ends are choices; the optimum here starts at node 2 and ends at 1.
        matrix = [[0, 1, 99], [99, 0, 99], [1, 99, 0]]
        write_instance(file, matrix)
        # Exact values near the documented limit, and the most negative int64 marker.
        inf = (2**63 - 1) // 4
        cap = inf // (16 * 5 * 5)
        extreme = [[0 if i == j else cap - i * 7 - j for j in range(4)] for i in range(4)]
        extreme[1][3] = -(2**63)
        write_instance(file, extreme)
        row, _ = run(solver, file, "--tail-dp", 0, "--threads", 4, "--history-mb", 1)
        validate(extreme, row)
        assert int(row["Best Cost"]) == oracle(extreme) and row["Optimal"] == "yes"
        extreme[0][1] = cap + 1
        write_instance(file, extreme)
        run(solver, file, expected_exit=2)
        write_instance(file, matrix)
        row, _ = run(solver, file)
        assert row["Best Path"] == "2 0 1" and row["Best Cost"] == "2"
        # Symmetry classes can have nonzero mutual cost; equality is checked exactly.
        for n in [2, 7, 15]:
            symmetric = [[0 if i == j else 9 for j in range(n)] for i in range(n)]
            write_instance(file, symmetric)
            row, _ = run(solver, file, "--threads", 4)
            assert row["Best Cost"] == str(9 * (n - 1)) and row["Optimal"] == "yes"
            assert row["Best Path"] == " ".join(map(str, range(n)))
        write_instance(file, matrix)
        # Time zero is a deadline, never a request for unlimited search.
        row, _ = run(solver, file, "--time-limit", 0)
        assert row["Best Cost"] == "-1" and row["Best Path"] == "" and row["Optimal"] == "no"
        # Cyclic input is syntactically valid but infeasible.
        cyclic = [[0, -2, 5], [5, 0, -3], [-4, 5, 0]]
        write_instance(file, cyclic, eof=False)
        row, proc = run(solver, file)
        assert row["Best Cost"] == "-1" and "infeasible" in proc.stderr
        # CSV header and append rules, including inter-process file locking.
        simple = [[0, 2, 7], [-1, 0, 3], [-1, -1, 0]]
        write_instance(file, simple)
        out = root / "result.csv"
        run(solver, file, "--output", out)
        run(solver, file, "--output", out)
        processes = [subprocess.Popen([str(solver), str(file), "--output", str(out)], stdout=subprocess.PIPE,
                                      stderr=subprocess.PIPE, text=True) for _ in range(8)]
        for p in processes:
            p.communicate(timeout=10)
            assert p.returncode == 0
        rows = list(csv.DictReader(io.StringIO(out.read_text())))
        assert len(rows) == 10 and all(r["Best Cost"] == "5" for r in rows)
        no_header = root / "no_header.csv"
        p = subprocess.run([str(solver), str(file), "--output", str(no_header), "--no-header"], capture_output=True, text=True)
        assert p.returncode == 0 and len(list(csv.reader(io.StringIO(p.stdout)))) == 1
        assert p.stdout == no_header.read_text() and not p.stdout.startswith("Instance,")
        empty = root / "empty.csv"
        empty.touch()
        run(solver, file, "--output", empty)
        assert empty.read_text().startswith("Instance,")
        run(solver, file, "--output", file, expected_exit=2)
        hardlink = root / "hardlink.csv"
        os.link(file, hardlink)
        run(solver, file, "--output", hardlink, expected_exit=2)
        # Malformed input never emits a fabricated result row.
        good = file.read_text()
        invalid = [good.replace("DIMENSION : 3", "DIMENSION : 1001"),
                   good.replace("SECTION\n3", "SECTION\n4"), good.replace("SECTION\n3\n0", "SECTION\n3\n1"),
                   good.replace("FULL_MATRIX", "UPPER_ROW"), good + "123\n", good.replace("EOF", "45"),
                   good.rsplit("EOF", 1)[0].rsplit(None, 1)[0] + "\nEOF\n", good.replace("DIMENSION : 3", "DIMENSION : 2\nDIMENSION: 3"),
                   good.replace("SECTION\n3\n0", "SECTION\n3\n999999999999999999999999")]
        invalid.append(good.replace("SECTION\n3\n0", "SECTION\n3\n+-0"))
        bad = root / "bad.sop"
        for text in invalid:
            assert text != good, "malformed-input fixture did not change its source"
            bad.write_text(text)
            p = run(solver, bad, expected_exit=2)
            assert not p.stdout
        for option, value in [("--threads", "0"), ("--time-limit", "nan"), ("--time-limit", "-1"),
                              ("--tail-dp", "21"), ("--history-mb", "-1"), ("--affinity", "bad")]:
            run(solver, file, option, value, expected_exit=2)
        # Compare exact answers across bitset word boundaries with an independent
        # two-chain recurrence, rather than an impossible full 2^130 oracle.
        for n in [64, 65, 130]:
            order = list(range(n)); rng.shuffle(order)
            a, b = order[:n // 2], order[n // 2:]
            matrix = [[0 if i == j else rng.randrange(1, 1001) for j in range(n)] for i in range(n)]
            for chain in [a, b]:
                for x, y in zip(chain, chain[1:]):
                    matrix[y][x] = -2
            write_instance(file, matrix, eof=False)
            row, proc = run(solver, file, "--threads", 4, "--tail-dp", 0, "--history-mb", 2, "--heuristic-seconds", 0)
            validate(matrix, row)
            assert row["Optimal"] == "yes" and int(row["Best Cost"]) == two_chain_oracle(matrix, a, b), proc.stderr
        # A shuffled 1000-node chain checks the full dimension, closure, endpoint derivation,
        # non-metric costs, and stack-independent handling of a unique order.
        n = 1000
        order = list(range(n)); rng.shuffle(order)
        large = [[0 if i == j else 10000000 for j in range(n)] for i in range(n)]
        for a, b in zip(order, order[1:]):
            large[b][a] = -7; large[a][b] = 3
        write_instance(file, large, eof=False, wrap=53)
        row, _ = run(solver, file, "--threads", 32)
        validate(large, row)
        assert row["Best Cost"] == str(3 * (n - 1)) and row["Optimal"] == "yes"
        assert list(map(int, row["Best Path"].split())) == order
        # A hard instance exercises a live timeout and actual 32-worker startup.
        matrix = random_matrix(120, rng, 0.04)
        write_instance(file, matrix)
        row, _ = run(solver, file, "--threads", 32, "--history-mb", 8, "--time-limit", 0.15)
        validate(matrix, row)
        assert row["Optimal"] == "no" and int(row["Best Cost"]) >= 0, row
        # Signal cancellation must join workers and preserve a validated incumbent.
        proc = subprocess.Popen([str(solver), str(file), "--threads", "4", "--history-mb", "4"],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        saw_best = False
        for _ in range(10):
            line = proc.stderr.readline()
            if "best cost =" in line:
                saw_best = True; break
        assert saw_best
        proc.send_signal(signal.SIGINT)
        stdout, stderr = proc.communicate(timeout=10)
        assert proc.returncode == 0, stderr
        row = list(csv.DictReader(io.StringIO(stdout)))[0]
        validate(matrix, row)
        assert row["Optimal"] == "no" and int(row["Best Cost"]) >= 0
    print(f"integration passed: {args.cases} independent-DP cases; N=64/65/130 two-chain oracles; parser/CSV/deadline/signal cases; N=1000; 32-worker timeout")


if __name__ == "__main__":
    main()
