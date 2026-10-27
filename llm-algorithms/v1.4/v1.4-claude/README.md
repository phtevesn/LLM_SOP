# sop_solver — exact, parallel solver for the Sequential Ordering Problem

`sop_solver` reads a TSPLIB-style `.sop` file, finds a minimum-cost Hamiltonian path that
respects all precedence constraints, **proves** it optimal (or reports that it could not
within the time limit), validates the result independently, and prints one CSV row.

It is C++17, uses only the standard library + POSIX/Linux system calls, and is written to
use all cores of a many-core NUMA machine (target: Ubuntu, AMD Threadripper 2990WX, 32 cores).

---

## 1. Requirements and installation

* Linux (Ubuntu 20.04+ tested design; developed/tested with g++ 13 on Ubuntu 24.04)
* `g++` with C++17 support (g++ 9 or newer; no C++20 features are used) and `make`
* No third-party libraries. `pthread` comes with glibc.
* Python 3 is needed **only** for the optional test scripts in `tests/`.

```bash
sudo apt-get update && sudo apt-get install -y build-essential
```

## 2. Build and run

```bash
make                    # builds ./sop_solver  (uses -O3 -march=native; build on the machine that runs it)
make NATIVE=0           # portable build without -march=native

# usage
./sop_solver FILE.sop [--time-limit SECONDS] [--threads N] [--output out.csv] [--no-header]
                      [--seed S] [--memory-gb G] [--no-pin] [--no-heuristic]
```

| option | meaning (default) |
|---|---|
| `--time-limit SECONDS` | wall-clock limit for the *whole run* (none) |
| `--threads N` | number of threads (1). Use `32` on the Threadripper. |
| `--output out.csv` | also **append** the result row to this file (stdout always gets the row too). The header is written to the file only if it is new or empty. |
| `--no-header` | never print/write the header row |
| `--seed S` | random seed of the heuristic (12345) |
| `--memory-gb G` | size of the dominance (history) table in GiB. Default: a quarter of physical RAM, at most 32 GiB (16 GiB on a 122 GiB machine, since the size is rounded down to a power of two). Larger is better for hard instances. |
| `--no-pin` | disable thread pinning and per-NUMA-node data replication |
| `--no-heuristic` | debugging aid: branch-and-bound only |

Example:

```bash
make
./sop_solver examples/rbg048a.sop --threads 32 --time-limit 3600 --output results.csv
```

stdout (and `results.csv`):

```
Instance,Instance Size,Time,Enumerated Nodes,Best Cost,Best Path,Optimal
rbg048a.sop,50,123.456,987654321,351,0 9 1 17 ... 49,yes
```

All logs (progress, every improvement of the best cost, validation result) go to **stderr**, e.g.

```
[    0.083s] new best cost: 257 (ILS)
[   14.131s] new best cost: 243 (B&B)
[   53.177s] result: cost 243, proven optimal, 27023152 nodes
```

Exit code: 0 on success, 1 on input/usage errors, 2 if the CSV file could not be written.

### Tests

```bash
tests/run_checks.sh 100 1                  # 100 random instances (N=4..14) vs. brute-force DP, 1 thread
tests/run_checks.sh 100 8                  # same with 8 threads
tests/run_checks.sh 100 4 --no-heuristic   # exercises the branch-and-bound alone
python3 tests/gen_instance.py 200 0.01 1 1000 > big.sop   # random instance generator
```

## 3. Input and output formats

### Input (`.sop`, TSPLIB style)

Header lines (`NAME`, `TYPE`, `COMMENT`, `DIMENSION`, `EDGE_WEIGHT_TYPE`,
`EDGE_WEIGHT_FORMAT`) are parsed by keyword (`KEY: value`); `COMMENT` is free text. After
`EDGE_WEIGHT_SECTION` the dimension N is repeated, followed by N×N integers (`FULL_MATRIX`),
which may wrap across lines. `EOF` is optional; trailing blank lines are fine. Exactly N×N values
are read after the repeated dimension. Nodes are `0..N-1` in row/column order.

Matrix semantics (T): `T[i][j] >= 0` is the cost of going directly from i to j; `T[i][j] < 0`
means i depends on j (j must come first); the diagonal is ignored; huge values (e.g. 10000000)
are just costs. Start and end nodes are **derived**: if the precedence order has several
minimal (maximal) nodes, a zero-cost dummy source (sink) is added internally so the solver
chooses the best start (end). Cyclic precedence data is reported as infeasible
(`Best Cost = -1`, empty path, `Optimal = no`).

### Output (CSV, one row per run)

`Instance,Instance Size,Time,Enumerated Nodes,Best Cost,Best Path,Optimal`

* **Instance** – input file name (without directory); **Instance Size** – N
* **Time** – total wall-clock seconds of the whole run (parsing, heuristic, search, proof, validation) or until the time limit
* **Enumerated Nodes** – branch-and-bound nodes, summed over all threads. *Definition: one node = one partial path (prefix) that was generated and had its assignment lower bound evaluated (plus each completed path).* Replays of donated work are not counted.
* **Best Cost** – cost recomputed from scratch from the input matrix; **Best Path** – 0-based indices, space separated
* **Optimal** – `yes` only if the search space was exhausted before the time limit; otherwise `no`.
  If no feasible solution is known: `Best Cost = -1`, empty path, `no`.

The final path is always checked (permutation, every precedence, cost recomputed); a failure is logged as `INTERNAL ERROR` and `Optimal` is forced to `no`.

## 4. Algorithm and design rationale

Source layout (`src/`): `instance` (parsing, preprocessing) · `heuristic` (ILS) · `bnb`
(branch-and-bound) · `memo` (dominance table) · `numa` (topology/pinning) · `incumbent`
(shared best solution) · `solver` (orchestration) · `validate`, `output`, `cli`, `main`.

**Preprocessing.** The precedence graph is closed transitively with bitsets (cycle ⇒ infeasible).
Arcs that can never occur in a feasible path are removed statically: `i→j` is impossible if
`j` must precede `i`, or if `i` must precede `j` with some `k` forced in between. The Hasse
diagram (transitive reduction) drives O(deg) "ready node" updates.

**Upper bound: iterated local search (ILS).** Randomised greedy construction, Or-opt
(segment insertion, no reversal, O(1) delta, precedence checked incrementally while scanning)
with a work list, and a local double-bridge kick. All threads run it briefly first; a few helper
threads (`threads/8`) keep running during the search and restart from the shared incumbent.

**Exact search: parallel DFS branch-and-bound over path prefixes.**
* *Lower bound = assignment problem (AP).* For a prefix ending at `last` with unvisited set R,
  every completion plus the closing arc `end→last` is a perfect matching on R∪{last}, so the
  AP optimum is a valid bound. Duals are stored per depth and a child is **warm-started** from
  its parent (delete row/col of `last`, patch ≤2 rows), so a node costs one or two augmenting-path
  searches (O(m²)) instead of a full O(m³) Hungarian run. The dual objective is monotone, so
  a node is cut as soon as `g + dual ≥ UB`, even in the middle of an augmentation.
* *Reduced-cost child filtering and ordering.* The bound of child `j` is known in O(1) as
  `g + obj + rc(last,j)`; children are visited in increasing reduced cost and the loop stops at
  the first one that cannot improve the incumbent.
* *Completion check.* If the optimal assignment is a single precedence-feasible cycle it *is* the
  optimal completion; the node is closed immediately (this finds many improving solutions).
* *History-based dominance.* A shared lock-free hash table records, for each state
  (visited set, last node), the cheapest prefix cost seen; an arrival with equal or higher cost
  is pruned. Keys are 78 bits (64-bit Zobrist hash + 14-bit tag, plus the bucket index), stored
  with an XOR check so torn concurrent writes read as misses; a false hit that would wrongly cut
  the optimum is astronomically unlikely. Replacement evicts the deepest (least valuable) entry.
  Dominance is disabled automatically if path costs could overflow the 40-bit cost field.
* *Parallelism.* A shared task pool; workers split their *shallowest* unexplored subtree and
  donate it only when another thread is idle (no static partitioning, no fixed depth). A task is a
  prefix plus a list of sibling candidates; the receiver replays the prefix to rebuild its duals.
  Optimality is declared only when the pool is empty and no worker is busy.
* *NUMA (Threadripper 2990WX has 4 dies, two without local memory).* CPU topology is read from
  `/sys`; threads are pinned to physical cores first (SMT siblings last), spread round-robin over
  NUMA nodes; every NUMA node gets its own first-touch replica of the read-only problem data, all
  per-thread arrays are allocated by the pinned thread, and the shared dominance table is mmap'ed
  with transparent huge pages and `mbind(MPOL_INTERLEAVE)` over the nodes that own memory.

**Why this design.** SOP instances range from stacker-crane problems with dense precedence to
nearly unconstrained ATSP-like instances. The AP bound is strong on the latter, DFS over prefixes
makes the history-based dominance (the key technique in the SOP branch-and-bound literature)
cheap and exact, and the incremental dual update keeps node throughput high. DFS also needs memory
only O(N²) per thread besides the table.

### Measured behaviour (single core sandbox; your 32-core run will be much faster)

| instance | result | note |
|---|---|---|
| `prob_42.sop` | 243, proven optimal | ≈53 s, 27 M nodes on 1 thread/0.5 GiB table |
| `R_200_1000_1.sop` | 1404, proven optimal | ≈11 s (root AP bound 1392) |
| `rbg048a.sop` | 351 (best known optimum) found immediately; **not proven in 270 s on 1 thread** | root AP bound 337; this instance is dominated by ties/zero-cost arcs and needs the big dominance table and many cores |

A generated N=1000 instance starts, honours the time limit, and returns validated solutions;
proving optimality for large, weakly constrained instances is generally beyond any exact method,
in which case the best solution found is reported with `Optimal = no`.

### Honest limitations / tuning

* Increase `--memory-gb` for hard instances; the dominance table is the main lever.
* Not yet tested on a real 2990WX or with g++ 9.4 (only g++ 13); the code sticks to C++17/POSIX features available in g++ 9.
* The ILS is deliberately simple; on large instances most improvements come from the B&B's
  assignment-guided dives.
