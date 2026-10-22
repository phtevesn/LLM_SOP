# SOP exact solver (parallel branch & bound with assignment-problem bound)

Solves the Sequential Ordering Problem (TSPLIB `.sop`, FULL_MATRIX) and reports whether the
result is proven optimal. Single C++17 file, no external libraries.

## 1. Requirements and installation

* Ubuntu (tested 24.04), `g++` >= 9 (C++17) and `make` (`sudo apt install build-essential`)
* Linux only (uses `pthread_setaffinity_np`, `mmap`, and `/sys` for NUMA topology; no libnuma needed)
* RAM: transposition table defaults to 25% of available memory (max 16 GB); override with `--mem-mb`.
  Per-thread search state is about 24 MB at N = 1000.

## 2. Build and run

```bash
make                      # or: g++ -O3 -march=native -std=c++17 -pthread -o sop_solver sop_solver.cpp
./sop_solver prob_42.sop --threads 32 --time-limit 3600 --output results.csv
```
Syntax: `sop_solver FILE.sop [--time-limit SECONDS] [--threads N] [--output out.csv] [--no-header]`
Extra options: `--mem-mb MB` (total transposition memory), `--no-pin` (disable CPU pinning).
Defaults: no time limit, 1 thread, stdout only, header included.
Note: `-march=native` ties the binary to the build machine; build on the Threadripper itself.

Example stdout:
```
Instance,Instance Size,Time,Enumerated Nodes,Best Cost,Best Path,Optimal
prob_42.sop,42,56.176,24736106,243,0 11 4 13 ... 41,yes
```

## 3. Input / output

**Input**: header lines parsed by keyword (`DIMENSION`, `EDGE_WEIGHT_FORMAT`, ...; others ignored), then
`EDGE_WEIGHT_SECTION`, the repeated N, then N x N integers that may wrap across lines. A trailing `EOF`
line is optional. `T[i][j] >= 0` is a cost; `T[i][j] < 0` means i depends on j (j first). Start/end nodes
are derived (start = no predecessors, end = no successors); if several exist, all are considered.
Arc costs must be < 1e9.

**Output** (one CSV row per run; header unless `--no-header`): `Instance,Instance Size,Time,Enumerated Nodes,Best Cost,Best Path,Optimal`.
Path is 0-based, space separated. `--output` writes the row to the file (appending; header only if the file
is new/empty, unless `--no-header`) *and* to stdout. All logs go to stderr. On timeout the best path found is
reported with `Optimal=no`; if none was found: `Best Cost=-1`, empty path, `no`. The same applies to
infeasible (cyclic) instances. The final path is re-validated (permutation, all precedences, cost recomputed
from scratch). `Time` is wall-clock from program start to output, including parsing, heuristic and proof.

**Enumerated Nodes** = number of branch & bound search nodes expanded (calls of the DFS routine), summed over all
threads. Heuristic local-search moves are not counted.

## 4. Algorithm and reasoning

1. **Preprocessing** (bitsets): transitive closure and reduction of the precedence graph, cycle detection,
   static arc elimination (arc u->v removed if v must precede u, or some k must lie between them).
   A virtual start row (cost 0 to every source) and virtual end column (cost 0 from every sink) let the
   solver choose the best start/end without hardcoding.
2. **Heuristic upper bound**: iterated local search (precedence-feasible Or-opt with segment lengths up to 35,
   don't-look queue, random segment kicks) run on every thread, seeded by a greedy construction on AP
   reduced costs. It is short (<= ~5 s); the B&B dives keep improving the incumbent.
3. **Exact search**: parallel depth-first branch & bound over path prefixes.
   * *Lower bound*: assignment-problem relaxation (rows = last node + unvisited, columns = unvisited + end).
     The dual solution is warm-started from the parent: removing a row and column keeps duals feasible, so a
     child bound is free (`parent bound + reduced cost`), and only one O(m^2) shortest-augmenting-path repair
     is needed when the node is actually entered, with early exit once the bound exceeds the incumbent.
     This also detects infeasible nodes. Children are explored in order of this bound.
   * *Dominance*: a lock-free transposition table keyed on (visited set, last node) stores the best prefix cost;
     a prefix reaching the same state at equal or higher cost is pruned. Entries use a 64-bit Zobrist key
     verified by the XOR scheme (torn concurrent writes are rejected). A false match needs a 64-bit hash
     collision (negligible, but the proof is technically probabilistic at that level).
   * *Parallelism*: shared atomic incumbent; idle threads trigger work donation, where a busy thread hands
     the unexplored siblings of its *shallowest* open level to a shared queue (large subtrees first).
   * *NUMA* (Threadripper 2990WX, where only two of four dies have memory): threads are pinned to physical
     cores first, filling memory-attached nodes first; each memory-bearing node gets its own transposition
     table, first-touched by its own threads, and memory-less nodes use the nearest one. Per-thread state is
     allocated inside the pinned thread. Tables are not shared across groups, trading some pruning for
     fewer remote accesses.
4. **Optimality** is reported only if the search completed without hitting the time limit (or the incumbent
   equals the root bound).

## Known limitations

Exact SOP is NP-hard; instances with weak assignment bounds (heavy precedence structure such as rbg048a, or
N=200 with sparse precedence such as R.200.1000.1) may not be proven in a given time. Tested in a 1-core sandbox
only (correctness against brute force on ~160 random instances, 1 and 3-4 threads; prob_42 proven optimal at 243);
32-core scaling and NUMA behavior are untested.
