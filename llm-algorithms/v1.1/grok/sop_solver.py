#!/usr/bin/env python3
"""
Sequential Ordering Problem (SOP) – strongest solver available in this environment.

Key techniques
--------------
• Transitive closure + forced-arc detection + arc filtering
• Multi-start constructive heuristic + 2-opt + Or-opt
• Single-commodity flow MIP formulation (tighter than pure MTZ)
  + position variables for precedences
  + forced arcs fixed
• Multi-threaded CBC branch-and-cut
• CBC log parsed for enumerated nodes and gap

Requirements: Python 3.8+, PuLP (CBC bundled)

Usage
-----
    python sop_solver.py file1.sop [file2.sop ...] [--time SECS] [--out out.csv]
"""

from __future__ import annotations

import argparse
import csv
import os
import re
import sys
import tempfile
import time
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple

import pulp


# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------

def parse_sop(path: Path) -> Tuple[str, int, List[List[int]]]:
    text = path.read_text(encoding="utf-8", errors="replace").splitlines()
    name = path.stem
    n: Optional[int] = None
    matrix_started = False
    values: List[int] = []

    for line in text:
        line = line.strip()
        if not line:
            continue
        upper = line.upper()
        if upper.startswith("NAME"):
            parts = line.split(":", 1)
            if len(parts) == 2:
                name = parts[1].strip()
        elif upper.startswith("DIMENSION"):
            parts = line.split(":", 1)
            if len(parts) == 2:
                n = int(parts[1].strip())
        elif upper.startswith("EDGE_WEIGHT_SECTION"):
            matrix_started = True
            continue
        elif upper == "EOF":
            break
        elif matrix_started:
            for tok in line.split():
                try:
                    values.append(int(tok))
                except ValueError:
                    pass

    if n is None:
        raise ValueError(f"{path}: DIMENSION not found")
    if values and values[0] == n:
        values = values[1:]
    expected = n * n
    if len(values) < expected:
        raise ValueError(f"{path}: expected {expected} matrix entries, got {len(values)}")
    values = values[:expected]
    T = [values[i * n:(i + 1) * n] for i in range(n)]
    return name, n, T


# ---------------------------------------------------------------------------
# Precedence + forced-arc preprocessing
# ---------------------------------------------------------------------------

def build_precedence(T: List[List[int]]) -> Tuple[List[Set[int]], List[Set[int]]]:
    n = len(T)
    pred: List[Set[int]] = [set() for _ in range(n)]
    for i in range(n):
        for j in range(n):
            if T[i][j] < 0:
                pred[i].add(j)

    changed = True
    while changed:
        changed = False
        for i in range(n):
            add = set()
            for j in pred[i]:
                add |= pred[j]
            if not add.issubset(pred[i]):
                pred[i] |= add
                changed = True

    succ: List[Set[int]] = [set() for _ in range(n)]
    for i in range(n):
        for j in pred[i]:
            succ[j].add(i)
    return pred, succ


def find_start_end(pred: List[Set[int]], succ: List[Set[int]], n: int) -> Tuple[List[int], List[int]]:
    starts = [i for i in range(n) if not pred[i]]
    ends   = [i for i in range(n) if not succ[i]]
    return starts, ends


def detect_forced_arcs(
    T: List[List[int]], pred: List[Set[int]], succ: List[Set[int]], n: int
) -> Set[Tuple[int, int]]:
    forced: Set[Tuple[int, int]] = set()
    for i in range(n):
        candidates = []
        for j in range(n):
            if i == j or T[i][j] < 0:
                continue
            if i in pred[j]:
                continue
            candidates.append(j)
        if len(candidates) == 1:
            forced.add((i, candidates[0]))
    return forced


def filter_arcs(T: List[List[int]], pred: List[Set[int]], n: int) -> List[List[int]]:
    T2 = [row[:] for row in T]
    for i in range(n):
        for j in pred[i]:
            if T2[i][j] >= 0:
                T2[i][j] = -2
    return T2


# ---------------------------------------------------------------------------
# Heuristic
# ---------------------------------------------------------------------------

def _build_path(
    T: List[List[int]], pred: List[Set[int]], start: int, ends: Set[int], n: int,
    strategy: str = "nearest"
) -> Tuple[int, List[int]]:
    path = [start]
    used = {start}
    cost = 0
    last = start
    for depth in range(1, n):
        cands = []
        for j in range(n):
            if j in used or not pred[j].issubset(used):
                continue
            e = T[last][j]
            if e < 0:
                continue
            if depth == n - 1 and j not in ends:
                continue
            cands.append((e, j))
        if not cands:
            return -1, []
        if strategy == "nearest":
            cands.sort()
        elif strategy == "farthest":
            cands.sort(reverse=True)
        else:
            cands.sort(key=lambda x: (x[0] * 31 + x[1]) % 97)
        if depth == n - 1:
            end_cands = [(e, j) for e, j in cands if j in ends]
            if end_cands:
                cands = sorted(end_cands)
        e, nxt = cands[0]
        path.append(nxt)
        used.add(nxt)
        cost += e
        last = nxt
    return cost, path


def path_cost(path: List[int], T: List[List[int]]) -> int:
    return sum(T[a][b] for a, b in zip(path, path[1:]))


def is_feasible(path: List[int], pred: List[Set[int]]) -> bool:
    pos = {v: i for i, v in enumerate(path)}
    for i, ps in enumerate(pred):
        for j in ps:
            if pos[j] >= pos[i]:
                return False
    return True


def two_opt(path: List[int], T: List[List[int]], pred: List[Set[int]], max_iter: int = 600) -> Tuple[int, List[int]]:
    n = len(path)
    best = path[:]
    best_c = path_cost(best, T)
    improved = True
    it = 0
    while improved and it < max_iter:
        improved = False
        it += 1
        for i in range(1, n - 1):
            for j in range(i + 1, n):
                newp = best[:i] + best[i:j][::-1] + best[j:]
                if is_feasible(newp, pred):
                    c = path_cost(newp, T)
                    if c < best_c:
                        best, best_c = newp, c
                        improved = True
                        break
            if improved:
                break
    return best_c, best


def or_opt(path: List[int], T: List[List[int]], pred: List[Set[int]], max_iter: int = 400) -> Tuple[int, List[int]]:
    n = len(path)
    best = path[:]
    best_c = path_cost(best, T)
    improved = True
    it = 0
    while improved and it < max_iter:
        improved = False
        it += 1
        for length in (1, 2, 3):
            for i in range(0, n - length + 1):
                segment = best[i:i + length]
                rest = best[:i] + best[i + length:]
                for j in range(len(rest) + 1):
                    if j == i:
                        continue
                    newp = rest[:j] + segment + rest[j:]
                    if is_feasible(newp, pred):
                        c = path_cost(newp, T)
                        if c < best_c:
                            best, best_c = newp, c
                            improved = True
                            break
                if improved:
                    break
            if improved:
                break
    return best_c, best


def _random_feasible_path(
    T: List[List[int]], pred: List[Set[int]], start: int, ends: Set[int], n: int, rng
) -> Tuple[int, List[int]]:
    """Build a random feasible path by shuffling candidate order at each step."""
    path = [start]
    used = {start}
    cost = 0
    last = start
    for depth in range(1, n):
        cands = []
        for j in range(n):
            if j in used or not pred[j].issubset(used):
                continue
            e = T[last][j]
            if e < 0:
                continue
            if depth == n - 1 and j not in ends:
                continue
            cands.append((e, j))
        if not cands:
            return -1, []
        rng.shuffle(cands)
        # still prefer cheaper ones with some probability
        cands.sort(key=lambda x: (x[0] + rng.random() * 30, x[1]))
        if depth == n - 1:
            end_cands = [(e, j) for e, j in cands if j in ends]
            if end_cands:
                cands = end_cands
        e, nxt = cands[0]
        path.append(nxt)
        used.add(nxt)
        cost += e
        last = nxt
    return cost, path


def _perturb(path: List[int], pred: List[Set[int]], rng, strength: int = 3) -> List[int]:
    """Random feasible perturbation: repeatedly try to relocate a small segment."""
    n = len(path)
    cur = path[:]
    for _ in range(strength):
        length = rng.randint(1, min(4, n - 2))
        i = rng.randint(0, n - length)
        segment = cur[i:i + length]
        rest = cur[:i] + cur[i + length:]
        positions = list(range(len(rest) + 1))
        rng.shuffle(positions)
        for j in positions:
            newp = rest[:j] + segment + rest[j:]
            if is_feasible(newp, pred):
                cur = newp
                break
    return cur


def strong_heuristic(
    T: List[List[int]], pred: List[Set[int]], starts: List[int], ends: Set[int], n: int
) -> Tuple[int, List[int]]:
    """
    Iterated local search with:
      - deterministic constructions (nearest / farthest / hash)
      - randomized feasible constructions
      - 2-opt + Or-opt
      - random feasible perturbations + re-optimisation
    """
    import random
    rng = random.Random(42)

    best_cost = float("inf")
    best_path: List[int] = []

    def improve(p):
        c = path_cost(p, T)
        for _ in range(2):
            c, p = two_opt(p, T, pred, max_iter=300)
            c, p = or_opt(p, T, pred, max_iter=200)
        c, p = two_opt(p, T, pred, max_iter=150)
        return c, p

    # 1. Deterministic multi-start
    for s in starts:
        for strat in ("nearest", "farthest", "hash"):
            c, p = _build_path(T, pred, s, ends, n, strategy=strat)
            if not p:
                continue
            c, p = improve(p)
            if c < best_cost:
                best_cost, best_path = c, p

    # 2. Randomized constructions
    for s in starts:
        for _ in range(12):
            c, p = _random_feasible_path(T, pred, s, ends, n, rng)
            if not p:
                continue
            c, p = improve(p)
            if c < best_cost:
                best_cost, best_path = c, p

    # 3. Iterated local search from the best
    if best_path:
        cur = best_path[:]
        for _ in range(25):
            pert = _perturb(cur, pred, rng, strength=rng.randint(2, 5))
            c, p = improve(pert)
            if c < best_cost:
                best_cost, best_path = c, p
                cur = p[:]
            elif c < path_cost(cur, T) * 1.05:  # accept mild worsening for diversification
                cur = p[:]

    return (int(best_cost), best_path) if best_path else (-1, [])


# ---------------------------------------------------------------------------
# Single-commodity flow MIP + CBC
# ---------------------------------------------------------------------------

def _parse_cbc_log(log_text: str) -> Tuple[int, Optional[float]]:
    nodes = 0
    m = re.search(r"Enumerated nodes:\s*(\d+)", log_text)
    if m:
        nodes = int(m.group(1))
    gap = None
    m = re.search(r"Gap:\s*([0-9.]+)\s*%", log_text)
    if m:
        gap = float(m.group(1)) / 100.0
    else:
        m = re.search(r"gap\s*=\s*([0-9.eE+-]+)", log_text, re.I)
        if m:
            try:
                gap = float(m.group(1))
            except ValueError:
                pass
    return nodes, gap


def solve_mip(
    T: List[List[int]],
    pred: List[Set[int]],
    succ: List[Set[int]],
    starts: List[int],
    ends: List[int],
    time_limit: float,
    warm_path: Optional[List[int]] = None,
) -> Tuple[int, List[int], bool, float, int, Optional[float]]:
    """
    Single-commodity unit-flow formulation + position variables for precedences.
    A flow of value 1 travels from the chosen start to the chosen end.
    This is tighter than pure MTZ while remaining correct.
    Returns (cost, path, is_optimal, elapsed, enumerated_nodes, gap).
    """
    n = len(T)
    start_time = time.perf_counter()

    Tf = filter_arcs(T, pred, n)
    forced = detect_forced_arcs(T, pred, succ, n)

    prob = pulp.LpProblem("SOP_SCF", pulp.LpMinimize)

    # Arc variables
    x = {}
    for i in range(n):
        for j in range(n):
            if i != j and Tf[i][j] >= 0:
                x[i, j] = pulp.LpVariable(f"x_{i}_{j}", cat="Binary")

    for (i, j) in forced:
        if (i, j) in x:
            x[i, j].fixBound = 1
            x[i, j].upBound = 1

    # Unit-flow variables (flow of value 1 from start to end)
    f = {}
    for i in range(n):
        for j in range(n):
            if (i, j) in x:
                f[i, j] = pulp.LpVariable(f"f_{i}_{j}", lowBound=0, upBound=1, cat="Continuous")

    # Position variables (for precedences)
    u = [pulp.LpVariable(f"u_{i}", lowBound=0, upBound=n - 1, cat="Continuous") for i in range(n)]

    # Start / end indicators
    is_start = {s: pulp.LpVariable(f"start_{s}", cat="Binary") for s in starts}
    is_end   = {e: pulp.LpVariable(f"end_{e}",   cat="Binary") for e in ends}
    prob += pulp.lpSum(is_start[s] for s in starts) == 1
    prob += pulp.lpSum(is_end[e]   for e in ends)   == 1

    # Objective
    prob += pulp.lpSum(Tf[i][j] * x[i, j] for (i, j) in x)

    # Exactly n-1 arcs
    prob += pulp.lpSum(x[i, j] for (i, j) in x) == n - 1

    # Degree constraints
    for i in range(n):
        outs = [x[i, j] for j in range(n) if (i, j) in x]
        ins  = [x[j, i] for j in range(n) if (j, i) in x]
        if i in starts and i in ends:
            prob += pulp.lpSum(outs) == 1 - is_end[i]
            prob += pulp.lpSum(ins)  == 1 - is_start[i]
        elif i in starts:
            prob += pulp.lpSum(outs) == 1
            prob += pulp.lpSum(ins)  == 1 - is_start[i]
        elif i in ends:
            prob += pulp.lpSum(outs) == 1 - is_end[i]
            prob += pulp.lpSum(ins)  == 1
        else:
            prob += pulp.lpSum(outs) == 1
            prob += pulp.lpSum(ins)  == 1

    # ----- Unit-flow constraints (value 1 from start to end) -----
    for (i, j) in x:
        prob += f[i, j] <= x[i, j]

    for i in range(n):
        inflow  = pulp.lpSum(f[j, i] for j in range(n) if (j, i) in f)
        outflow = pulp.lpSum(f[i, j] for j in range(n) if (i, j) in f)
        # net outflow = 1 if start, -1 if end, 0 otherwise
        if i in starts and i in ends:
            prob += outflow - inflow == is_start[i] - is_end[i]
        elif i in starts:
            # if chosen start: net +1, else 0 (but non-chosen starts still have degree constraints)
            # actually non-chosen starts behave as ordinary nodes (net 0)
            prob += outflow - inflow == is_start[i]
        elif i in ends:
            prob += outflow - inflow == -is_end[i]
        else:
            prob += outflow - inflow == 0

    # Position / MTZ linking
    for i in range(n):
        for j in range(n):
            if (i, j) in x:
                prob += u[i] + 1 <= u[j] + (n - 1) * (1 - x[i, j])

    # Precedences on positions
    for i in range(n):
        for j in pred[i]:
            prob += u[j] + 1 <= u[i]

    # Force start position = 0, end position = n-1
    for s in starts:
        prob += u[s] <= (n - 1) * (1 - is_start[s])
    for e in ends:
        prob += u[e] >= (n - 1) * is_end[e]

    # Warm start
    if warm_path and len(warm_path) == n:
        try:
            for i in range(n):
                for j in range(n):
                    if (i, j) in x:
                        x[i, j].setInitialValue(
                            1.0 if warm_path.index(j) == warm_path.index(i) + 1 else 0.0
                        )
            for idx, node in enumerate(warm_path):
                u[node].setInitialValue(float(idx))
                if idx < n - 1:
                    a, b = warm_path[idx], warm_path[idx + 1]
                    if (a, b) in f:
                        f[a, b].setInitialValue(1.0)
            if warm_path[0] in is_start:
                is_start[warm_path[0]].setInitialValue(1.0)
            if warm_path[-1] in is_end:
                is_end[warm_path[-1]].setInitialValue(1.0)
        except Exception:
            pass

    log_fd, log_path = tempfile.mkstemp(suffix=".cbc.log")
    os.close(log_fd)

    try:
        nthreads = min(4, os.cpu_count() or 1)
        solver = pulp.PULP_CBC_CMD(
            msg=False,
            timeLimit=max(1, int(time_limit)),
            warmStart=True,
            logPath=log_path,
            options=["sec", str(max(1, int(time_limit)))],
            threads=nthreads,
            cuts=True,
            strong=5,
        )
        status = prob.solve(solver)
        elapsed = time.perf_counter() - start_time

        log_text = ""
        try:
            log_text = Path(log_path).read_text(errors="replace")
        except Exception:
            pass
        enumerated, gap = _parse_cbc_log(log_text)

        is_optimal = pulp.LpStatus[status] == "Optimal"
        cost = -1
        path: List[int] = []

        try:
            succ_map: Dict[int, int] = {}
            for (i, j), var in x.items():
                val = pulp.value(var)
                if val is not None and val > 0.5:
                    succ_map[i] = j

            start_node = None
            if succ_map:
                incoming = set(succ_map.values())
                for s in starts:
                    if s in succ_map and s not in incoming:
                        start_node = s
                        break
            if start_node is None:
                us = [(pulp.value(u[i]), i) for i in range(n)]
                us = [(v, i) for v, i in us if v is not None]
                if us:
                    start_node = min(us)[1]

            if start_node is not None and succ_map:
                path = [start_node]
                cur = start_node
                seen = {cur}
                while cur in succ_map and len(path) < n:
                    cur = succ_map[cur]
                    if cur in seen:
                        break
                    path.append(cur)
                    seen.add(cur)
                if len(path) == n:
                    cost = int(round(path_cost(path, T)))
            elif start_node is not None:
                us = sorted((pulp.value(u[i]), i) for i in range(n) if pulp.value(u[i]) is not None)
                path = [i for _, i in us]
                if len(path) == n:
                    cost = int(round(path_cost(path, T)))
        except Exception:
            pass

        return cost, path, is_optimal, elapsed, enumerated, gap
    finally:
        try:
            os.unlink(log_path)
        except Exception:
            pass


# ---------------------------------------------------------------------------
# Validity
# ---------------------------------------------------------------------------

def validate(path: List[int], T: List[List[int]], pred: List[Set[int]]) -> Tuple[bool, int]:
    n = len(T)
    if len(path) != n or len(set(path)) != n:
        return False, -1
    pos = {node: idx for idx, node in enumerate(path)}
    for i in range(n):
        for j in pred[i]:
            if pos[j] >= pos[i]:
                return False, -1
    cost = 0
    for a, b in zip(path, path[1:]):
        if T[a][b] < 0:
            return False, -1
        cost += T[a][b]
    return True, cost


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    ap = argparse.ArgumentParser(description="Strongest SOP solver (SCF MIP + CBC + heuristic)")
    ap.add_argument("files", nargs="+", type=Path)
    ap.add_argument("--time", type=float, default=60.0)
    ap.add_argument("--out", type=Path, default=None)
    args = ap.parse_args()

    rows = []
    for path in args.files:
        try:
            name, n, T = parse_sop(path)
            pred, succ = build_precedence(T)
            starts, ends = find_start_end(pred, succ, n)
            if not starts:
                raise ValueError("No start node")
            if not ends:
                raise ValueError("No end node")

            t0 = time.perf_counter()
            h_cost, h_path = strong_heuristic(T, pred, starts, set(ends), n)

            remaining = max(1.0, args.time - (time.perf_counter() - t0))
            m_cost, m_path, optimal, m_elapsed, enumerated, gap = solve_mip(
                T, pred, succ, starts, ends, remaining, warm_path=h_path
            )

            best_cost = -1
            best_path: List[int] = []
            if m_path and len(m_path) == n:
                best_cost, best_path = m_cost, m_path
            if h_path and (best_cost < 0 or h_cost < best_cost):
                best_cost, best_path = h_cost, h_path
                if not (m_path and m_cost == h_cost and optimal):
                    optimal = False

            if optimal and m_path and m_cost != best_cost:
                optimal = False

            elapsed = time.perf_counter() - t0

            ok, recomputed = validate(best_path, T, pred)
            if not ok or recomputed != best_cost:
                print(
                    f"WARNING: {path.name} failed validation "
                    f"(ok={ok}, recomputed={recomputed}, claimed={best_cost})",
                    file=sys.stderr,
                )
                best_cost = -1
                best_path = []
                optimal = False

            status = "optimal" if optimal else "best-found"
            gap_str = f"  gap={gap:.4f}" if gap is not None and not optimal else ""
            path_str = " ".join(map(str, best_path)) if best_path else ""
            rows.append({
                "Instance": path.name,
                "Instance Size": n,
                "Time": f"{elapsed:.4f}",
                "Enumerated Nodes": enumerated,
                "Best Cost": best_cost,
                "Best Path": path_str,
            })
            print(
                f"{path.name}: N={n}  cost={best_cost}  nodes={enumerated}  "
                f"time={elapsed:.3f}s  [{status}]{gap_str}",
                file=sys.stderr,
            )
        except Exception as exc:
            print(f"ERROR {path}: {exc}", file=sys.stderr)
            rows.append({
                "Instance": path.name,
                "Instance Size": -1,
                "Time": "0.0000",
                "Enumerated Nodes": 0,
                "Best Cost": -1,
                "Best Path": "",
            })

    fields = ["Instance", "Instance Size", "Time", "Enumerated Nodes", "Best Cost", "Best Path"]
    out = open(args.out, "w", newline="") if args.out else sys.stdout
    try:
        w = csv.DictWriter(out, fieldnames=fields)
        w.writeheader()
        w.writerows(rows)
    finally:
        if args.out:
            out.close()


if __name__ == "__main__":
    main()
