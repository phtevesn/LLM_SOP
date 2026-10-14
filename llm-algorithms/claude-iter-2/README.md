# SOP Solver

A single-file C++17 program that solves Sequential Ordering Problem (SOP) instances from
TSPLIB-style `.sop` files: find the cheapest Hamiltonian path that respects all precedence
constraints. It runs a parallel portfolio of a strong metaheuristic and two exact methods,
and reports either a proof of optimality or a certified lower bound and gap.

Files:

| File | Purpose |
|---|---|
| `sop_solver.cpp` | The complete program (parser, heuristic, exact solvers, validation, CSV output) |
| `examples/prob42.sop` | The N = 42 instance from the task description |
| `examples/small30.sop` | A random N = 30 instance, solved to proven optimality in well under a second |
| `examples/gen_random_sop.py` | Optional generator: `python3 gen_random_sop.py N DENSITY SEED > x.sop` |

## Overview of the algorithm

Everything runs concurrently and shares one incumbent (best path so far) and one stop flag.

1. **Heuristic threads (iterated local search).**
   * *Neighbourhood:* remove a block of consecutive nodes of any length (up to 12 to 40,
     depending on N) and re-insert it, order preserved, in another gap of the path. This is
     the reversal-free 3-exchange that SOP-specific heuristics are built around (the
     SOP-3-exchange of HAS-SOP). Moves are scored in O(1) and precedence-checked by inspecting
     only the nodes the block jumps over (direct constraints suffice).
   * *Work queue:* after a perturbation or a move, only the nodes at changed edges are
     re-examined, so one ILS round costs O(touched x block length x N) instead of a full
     pass. This is what keeps the search fast for large N.
   * *Metaheuristic:* random feasible segment moves as perturbation, simulated-annealing
     acceptance (temperature tuned on `prob42`), return to the best path after a stale
     period, occasional fresh randomized restarts, and cooperation: a thread adopts the
     shared best when it is better than its own.
2. **Exact thread, preprocessing.** Arc u->v is removed if T[u][v] < 0, if v must precede u,
   or if some node must lie strictly between u and v in the precedence order. Precedence
   closure and these tests use bitsets.
3. **Exact thread, root LP (own dense simplex).** The directed Dantzig-Fulkerson-Johnson
   relaxation, written as a cycle through a dummy node (start and end are derived, not
   hardcoded), strengthened by cuts found with max-flow:
   * subtour-elimination cuts;
   * precedence cuts: if k must precede l, the real arcs leaving any node set containing k
     but not l must carry at least one unit.

   Primal simplex solves the degree system, dual simplex re-optimizes after each cut batch.
   The result is a strong lower bound (if it reaches the incumbent, optimality is proven),
   plus reduced costs. Arcs whose reduced cost pushes every path through them to at least the
   incumbent are removed (**reduced-cost fixing**). Dual feasibility keeps both the bound and
   the fixing valid even if the LP is stopped early.
4. **Exact portfolio, two searches in two threads on the reduced problem:**
   * *Tree search with LP-guided bounds:* depth-first search over partial paths. Each node is
     bounded by an assignment problem (Hungarian algorithm) on the LP reduced costs, added to
     the LP value and the prefix reduced cost (valid for every path), and by the plain
     assignment bound. Children are ordered by reduced cost. A transposition table on
     (visited set, last node), with 128-bit Zobrist keys, discards dominated states. This is
     extremely fast when the precedences are tight.
   * *LP-based branch and cut:* branching on arcs (x = 1 forces the arc, x = 0 forbids it),
     re-solving the node LP from the parent basis, separating cuts at every node and fixing
     arcs by reduced cost. Its bound is stronger per node but each node is far slower.

   The tree search is itself parallel: the top of the tree is cut into short path prefixes
   (tasks) that worker threads take from a shared counter, each worker with a private copy of
   the search state and its own share of the transposition-table memory.

   Whichever search finishes first proves optimality and stops all threads. The upper bound used by
   both searches is read live from the shared incumbent, so the heuristic threads
   continuously tighten the exact search.

If the time limit is reached first, the result is reported as "best found" together with a
certified lower bound and gap.

## Requirements and installation

* A C++17 compiler with thread support (tested with g++ 13). No packages.
* Memory: about 100 MB for the transposition table by default (`--tt-bits`), plus the LP
  tableau (skipped automatically when it would exceed about 200 MB; this happens for
  N of a few hundred and up).

## Build and run

```bash
g++ -O2 -std=c++17 -pthread -o sop_solver sop_solver.cpp

# one instance, 60 s limit (the default), CSV to stdout
./sop_solver examples/prob42.sop

# several instances, 30 s limit each, CSV written to a file
./sop_solver -t 30 -o results.csv examples/prob42.sop examples/small30.sop
```

Options:

| Option | Meaning |
|---|---|
| `-t, --time-limit SEC` | Wall-clock limit per instance (default 60; `0` = unlimited, runs until optimality is proven) |
| `-o, --output FILE` | Write CSV to FILE instead of stdout |
| `-j, --threads N` | Heuristic worker threads (default: cores/4 with 4 or more cores, otherwise cores - 1, at least 1) |
| `-w, --dfs-threads N` | Tree-search worker threads (default: the remaining cores with 4 or more cores, otherwise 1). The LP branch-and-cut always has one thread of its own |
| `-p, --proof-share F` | Fraction (0 to 1) of the time limit the exact threads may use (default 1.0 with 4 or more cores, otherwise 0.5, or 0.15 when N > 100) |
| `-b, --tt-bits B` | Transposition table with 2^B entries of 24 bytes (default 22, range 10 to 28) |
| `-s, --seed N` | Random seed (default 12345) |
| `--no-lp` | Skip the LP (assignment bounds only) |
| `--no-bc` | Skip the LP branch-and-cut thread |
| `--no-exact` | Heuristic only (needs a finite time limit) |
| `--no-heuristic` | Exact threads only, after one quick heuristic run |

A status line per instance goes to **stderr**: whether the result is `PROVEN OPTIMAL` or
`BEST FOUND`, with the certified lower bound and gap in the latter case, and LP statistics:

```
[small30.sop] N=30 cost=643  PROVEN OPTIMAL  [LP bound 571, 131 cuts, 601 arcs removed]  time=0.316s  nodes=383
[prob42.sop] N=42 cost=243  BEST FOUND (not proven optimal; lower bound 229, gap <= 5.8%)  [LP bound 229, 120 cuts, 1366 arcs removed]  time=20.000s  nodes=153276
```

## Input format

TSPLIB-style. Header lines are parsed by keyword (`NAME`, `DIMENSION`, `EDGE_WEIGHT_FORMAT`,
...); `TYPE`, `COMMENT` and `EDGE_WEIGHT_TYPE` are metadata. After `EDGE_WEIGHT_SECTION` come
`N` again and then `N x N` integers that may wrap across lines arbitrarily. Reading stops after
exactly `N x N` values, so a trailing `EOF` line, blank lines, CRLF line endings or no
terminator at all are all fine.

Matrix semantics (`T` is the matrix, nodes are `0..N-1`):
* `T[i][j] >= 0`: cost of going directly from `i` to `j`.
* `T[i][j] < 0`: `i` depends on `j`, so `j` must come before `i`.
* The diagonal is ignored; large values such as `10000000` are ordinary costs.
* A cyclic precedence relation is reported as an error (no row is written).

## Output format

CSV with a header row and one row per successfully processed instance:

`Instance,Instance Size,Time,Enumerated Nodes,Best Cost,Best Path`

| Column | Meaning |
|---|---|
| Instance | Input file name (without directory) |
| Instance Size | N |
| Time | Total wall-clock seconds spent on the instance. Includes the proof time when optimality was proven |
| Enumerated Nodes | Search nodes entered by the exact solvers: nodes of the tree search plus LP nodes of the branch and cut. Heuristic work and cutting-plane rounds are not counted |
| Best Cost | Cost of the reported path, recomputed from scratch |
| Best Path | Node indices in order, space-separated, 0-based |

Before any row is written the path is validated: it must be a permutation of `0..N-1`,
satisfy every precedence entry, and its cost is recomputed from the matrix. Instances that
fail parsing or validation are reported on stderr, produce no CSV row, and make the exit code 1.

### Sample output rows

```
small30.sop,30,0.316,383,643,0 12 6 21 15 11 27 2 9 25 20 4 28 13 10 3 16 17 22 1 7 18 24 26 5 19 23 14 8 29
prob42.sop,42,20.000,153276,243,0 11 4 13 7 27 34 6 22 19 33 16 25 24 39 1 36 32 18 38 5 2 8 23 3 35 15 9 14 10 20 12 30 31 26 17 28 21 40 29 37 41
```

## Measured behaviour

All numbers below are from a **single-core** sandbox, so the heuristic and exact threads were
time-sliced. The parallel tree search is verified for correctness (same optimum as the
single-threaded search on every test) but its speedup on real multi-core hardware is
**not measured**; on one core it costs a little extra work.

* **Robustness:** the program runs clean under AddressSanitizer, UndefinedBehaviorSanitizer
  and ThreadSanitizer (including the threaded portfolio and the LP code). Edge cases behave
  correctly: N = 1, N = 2, instances with no precedence at all (free start and end), fully
  chained instances, a cyclic precedence relation (rejected with an error), a truncated matrix
  (rejected), and a missing repeated dimension line.
* **Correctness:** about 130 random instances (N = 8 to 15, precedence densities 0 to 0.7)
  matched an independent brute-force DP, and all were proven optimal. The LP bound was never
  above the optimum, and on `prob42` it was cross-checked against HiGHS (228.2 from both).
* **`prob42` (N = 42):** cost 243 reached with 3 of 3 seeds within 20 s. 243 matches the best
  known value as far as I recall, but I could not check that offline. The earlier
  or-opt-only version got 246 to 247.
* **Proofs:** random instances with moderately dense precedences were proven optimal in
  seconds up to N = 60 (for example N = 35 in 5.5 s, N = 45 in 3.9 s, N = 60 in 1.1 s).
* **Large instances:** the work-queue local search beat the previous version on N = 120
  (358 vs 368) and N = 300 (713 vs 783) in 30 s. N = 600 and N = 1000 run and respect the
  time limit.

## Known limitations

* **`prob42` is not proven optimal.** The strengthened LP bound is 229 (the LP optimum is
  228.2) against a cost of 243, a certified gap of at most 5.8%. About 2 CPU-minutes of
  branch and cut and 5.6 million tree-search nodes did not close it. State-of-the-art branch
  and cut for the SOP uses additional precedence-lifted cut families that are not implemented.
* **Some mid-size instances stay open.** For example a random N = 40 instance with precedence
  density 0.1 is not proven within 40 s (LP bound 277 against cost 399). There the certified
  gap is large and the cost is a heuristic result.
* The heuristic is stochastic: costs can differ between seeds and limits, and runs with several
  threads are not exactly reproducible.
* For N in the hundreds the LP is skipped for memory reasons and the Hungarian bound costs
  O(N^3) per node, so the tree search is slow; the local search does the work, and the exact
  share of the time is reduced automatically. Heuristic block length is capped at 12 to 40.
* The LP uses double-precision arithmetic with tolerances; bounds are rounded conservatively,
  and any numerical or capacity trouble in the branch and cut prevents it from claiming
  optimality. The 128-bit transposition-table keys make a wrong prune astronomically
  unlikely, not mathematically impossible.
* Time limits are checked at every tree node and every few local-search steps; a very large
  single Hungarian solve or the initial heuristic on huge instances can overshoot a short
  limit slightly.
