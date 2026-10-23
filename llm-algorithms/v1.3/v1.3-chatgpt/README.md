# Exact parallel Sequential Ordering Problem solver

A standalone C++17 solver for the supplied SOP format, with integer optimality
proofs by branch-and-bound, incremental assignment bounds, precedence pruning,
bounded history domination, and OpenMP parallel tasks. No commercial solver,
Python package, or downloaded optimization library is needed to run it.

This is an original, tested implementation informed by the three supplied papers.
It is not a reproduction of their full implementations, and no claim is made that
it outperforms established research solvers. An input size of 1,000 is supported;
that does **not** mean every 1,000-node instance can be solved in practical time.
Unrestricted SOP has exponential worst-case search. Use a time limit for hard data.

## 1. Requirements and installations required

Ubuntu with a C++17 compiler and OpenMP (GCC 9 or later recommended), GNU Make,
and enough RAM for the requested worker count. Python 3 is needed only for tests.

```bash
sudo apt-get update
sudo apt-get install -y build-essential python3 numactl
```

`numactl` is optional and only used for hardware inspection and placement.
The code has no network access and does not invoke other programs.

## 2. Exact commands to build and run

Extract the archive, enter its `sop_solver` directory, then:

```bash
make -j
./sop /path/to/prob.42.sop
./sop /path/to/rbg048a.sop --time-limit 3600 --threads 32 --output results.csv
./sop /path/to/R.200.1000.1.sop --time-limit 3600 --threads 32 --no-header
make test
```

Defaults: one worker, no time limit, CSV on stdout with a header. A file supplied
with `--output` receives the same data row in addition to stdout. Existing CSV
files are appended to. Do not have independent processes append to the same CSV
simultaneously; interprocess locking is not provided.

Additional tuning options:

| Option | Default | Meaning |
|---|---:|---|
| `--history-mb MB` | 16 | Approximate history-table budget **per worker**; 0 disables history |
| `--heuristic-starts N` | 32 | Total greedy/descent starts, including initial start |
| `--seed N` | 1 | Unsigned 32-bit random seed for heuristic starts |
| `--help` | | Print CLI usage |

Example with larger history and more initial solution search:

```bash
./sop /path/to/instance.sop --threads 32 --time-limit 3600 \
  --history-mb 64 --heuristic-starts 128 --seed 7 --output results.csv
```

More heuristic starts can help obtain a better upper bound but delay exact search.
More history memory can reduce repeated subproblems. Neither option changes
exactness. Parallel scheduling can change the returned optimal path, node count,
and incumbent at a timeout even with a fixed random seed.

### Threadripper 2990WX / NUMA

Inspect the actual operating-system topology and allowed CPUs first:

```bash
lscpu -e=CPU,CORE,SOCKET,NODE
numactl --hardware
```

Start with one worker per physical core (32), not automatically one per SMT
logical CPU. The 2990WX has nonuniform memory access; effective placement depends
on firmware and the topology Linux exposes. Do not assume a CPU-number ordering
or a fixed set of memory-bearing NUMA node IDs.

A baseline placement to measure on your machine is:

```bash
OMP_PLACES=cores OMP_PROC_BIND=spread OMP_STACKSIZE=64M \
  numactl --interleave=all ./sop /path/to/instance.sop \
  --threads 32 --time-limit 3600 --history-mb 64 --output results.csv
```

Compare against the same command without `numactl --interleave=all`, and against
`OMP_PROC_BIND=close`. Interleaving can help distribute the read-mostly matrix
but can hurt worker-local history access. It is a benchmark option, not a promised
speedup. The program respects the process affinity mask and OpenMP settings;
it does not hardcode CPU affinity or move memory pages itself.

Workers use separate history maps, with entries allocated on the accessing
worker, and cache-line-separated counters. Task states are independently owned;
the matrix and incumbent are shared. OpenMP schedules bounded tied tasks among
workers. The implementation does not provide NUMA-aware task stealing or a
specific runtime scheduling guarantee. Use an appropriate affinity mask if your
process is restricted to a subset of the host's CPUs.

History budgets are estimates, not RSS limits: STL allocation overhead varies.
At default settings, 32 workers target roughly 512 MiB of history, plus the
matrix, matching state, search stacks, task states, allocator overhead and the
OpenMP runtime. At 1,000 nodes, deep paths can retain tens of MiB of state per
active search chain. Start smaller if RAM is constrained. No claim of 32-core
scaling is based on the included tests; the development host is not a 2990WX.

## 3. Input and output format descriptions

### Input

The parser reads metadata by keyword, accepts whitespace and optional `:`, and
recognizes `DIMENSION`, `TYPE`, `EDGE_WEIGHT_TYPE`, and `EDGE_WEIGHT_FORMAT`.
`NAME`, `COMMENT`, and unknown extension metadata do not affect the graph.
`COMMENT` is treated as free text, including text that resembles metadata.
`DIMENSION` and `EDGE_WEIGHT_SECTION` are required. When present, the type fields
must be `SOP`, `EXPLICIT`, and `FULL_MATRIX` respectively.

`EDGE_WEIGHT_SECTION` must be on its own line, followed by the repeated dimension
and exactly N*N whitespace-separated signed integers. Rows may wrap arbitrarily.
An optional `EOF` token and trailing whitespace are accepted. An incorrect
repeated dimension, missing values, extra values, noninteger entries, nonzero
diagonal, or unsupported format produces an error on stderr and exit status 2.
N must be 1 through 1,000. Comma-separated matrices and TSPLIB variants without
the repeated dimension are outside this prompt's format and are rejected.

For row i, column j:

* A nonnegative entry is the directed edge cost i -> j.
* **Any negative entry** means j must occur before i. It is not a travel cost.
* Diagonal entries must be zero and do not contribute to a path cost.
* Large finite values remain ordinary costs, not missing edges.

Costs and bounds use signed 64-bit integers. To prevent arithmetic overflow,
positive entries may not exceed
`floor((INT64_MAX / 4) / (8 * (N + 1)))`.
At N=1,000 this still permits costs above 2.8e14 per edge. Larger inputs are
explicitly rejected rather than silently rounded or overflowed.

Nodes are 0-based in matrix order. Start candidates are all precedence sources;
end candidates are all precedence sinks. The solver optimizes over them instead
of assuming node 0 and node N-1. Cyclic precedences make the instance infeasible.
Under these matrix semantics every acyclic precedence graph admits a feasible
path, because every consistent consecutive pair has a nonnegative matrix entry.

### Output

Exactly these seven columns are emitted:

```text
Instance,Instance Size,Time,Enumerated Nodes,Best Cost,Best Path,Optimal
```

* `Instance`: basename of the input, correctly CSV-quoted.
* `Instance Size`: dimension N.
* `Time`: wall-clock seconds including input parsing, preprocessing, heuristics,
  exact search, worker shutdown and final validation; excludes CSV I/O itself.
* `Enumerated Nodes`: sum of processed search-state entries across workers,
  including states rejected during matching repair, bound or history pruning.
  The root counts when search is entered. Matching augmentations, preprocessing,
  local-search moves and heuristic starts are not separate enumerated nodes.
  A solution proved by the initial assignment bound may report zero nodes.
* `Best Cost`: independently recomputed cost of the incumbent, or -1 when absent.
* `Best Path`: space-separated node indices, or an empty quoted field when absent.
* `Optimal`: `yes` only for a validated solution with completed exact proof and
  no observed expiration; otherwise `no`.

`--output` writes a header only to a new/empty file. Stdout always gets its own
header. `--no-header` suppresses both. All diagnostic messages go to stderr.
A timeout or a cyclic instance emits a normal CSV row and exits 0; malformed
input, internal validation failure or I/O failure exits 2. Cyclic instances use
cost -1, empty path, and `Optimal=no`, with infeasibility explained on stderr.
This CSV schema does not have a separate infeasibility-status column.

The time limit starts before reading the instance. Expiration is checked in
preprocessing, greedy/descent loops, each assignment augmentation iteration and
each search node. Input parsing, memory allocation, scheduling, and final
validation are not forcibly interrupted, so wall time can slightly exceed the
limit. The limit is cooperative, not a hard real-time kill. Output is suppressed
on fatal errors rather than emitting an unverified solution. Final validation
checks permutation, every original negative entry, and cost from the original
matrix. A proof completed near a deadline may conservatively be reported `no`
if expiration is observed during final validation.

## 4. Algorithms and reasoning for the choices

### Precedence preprocessing and feasible branching

Kahn's algorithm detects cycles. Bitset transitive closure supports fast
precedence checks. An arc i -> j is removed from the relaxation if j must precede
i, or if some k is forced strictly between i and j. Such an arc cannot be used
by any feasible path. Only vertices whose remaining predecessor counts are zero
are eligible at a search node. This enumerates all topological permutations.

### Incremental minimum-cost assignment lower bound

The root relaxation has rows for all nodes plus a virtual source, and columns
for all nodes plus a virtual sink. Virtual-source arcs of cost zero go to every
precedence source; cost-zero sink arcs leave every precedence sink. There is no
virtual-source-to-virtual-sink arc. Self-loops and forbidden arcs are absent.
Every feasible Hamiltonian path induces a perfect assignment, but an assignment
can contain disconnected cycles and violate global ordering. Its minimum cost
is therefore a **lower bound**, not normally a solution.

After committing last -> v, remove the old last row and the v column. Retain the
matching and feasible dual potentials. Deleting this row and column leaves at
most one unmatched row. The new last row additionally forbids nonready next
vertices and premature termination, potentially leaving a second unmatched row.
Repair using at most two shortest augmenting paths. This avoids restarting the
Hungarian algorithm from scratch at every search node. Root work is O(N^3) in
the worst case; a child repair costs O(m^2) per augmentation for m residual nodes,
plus O(N) state copies and scans.

With assignment bound L, committed cost C, and incumbent U, prune when C+L >= U.
Assignment duals also provide nonnegative reduced costs r(last,v): a branch can
be omitted if C+L+r(last,v) >= U. Branches are ordered by reduced cost, then edge
cost. If the assignment itself forms a feasible topological path, it attains the
bound and solves that subtree immediately.

### History domination and bounded memory

A history key stores the **complete visited bitset and last node**; hash equality
alone is never treated as state equality. For two paths with the same key,
the remaining feasible suffixes are identical. A prefix with cost no smaller
than one previously accepted can therefore be discarded. Cheaper later prefixes
replace older costs and are explored again.

Each worker owns a history map. On reaching its estimated capacity it clears
that map; forgetting entries affects speed, not correctness. A dominating search
may still have queued descendants; these remain scheduled. No subtree is
cancelled based on an incomplete history record. Thus all pruned states have an
equal-or-better representative either explored, bounded, or still scheduled.
There is no cross-worker history lookup or paper-style stop/resume mechanism.

### Incumbent heuristics and parallel scheduling

A precedence-feasible nearest-neighbor path supplies the first upper bound.
Randomized multistart construction plus precedence-feasible Or-opt-style
relocation of blocks of one to three nodes improves it. Each descent has up to
eight passes. These heuristics never establish optimality by themselves.

The first start runs before matching; remaining starts run in parallel before
exact search. Search workers publish better, validated paths through an atomic
cost and mutex-protected path. A bounded number (up to eight times the worker
count) of explicit OpenMP tasks exposes sibling subtrees; small subtrees stay
in depth-first search. The final parallel-region barrier includes all descendant
tasks. Exceptions inside workers stop the run and result in an error, not an
optimality claim.

### Why the optimality claim is valid

1. Feasible branching covers every precedence-respecting Hamiltonian path.
2. Static arc elimination excludes only provably impossible consecutive pairs.
3. Assignment and reduced-cost bounds never exceed the cost of a feasible
   completion in the corresponding branch.
4. Dominance only removes prefixes whose identical suffix space is represented
   with equal or lower committed cost.
5. If all scheduled search work finishes without expiration, every better path
   has either been examined or excluded by one of those arguments.
6. The reported incumbent is checked independently against the original matrix.

This is an algorithmic optimality proof, not a separately checkable proof-log
certificate. No floating-point tolerance is used in optimization decisions.
Time measurement is the only floating-point component relevant to stopping.

### Relationship to the supplied research

* Shobaki and Jamal, *An exact algorithm for the sequential ordering problem and
  its application to switching energy minimization in compilers*,
  DOI 10.1007/s10589-015-9725-9: informs topological enumeration, precedence-aware
  edge elimination, free endpoints, and history domination.
* Gonggiatgul, Shobaki, and Muyan-Ozcelik, *A parallel branch-and-bound algorithm
  with history-based domination and its application to the sequential ordering
  problem*, Journal of Parallel and Distributed Computing 172 (2023), 131-143:
  informs incremental matching, load distribution, history/memory tradeoffs and
  the importance of avoiding unsafe cancellation of parallel work.
* Shobaki, Gonggiatgul, Normington, and Muyan-Ozcelik, *Combining a Parallel
  Branch-and-Bound Algorithm with a Strong Heuristic to Solve the Sequential
  Ordering Problem*, ICPP Workshops 2023: motivates strong initial incumbents
  and sharing improvements among workers.

The implementation does not bundle LKH, reproduce the papers' parallel
stop/resume and restart logic, or run a separate continuous LKH worker.
Its heuristic is simpler than LKH. It also has no branch-and-cut fallback,
checkpoint/resume, external incumbent import, or independently checkable proof
certificate. Those are material differences when comparing research results.

## Validation and reproducibility

`make test` runs 261 solver invocations covering random directed costs and DAGs,
free and relabeled endpoints, ties and zero costs, negative precedence values
other than -1, single-node instances, cyclic precedence, wrapped matrices with
and without EOF, malformed input, CSV append/quoting, CLI errors and timeout.
It compares 235 cases against an independent exact subset-DP oracle and 10
additional cases against exhaustive permutations. A shuffled 1,000-node forced
chain exercises the maximum supported size. Tests alternate one and four workers
and include disabled history.

```bash
make debug
ASAN_OPTIONS=detect_leaks=0 python3 tests/test_solver.py ./sop-debug
```

The debug build enables AddressSanitizer and UndefinedBehaviorSanitizer.
See `validation/RESULTS.md` and `validation/benchmarks.csv` for actual supplied
instance runs. Benchmark rows are feasible solutions, not claims of optimality
unless their `Optimal` column says `yes`. The research PDFs and supplied instances
are not redistributed in this package.
