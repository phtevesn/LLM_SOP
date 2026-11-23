# SOP exact solver (C++17, parallel)

Solves the Sequential Ordering Problem (SOP) from TSPLIB-style `.sop` files:
find a minimum-cost Hamiltonian path that respects precedence constraints, and
report whether optimality was **proven** before the time limit.

Status in one paragraph: the program is correct on everything I tested (see
`DEVELOPMENT.md`), proves optimality on `prob_42` (243) and `R_200_1000_1`
(1404) on a single core, and on `rbg048a` finds 351 but did **not** prove it in
290 s on one core. For large or tightly constrained instances the proof can be
out of reach; the program then reports the best solution found and `no`.
Multi-core speed-up and NUMA behaviour on the Threadripper were **not** measured
(the development sandbox had one core).

## 1. Requirements and installation

* Ubuntu (developed against the Linux APIs only: `pthread`, `mmap`, `mbind`
  through `syscall`, `/sys/devices/system/node`). No third-party libraries,
  no libnuma needed.
* `g++` with C++17 (target: g++ 9.4 on Ubuntu 20.04). The code was compiled
  and tested with g++ 13.3 only; it avoids C++20 features, but a g++ 9.4 build
  has **not** been tried.
* `make`. Optional: `python3` for the test scripts in `tests/`.
* `cmake` is optional: a `CMakeLists.txt` is included but was **not** tested
  (cmake was not available where I worked).

```bash
sudo apt-get install -y build-essential python3     # if not already installed
```

## 2. Build and run

```bash
make                      # builds ./sop_solver  (-O3 -march=native -pthread)
make clean                # remove build products
bash tests/run_tests.sh 60 4   # optional: 60 random instances vs brute force, 4 threads
```

`-march=native` tunes for the machine that compiles; build on the target
machine (Zen+ for the 2990WX).

Command line:

```
./sop_solver FILE.sop [--time-limit SECONDS] [--threads N] [--output out.csv] [--no-header]
```

Defaults: no time limit, 1 thread, CSV to stdout with header. Extra optional
flags (not required by the task):

| flag | meaning |
|---|---|
| `--tt-mb MB` | size of the shared transposition table (default: 1/4 of available RAM, at most 16 GiB, rounded down to a power of two; memory is mapped lazily) |
| `--seed S` | random seed (ILS and hashing) |
| `--warmup SEC` | force the heuristic warm-up length (default: automatic) |
| `--heuristic-only` | skip the exact search (never reports `yes`) |

Example (single thread; the path field is shortened here):

```
$ ./sop_solver R_200_1000_1.sop 2>log.txt
Instance,Instance Size,Time,Enumerated Nodes,Best Cost,Best Path,Optimal
R_200_1000_1.sop,200,1.949695,1138263,1404,0 31 104 113 47 135 59 39 1 ... ,yes
```

For the 32-core machine use `--threads 32` (threads are pinned in a
NUMA-aware order, see section 4). `--output results.csv` appends one row (and
the header only if the file is new or empty, unless `--no-header`); stdout
always gets the row too. All logs, including every improvement of the best
known cost (`new best cost ...`), go to stderr.

## 3. Input and output format

**Input** (`.sop`, TSPLIB style): header lines `NAME`, `TYPE`, `COMMENT`,
`DIMENSION`, `EDGE_WEIGHT_TYPE`, `EDGE_WEIGHT_FORMAT` are parsed by keyword;
`EDGE_WEIGHT_SECTION` is followed by `N` again and then `N x N` integers that
may wrap across lines; a trailing `EOF` line is optional. Entry `T[i][j] >= 0`
is the cost of arc `i -> j`; `T[i][j] < 0` means node `i` depends on node `j`
(`j` must come first); the diagonal is ignored. Start and end nodes are
derived from the precedence relation (sources and sinks of its transitive
closure); if several exist, zero-cost dummy start/end nodes are added
internally and removed from the output path.

**Output** (CSV, one row per run, header unless `--no-header`):

`Instance,Instance Size,Time,Enumerated Nodes,Best Cost,Best Path,Optimal`

* `Instance`: input file name (base name); `Instance Size`: N.
* `Time`: wall-clock seconds from program start (parsing included) to the end
  of the run.
* `Enumerated Nodes`: search-tree nodes of the exact branch and bound that
  were *expanded* (a node = a prefix path that passed the lower-bound test and
  the dominance test, plus complete paths reached), summed over all threads and
  over all budget iterations. Heuristic (ILS) work and the assignment solve are
  not counted. Instances solved by the heuristic/bounds alone report 0.
* `Best Cost`: recomputed from the raw matrix by an independent validity check
  (permutation, all precedence constraints, cost from scratch). If no feasible
  path was found (or the precedence relation is cyclic): `-1`, empty path, `no`.
* `Best Path`: 0-based node indices separated by spaces.
* `Optimal`: `yes` only if the search space was exhausted before any time
  limit expired.

## 4. Algorithmic choices (short version)

Full inventory with sources: `ALGORITHMS.md`. Development history and what was
actually measured: `DEVELOPMENT.md`.

* **Preprocessing.** Transitive closure (bitsets), derivation of start/end,
  removal of arcs that cannot occur in any feasible path (arc `i -> j` is
  impossible if `j` must precede `i` or some node must lie strictly between
  them), cover relation for O(1) availability tests.
* **Root lower bound.** Assignment-problem relaxation (Hungarian algorithm).
  Its optimal dual potentials give non-negative *reduced costs*; every
  start-to-end path costs `LB0 + sum of reduced costs`, so the whole search is
  carried out in "reduced-cost space".
* **Upper bound.** Parallel iterated local search (precedence-aware segment
  insertion with candidate lists, local double-bridge kicks). Once the AP
  duals exist, candidate lists use reduced costs and restarts can start from a
  Karp-style patching of the AP cycles (cycles merged into the path, violating
  nodes reinserted at their cheapest feasible position).
* **Exact search.** Depth-first branch and bound over path prefixes, run as
  *iterative deepening on the reduced-cost budget* B = 0, 1, 2, 3, 5, ...: a
  completed iteration proves that no path cheaper than `LB0 + B + 1` exists, the
  first success gives an incumbent, and the iteration whose budget reaches
  `UB - 1 - LB0` is the optimality proof. This made the search both a heuristic
  and an exact method and was decisive for ATSP-like instances (R_200).
* **Node bounds.** (a) cheapest allowed in-arc per unvisited node (incremental,
  undo log); (b) a full assignment bound maintained incrementally: moving to
  the next node removes one row and one column of the parent's optimal
  assignment and is repaired by one shortest augmenting path, capped by the
  remaining budget. (b) cut the node count of the first two rbg048a budget
  iterations by roughly 430x and 260x compared with (a) alone (5.55M -> 13k and
  38.9M -> 151k nodes, one thread).
* **Dominance.** Held-Karp style: a shared lock-free transposition table keyed
  by (set of visited nodes, last node) with two independent 64-bit Zobrist
  hashes (128-bit identity; a hash collision could in theory cause a wrong
  prune, probability negligible but not zero). Entries are salted per budget
  iteration. The table is lossy (replacement), which costs speed only.
* **Parallelism / NUMA.** One shared work queue of path-prefix tasks; a busy
  thread donates the unexplored siblings of its shallowest open node whenever
  threads are idle. Threads are pinned: memory-owning NUMA nodes first,
  round-robin, one hardware thread per core before SMT siblings (the 2990WX
  has two dies without local memory, discovered from `/sys`, nothing is
  hard-coded). Per-thread state is allocated after pinning (first touch); the
  big transposition table is interleaved across memory nodes with `mbind`.
  With 4 or more threads, `max(1, T/8)` threads keep running the ILS during the
  exact phase so a hopeless proof still improves the incumbent.
* **Proof logic.** `Optimal = yes` only if the final iteration exhausted the
  queue with every worker idle and no worker interrupted by the time limit.

## 5. Known limitations

* Proofs on instances with a large gap between the AP bound and the optimum
  (e.g. rbg048a: 337 vs 351) need many nodes; I could not prove it on one core.
* For N around 1000 with few constraints the heuristic result was within about
  10% of the root bound on my synthetic test (1294 vs 1178), not closer.
* The assignment-solve and the per-worker assignment snapshots use roughly
  `24 * N^2` bytes per exact thread (about 24 MB per thread at N = 1000, from
  the array sizes; resident memory was not measured).
