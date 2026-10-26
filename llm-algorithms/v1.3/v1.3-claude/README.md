# SOP exact solver (parallel branch-and-bound)

Finds a minimum-cost Hamiltonian path that respects precedence constraints (Sequential Ordering
Problem, SOP) from TSPLIB-style `.sop` files, and reports whether optimality was **proven**.

* `src/sop_solver.cpp` – the whole program (single C++17 file, no dependencies)
* `tests/`, `tools/` – independent reference solvers and the test-suite used to validate it

---------------------------------------------------------------------------------------------

## 1. Requirements and installation

* Linux (developed for Ubuntu), `g++` >= 9 (C++17, pthreads). Nothing else to install:

  ```bash
  sudo apt-get install -y g++ make        # only if g++ is missing
  ```
* Optional, for the test-suite: `python3` (>= 3.8).
* Optional, recommended on the 32-core Threadripper 2990WX (4 NUMA nodes, 2 without memory):
  nothing extra is needed – the program pins threads and interleaves its big table itself
  (see section 4). `numactl` is not required.
* RAM: the history table uses ~30 % of available memory by default (capped at 24 GiB, honours
  cgroup limits). Override with `--mem-mb`.

## 2. Build and run

```bash
cd sop
g++ -O3 -march=native -std=c++17 -pthread -o sop_solver src/sop_solver.cpp
#   (building on a different machine than the one that runs it?  use -march=znver1 for the 2990WX)

./sop_solver FILE.sop [--time-limit SECONDS] [--threads N] [--output out.csv] [--no-header]
```

| option | meaning | default |
|---|---|---|
| `--time-limit S` | wall-clock limit in seconds (fractions allowed) | none |
| `--threads N` | total threads (`0` = one per physical core) | 1 |
| `--output F` | also append the result row to CSV `F` | stdout only |
| `--no-header` | never print/write the header row | header on |

Debug / test options (not needed normally): `--no-heur` `--no-history` `--no-pin` `--seed K`
`--mem-mb M` `--quiet`. `SOP_DEBUG=1` in the environment re-validates every incumbent.

Example (all 32 cores, 1 hour limit, results appended to `results.csv`):

```bash
./sop_solver /data/R_200_1000_1.sop --threads 32 --time-limit 3600 --output results.csv
```

stdout (log lines go to **stderr** only):

```
Instance,Instance Size,Time,Enumerated Nodes,Best Cost,Best Path,Optimal
R_200_1000_1.sop,200,<seconds>,<nodes>,1404,0 ... 199,yes
```

Exit code: 0 on success (including "not proven"/"infeasible" rows), 1 on usage/parse errors,
2 if the final path ever failed the built-in validity check (should never happen; no row is printed).

## 3. Input and output formats

**Input** – TSPLIB-style. Header lines are parsed by keyword (`DIMENSION`, `EDGE_WEIGHT_FORMAT`; the
rest is ignored). After `EDGE_WEIGHT_SECTION` the program reads the repeated `N` and then exactly
`N*N` integers (may wrap over lines, any whitespace). A trailing `EOF` line is optional; blank
lines/no final newline are tolerated. Only `FULL_MATRIX` is supported. `N <= 5000` is accepted.

Semantics: `T[i][j] >= 0` cost of arc i->j; `T[i][j] < 0` means *i depends on j* (j must come
earlier); the diagonal is ignored; huge values are ordinary costs. Start and end are **derived**:
the start must be a root of the (transitively closed) precedence DAG and the end a leaf. If there
are several roots (leaves) a zero-cost dummy start (end) node is added internally, so the best
start/end pair is found by the search; dummies never appear in the output.

**Output** – one CSV row per run:

| column | meaning |
|---|---|
| Instance | input file name (without directory) |
| Instance Size | N |
| Time | total wall-clock seconds of the run (parse, preprocessing, search, validity check) |
| Enumerated Nodes | sum over all threads of search-tree nodes processed (definition below) |
| Best Cost | cost recomputed from scratch from the original matrix; `-1` if no feasible path exists/was found |
| Best Path | 0-based node indices, space separated (empty if none) |
| Optimal | `yes` only if optimality was proven before the time limit, else `no` |

*Enumerated node* = one B&B tree node that was generated and evaluated (history lookup, bound
computation or leaf completion); the root counts as 1. Heuristic (ILS) moves and the internal replay of an
already-counted ancestor path when a thread picks up donated work are **not** counted.

Behaviour notes: with `--output` the row goes to stdout and is appended to the file; the header is
written to the file only if it is new/empty (never with `--no-header`), stdout always gets it unless
`--no-header`. Contradictory precedence (a cycle) means no feasible path: the row has cost `-1`,
empty path, `no`. Before output every path is checked (permutation, all precedences, cost
recomputed) against the original matrix.

## 4. Algorithms and reasoning

The three supplied papers (Shobaki & Jamal 2015; Gonggiatgul, Shobaki, Muyan-Özçelik JPDC 2023;
Shobaki et al. ICPP-W 2023) motivate the design: a depth-first B&B with a strong relaxation bound,
history-based domination, a pool-based parallelisation and a concurrently running heuristic that
shares its best solution. What was built and why:

1. **Preprocessing.** Transitive closure with bitsets (Warshall) – detects cycles, finds roots and
   leaves. An arc i->j can follow immediately only if j is not a predecessor of i and no node lies
   between them in the closure; all other arcs get cost `BIG` (> any real path) so every later
   component sees a plain cost matrix. Dummy start/end handle several roots/leaves.
2. **Lower bound: assignment-problem (AP) relaxation**, the same relaxation as the "MCPM" bound of
   the JPDC paper. For a prefix ending in `l` with unscheduled set `U`, each of `{l}∪U\{end}` gets
   one successor in `U` and each of `U` one predecessor; the optimal perfect matching is a valid
   bound. It is maintained *incrementally* with duals: scheduling `x` deletes row `l` and column `x`
   and re-augments at most two rows (shortest-augmenting-path step, O(n²)); the row of the new last
   node is restricted to *ready* successors. On random asymmetric costs (SOPLIB `R.*`) this bound is
   within ~1 % of the optimum, far tighter than the edge/MST bounds of the 2015 paper, which cost
   about the same per node. Reduced costs also give an O(1) lower bound per child
   (`AP + c(l,x) − u_l − v_x`), used to discard and order children (best-first inside DFS).
3. **History-based domination** (key = scheduled set + last node, value = best prefix cost). The
   table stores **full keys** (never just hashes), so a hash collision cannot cause a wrong
   prune. Buckets of 4 slots with a spin lock; when full, the deepest entry is evicted (shallower
   entries save more work; evicting is always safe because entries are only hints).
   *Deliberately not implemented:* the paper's "suffix reuse / history utilization" and
   thread stop/resume. With several threads a history entry can still be *in progress*, so a stored
   "suffix bound" is not yet a valid bound; plain domination (`g' >= g_stored`) remains sound
   because an in-progress entry with a smaller prefix cost will itself finish exploring the
   equivalent sub-problem.
4. **Parallel search.** All B&B workers share the incumbent (atomic cost, mutex for the path),
   the history table and a priority task-pool (ordered by lower bound). A task is a prefix path; a
   worker replays it (without touching the table) and explores the subtree depth-first. When some
   worker is idle and the pool is empty, busy workers *donate* the shallowest unexplored
   siblings (largest remaining work) – cooperative work sharing without locks on the hot path.
   Termination = pool empty and no active worker; a timeout never counts as proof.
5. **Heuristic thread.** An iterated local search (greedy construction, precedence-aware segment
   insertion (or-opt) in both directions, random segment-move kicks) runs on the main thread and
   shares its best path with the B&B (which then prunes earlier). After it stagnates it joins the
   B&B pool, so all cores end up searching. With `--threads 1` a short ILS pre-phase is used and then
   all the time goes to B&B. The ILS is my own simple heuristic, **not** LKH; on random asymmetric
   instances it is weak (the B&B's AP-guided dive usually finds better solutions).
   If the incumbent equals the root AP bound the run stops immediately (proven optimal).
6. **NUMA / CPU topology (Linux sysfs, best effort).** Worker threads are pinned to physical cores
   first (SMT siblings only if more threads than cores), distributed round-robin over NUMA nodes;
   each worker allocates its own scratch memory after pinning (first-touch locality); the
   history table is `mbind`-interleaved over the nodes that own memory (on the 2990WX only 2 of 4
   nodes do); for large matrices (N >= 362) a per-node replica of the cost matrix is used. Disable
   pinning with `--no-pin`. **These parts could not be exercised on a real multi-node machine in
   the development environment (1 CPU, 1 node); see "Verification status".**

## Verification status (what was and was not checked)

Development environment: 1 vCPU, 1 NUMA node, 4 GB RAM. Therefore **speed-ups, load balance and
the NUMA code paths with 32 cores were not measured** – multi-thread runs were only used for
correctness (oversubscribed threads on one core).

*Checked against independent references* (`tests/`): brute force over all permutations and a
Held-Karp style DP (the two agree on 600/600 random instances), compared with the solver on
random instances including several roots/leaves, ties, zero costs, N=1, N=2, cyclic (infeasible)
inputs, wrapped lines, missing EOF: **250/250** (x4 configurations: 1 thread, 3 and 4 threads
oversubscribed, with/without heuristic), **150/150 + 100/100** for N up to 13, and **60/60 + 60/60**
for N 14–24 against an independent C++ DP. Three deliberately broken builds (`-DSOP_MUTANT=1..3`)
are each caught by the suite, i.e. the tests can fail. ThreadSanitizer, AddressSanitizer and
UBSan runs (small instances and 30–40 s on the real instances with 4 threads) reported nothing.

*Not independently verified:* the optimal values/proofs on the three supplied instances (the
plain DP and a bounded DP both exceed memory there). Those results rest on the solver's own bound
and history logic, which is validated on the smaller random instances above.

Results on the three supplied instances (1 vCPU sandbox, `--threads 1`; **not** the 32-core target):

| instance | N | best cost | proven | time | enumerated nodes |
|---|---|---|---|---|---|
| prob_42 | 42 | 243 | yes | ~97-102 s | 31,989,350 |
| R_200_1000_1 | 200 | 1404 | yes | ~37-41 s | 2,005,643 |
| rbg048a | 50 | 351 | **no** (120 s limit hit; a longer run was killed by a sandbox restart, so its outcome is unknown) | 120 s | 88.9 M |

Node counts are deterministic for a single thread and were reproduced exactly in a second run.

```bash
# run the test-suite
g++ -O2 -std=c++17 -pthread -o sop_solver src/sop_solver.cpp
g++ -O2 -std=c++17 -o dp_ref tools/dp_ref.cpp
python3 tests/selfcheck_refs.py                 # references agree with each other
python3 tests/run_tests.py 250 8 --no-heur      # vs brute force
python3 tests/run_tests.py 250 8 --threads 4 --no-pin --no-heur
python3 tests/run_mid.py 60 --no-heur           # N 14..24 vs tools/dp_ref
```
