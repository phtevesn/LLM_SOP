# SOP Exact

A standalone C++17 exact Sequential Ordering Problem solver for Ubuntu, with
parallel branch-and-bound, incremental integer assignment bounds, two complementary
search engines, incumbent sharing, work stealing, and Linux NUMA-aware scheduling.
No commercial optimizer, Python package, or third-party solver is required.

**Scope:** this is a tested exact solver, not a claim of world-best benchmark
performance. SOP is NP-hard. Accepting N <= 1000 does not mean every such instance
can be solved in practical time or memory. An unrestricted run continues until it
proves optimality, is interrupted, or encounters a resource/error condition.
A time-limited run may return a feasible solution without a proof. There is no
approximate pruning, floating-point objective, or fabricated optimality flag.

## Section 1: Requirements and installations required

- Ubuntu Linux, 64-bit x86; GCC 9+ with C++17 and signed 128-bit integer support.
- GNU Make and POSIX threads (included in normal Ubuntu development tools).
- Python 3.9+ for the optional tests and independent solution validator only.
- Optional `numactl` and `lscpu` for checking CPU and memory topology.

```bash
sudo apt update
sudo apt install build-essential python3 numactl
```

No runtime dependencies beyond the C++ standard library and pthreads are used.
The source was compiled and tested with GCC 13.3 on Ubuntu in the development
environment, using C++17 mode. GCC 9 compatibility is based on the language and
standard-library features used; GCC 9 itself was not available for testing.
It has not been benchmarked on the user's actual 2990WX.

Memory use includes approximately 8(N+1)^2 bytes **per worker** for private costs,
shared input/precedence data, search states, and bounded history tables. At N=1000,
32 matrix copies consume about 245 MiB. Search frontier memory is additional and
not bounded by `--history-mb`; difficult arc searches can exhaust RAM. Start with
several GiB of free RAM. Allocation failures produce an error, not a proof.

## Section 2: Exact commands to build and run with an example

Unpack the archive, then:

```bash
cd sop-exact
make -j
./sop examples/prob.42.sop
```

The default is one worker, no time limit, stdout CSV, and a header. For the 32-core
Threadripper 2990WX:

```bash
./sop examples/R.200.1000.1.sop --threads 32 --time-limit 3600 --output results.csv
./sop examples/rbg048a.sop --threads 32 --time-limit 3600 --output results.csv
./sop /path/to/input.sop --threads 32 --output results.csv
```

The third command has **no time limit**. To omit CSV headers on both destinations:

```bash
./sop examples/prob.42.sop --threads 32 --time-limit 600 --no-header
```

Optional native optimization (build on the machine that will execute it):

```bash
make clean
make CXXFLAGS='-O3 -DNDEBUG -std=c++17 -march=native -flto -pthread -Wall -Wextra -Wpedantic' LDFLAGS='-pthread -flto'
```

Additional tuning options:

| Option | Default | Meaning |
|---|---|---|
| `--strategy portfolio` | selected | With >=2 workers, split workers between the two exact engines. With one worker, use the arc engine. |
| `--strategy prefix` | | All workers use precedence-feasible prefix search and history dominance. |
| `--strategy arcs` | | All workers use assignment subtour/precedence-conflict branching. |
| `--history-mb 512` | 512 | Total nominal history budget in MiB. Shared mode divides it among shards; local mode divides it by requested workers. Only prefix search uses it. Zero disables history. |
| `--history-mode shared` | shared | Share dominance across prefix workers using 256 locked shards. `local` selects private tables for lower synchronization traffic. |
| `--seed 1` | 1 | Random seed for feasible incumbent perturbations. Parallel execution is not deterministic. |
| `--no-pin` | pinning enabled | Let the OS schedule workers; useful for comparing placement policies. |

For example, compare the engines on your actual instance family:

```bash
./sop examples/rbg048a.sop --strategy prefix --threads 32 --time-limit 600 --history-mb 2048
./sop examples/rbg048a.sop --strategy arcs --threads 32 --time-limit 600
```

### Threadripper 2990WX and NUMA

Inspect the **actual Linux topology**, including BIOS and cpuset restrictions:

```bash
lscpu -e=CPU,NODE,SOCKET,CORE
numactl --hardware
```

The program discovers allowed CPUs with `sched_getaffinity` and reads Linux sysfs
core/node topology. It chooses one logical CPU per physical core before using SMT
siblings, spreads those selections across reported NUMA nodes, and pins each
worker. Start with **32 workers**, corresponding to physical cores, rather than
assuming 64 SMT threads will be faster.

Each worker allocates and first-touches its private cost matrix after
pinning. Local history tables are also first-touched there. In shared-history mode,
shards are allocated on first use by workers, with the usual kernel memory fallback. Workers prefer stealing from the same NUMA node within their search
engine before stealing remotely. Hungarian scratch buffers are private; incumbents, termination counters, and task
queues are shared. Shared history adds synchronization and remote-memory traffic;
`--history-mode local` provides a comparison for memory-bandwidth-sensitive runs.
The queues are cache-line aligned.

This does not assume that every CPU node has local RAM. On the 2990WX's asymmetric
layout, allocation on a memoryless node still uses the kernel's eligible fallback
memory node. The code does not hardcode node IDs, force allocation onto a
memoryless node, or override the user's memory policy. Shared preprocessing data
is not replicated, and transferred tasks can still incur remote accesses.
First-touch locality is a placement preference, not a guarantee: allocator reuse,
container policies, and kernel page migration can affect it.

For a measured comparison, you can also run:

```bash
numactl --interleave=all ./sop examples/prob.42.sop --threads 32 --time-limit 600
```

Interleaving trades locality for bandwidth; it is not assumed to be better.
Do not blindly bind all workers to one node or assume a 32x speedup. Root parsing,
preprocessing, and the initial assignment solve are serial; search and incumbent
improvement are parallel. Small or already-solved instances cannot usefully keep
32 cores busy. When either portfolio engine proves the global optimum, the other
engine stops.

### Tests and validation

```bash
make test
make sanitize
ASAN_OPTIONS=detect_leaks=0 python3 tests/test_solver.py ./sop-sanitize
ASAN_OPTIONS=detect_leaks=0 python3 tests/test_extended.py ./sop-sanitize
```

`detect_leaks=0` is needed in the restricted development environment because
LeakSanitizer cannot inspect `/proc` there. On a normal Ubuntu host you may omit
it to enable leak checking as well. AddressSanitizer and UndefinedBehaviorSanitizer
remain enabled in either case.

Tests independently compare all engines against exhaustive permutation search
on 180 random instances (900 engine/thread comparisons), and against subset DP
on 14 further instances (40 comparisons: N=10..16 plus two-chain N=70 and N=130). They also cover arbitrary
endpoints, zero weights, indirect precedence, non-unit negative markers, large
integer costs, cyclic constraints, malformed input, append/header behavior,
SIGINT, deadlines up to 32 workers, a shuffled 1000-node unique chain, and a 1000-node instance with two possible orders.

An independent Python checker verifies returned paths and costs:

```bash
./sop examples/R.200.1000.1.sop --threads 32 --output run.csv
python3 tools/verify.py examples/R.200.1000.1.sop run.csv
```

It checks feasibility and objective only; a path by itself is not an independent
optimality certificate. See Section 4 for how the search proves optimality.

## Section 3: Input and output format descriptions

Usage:

```text
sop FILE [--time-limit SECONDS] [--threads N] [--output out.csv] [--no-header]
```

The input is keyword-parsed TSPLIB-style SOP with `DIMENSION` in 1..1000,
`EDGE_WEIGHT_TYPE: EXPLICIT`, `EDGE_WEIGHT_FORMAT: FULL_MATRIX`, and
`EDGE_WEIGHT_SECTION`, followed by the repeated dimension and exactly N*N signed
integers. Metadata can use a colon or whitespace separator. COMMENT text is
ignored. Matrix whitespace and line wrapping are arbitrary. An optional trailing
`EOF` and trailing whitespace are accepted; extra matrix tokens are rejected.
TYPE/weight metadata, when supplied, must describe the supported format.

- Nodes are numbered 0..N-1 in matrix order.
- A negative off-diagonal T[i][j] means j must precede i; its magnitude is ignored.
- Nonnegative off-diagonal entries are actual costs, including zero and large costs.
- Diagonal entries are ignored.
- Starts and ends are not hardcoded. All legal choices participate in optimization.
- Duplicate DIMENSION, missing data, out-of-range N, and unsupported formats fail.

Costs are parsed as signed 64-bit integers. A positive cost must be at most
`floor((INT64_MAX / 4) / (8 * (N + 1)))`; larger input costs are rejected rather
than silently overflowing. This still permits approximately 2.88e14 per edge at
N=1000. Path and assignment costs use checked-range 64-bit arithmetic. Hungarian
potentials, reduced costs, and distances use signed **128-bit** integer arithmetic.
No epsilon or floating-point comparison is used for optimization. Floating point
is used only for wall-clock time limits and display.

Output has exactly these columns:

```text
Instance,Instance Size,Time,Enumerated Nodes,Best Cost,Best Path,Optimal
```

- **Instance:** input basename, CSV quoted.
- **Instance Size:** N from the header.
- **Time:** steady-clock wall seconds from program entry through parsing,
  preprocessing, search, worker shutdown, and final validation, measured just
  before CSV serialization. Startup/validation are not excluded from the budget.
  OS scheduling, allocation, cleanup and output I/O can make process termination
  occur after the requested limit; this is a cooperatively checked, soft deadline.
- **Enumerated Nodes:** assignment search subproblems constructed for the root or
  a branch, summed over constructing workers, including infeasible and bound-pruned
  subproblems. A queued state is not counted again when stolen or expanded.
  Internal Hungarian iterations, heuristic moves, and preprocessing are not tree
  nodes. Immediate zero-cost/unique-order proofs can report zero nodes.
- **Best Cost:** independently recomputed path cost, or -1 if none was found.
- **Best Path:** quoted, space-separated, 0-based permutation, or an empty field.
- **Optimal:** `yes` only after a complete exact search, a matching upper/lower
  bound, or an immediate zero-cost/unique-order proof, with the deadline still
  unexpired. Otherwise `no`.

Before output, the solver checks permutation membership, every original
precedence relation, and a fresh objective sum, and compares that sum to its stored
incumbent. A violation is an error, not an accepted answer.

On timeout or SIGINT/SIGTERM, the best available solution is returned with `no`.
A zero time limit can stop during parsing before a path exists. A precedence cycle
is diagnosed as infeasible on stderr; the CSV uses -1, empty path, and `no` because
its `Optimal` field describes an attained solution, not an infeasibility proof.
Malformed input and internal/resource/output errors exit with code 2 and an error
on stderr. Successful solving and ordinary timeouts exit with code 0.

All diagnostics go to stderr. Without `--output`, CSV goes to stdout. With
`--output`, the same row goes to both stdout and the appended file. Stdout gets a
header unless `--no-header`; the file gets one only when new/empty and headers are
enabled. Concurrent independent processes should use separate output files or an
external lock; append/header creation is not a multi-process transaction.

## Section 4: Algorithm names, descriptions, and reasoning for choices

### 4.1 Dummy-node reduction and precedence preprocessing

Add a zero-cost dummy node d. A Hamiltonian path becomes a cycle through d; remove
d to recover the path. Only precedence sources can follow d and only precedence
sinks can precede d. This covers every legal start/end pair without fixing either.
Kahn's algorithm detects cycles and unique topological orders. Bitset transitive
closure removes backward arcs, and removes i->j when some node k must occur
strictly between i and j. These eliminations preserve every feasible path.

### 4.2 Integer assignment relaxation and dynamic Hungarian updates

A minimum-cost perfect matching between outgoing and incoming node copies is a
cycle cover. Relaxing connectivity and precedence gives a valid lower bound.
The root uses the Hungarian shortest-augmenting-path algorithm. It keeps an exact
primal matching and dual row/column potentials.

Prefix branching fixes last->x, removes one row and one column, and repairs the
matching instead of solving the assignment from scratch. The new prefix endpoint
is restricted to currently eligible successors; this can require a second repair.
Arc branching forbids a currently matched arc and repairs one augmenting path.
A repair has O(N^2) worst-case time, versus O(N^3) for a fresh dense assignment.
If no augmenting path exists, that branch has no feasible assignment and is safely
pruned. Child objective bounds include the already-fixed prefix cost where needed.

For prefix candidates, dual feasibility gives the cheap bound
`parent_bound + reduced_cost(last,x)`, before constructing the child. It is valid
because every matching containing that arc pays at least its reduced cost above
the dual objective. An assignment that itself forms a feasible full path supplies
an incumbent and closes that state immediately.

### 4.3 Prefix branch-and-bound with exact history dominance

Enumerate only precedence-eligible next nodes. Order children by their lower-bound
estimate and then immediate edge cost. Two prefixes with the same visited set and
last node have identical feasible completions; the more expensive prefix can be
pruned. Bounded, four-way set-associative history tables store **complete**
visited bitsets, last nodes, and prefix costs. Hash matches alone never prune.
Collisions and replacement only discard useful history; they cannot invalidate
the proof. Table allocations are bounded and power-of-two rounded down from the
configured budget. In the default shared mode, a hash selects one of 256 cache-line-aligned shards;
a short mutex-protected lookup/update eliminates dominated duplicates across
workers. Equal-cost duplicates are pruned only after another state has been
recorded; an active or queued representative remains covered by the termination
accounting. Lower-cost replacements are explored in their own right. In local
mode, tables require no locks, but duplicates on different workers may be explored.
Evicted representatives remain covered by their search tasks; eviction never
cancels a task. Shared history is bounded, not a permanently growing global map.

### 4.4 Assignment subtour and precedence-conflict branching

The complementary engine searches over assignment constraints. If the cycle
cover has a subtour excluding d, at least one arc in that cycle must be absent in
any valid Hamiltonian cycle. Choose a shortest such cycle. If the assignment is
one full cycle but violates j-before-i, at least one arc on its directed path from
i to j must be absent in any valid SOP solution. Choose a shortest directly
witnessed precedence violation.

For conflict edges e1..ek, use the disjoint partition:

```text
exclude e1
include e1, exclude e2
include e1, include e2, exclude e3
...
```

Already-forced edges cannot be excluded. If all conflict edges are forced, the
state is infeasible. Including a currently matched edge preserves the current
optimal assignment; excluding one requires one dynamic repair. Sparse,
copy-on-write forbidden-row lists avoid copying an N*N exclusion matrix per node.
This engine uses an explicit local stack, avoiding recursion depth proportional
to the number of arc decisions.

### 4.5 Feasible upper bounds and parallel portfolio

A greedy topological path supplies an early feasible incumbent. Workers improve
shared incumbents using random precedence-feasible relocations and best-improving
relocation of contiguous blocks of length 1..3. Periodic short perturb-and-improve
runs continue during exact search. These heuristics affect speed and solution
quality at timeout; they never justify a proof or prune by themselves.

With two or more portfolio workers, approximately half run each complete search
engine. All share one validated incumbent. Work stealing redistributes subtrees
within each engine, preferring local NUMA nodes. An atomic count includes every
queued or active task; an engine is exhausted only when that count reaches zero.
An active arc task also owns its explicit local frontier until it is empty.
Whichever engine completes first establishes the global proof and stops the other.
No worker is allowed to infer exhaustion merely from an empty queue.

### 4.6 Why `Optimal=yes` is justified, and its limits

Every feasible path is represented by the dummy-node reduction. Prefix branching
covers every legal continuation; conflict branching covers every feasible
assignment through an exhaustive disjunction. All lower bounds relax the original
problem, and dominance compares equivalent future subproblems. Pruning therefore
cannot remove a path cheaper than the incumbent. Once one complete search space
is exhausted, no cheaper feasible path exists. Matching the global lower bound
also proves optimality. Nonnegative costs prove a zero-cost incumbent optimal;
a unique topological order leaves only one feasible path.

This is an **algorithmic optimality proof**, not an exported independently
checkable proof log. The CSV and path alone cannot certify the search. The source,
reasoning above, and independent oracle tests make the implementation auditable.
No timeout run is promoted to optimal based on a known reference objective.

The design draws on assignment-based exact SOP and history-dominance research,
but is an original implementation, not a reproduction or benchmark-equivalent
version of any paper. It does not include LKH, advanced branch-and-cut polyhedral
separation, persistent restart files, or a formal proof-certificate checker.
Which engine is faster depends strongly on cost structure and precedence density.

References:

- Jamal et al., *Solving the sequential ordering problem using branch and bound*:
  https://iris.unimore.it/handle/11380/1177202
- *A parallel branch-and-bound algorithm with history-based domination and its
  application to the sequential ordering problem*:
  https://par.nsf.gov/servlets/purl/10379553
- *Combining a Parallel Branch-and-Bound Algorithm with a Strong Heuristic to
  Solve the Sequential Ordering Problem*:
  https://par.nsf.gov/servlets/purl/10463085
- Linux kernel NUMA documentation, including memoryless nodes:
  https://docs.kernel.org/mm/numa.html

## Files

- `src/sop.cpp`: complete solver source.
- `Makefile`: build, test, sanitizer, and clean targets.
- `tests/`: independent exhaustive and subset-DP correctness tests.
- `tools/verify.py`: independent CSV solution checker.
- `examples/`: the three supplied inputs, unchanged.
- `results/`: recorded development runs and validation notes.
- `LICENSE`: MIT license.

The MIT license covers the original solver code and documentation. Supplied example
instances retain their original provenance and any applicable rights.
