# SOP Exact — C++17 parallel solver

This project solves the Sequential Ordering Problem as an **exact, integer-only
optimization problem**. It accepts arbitrary start/end choices implied by the
precedence DAG, supports `1 <= N <= 1000`, and reports `Optimal=yes` only after
a valid optimality proof finishes before the deadline. It is self-contained:
there is no commercial solver, downloaded dependency, or external service at run time.

The implementation combines published exact-search ideas with carefully specified
custom bounds and dominance rules. It is not a claim of being the fastest SOP
solver in the literature. SOP includes the asymmetric traveling-salesman path
problem; difficult instances can require exponential time. Supporting 1000 nodes
means the representation and algorithms accept that size, **not** that every
1000-node instance will finish in a practical time. See `DEVELOPMENT.md` for actual
tests, measured results, and untested hardware.

## 1. Requirements and installations required

Target: 64-bit Ubuntu/Linux, GCC 9 or newer (or a recent Clang), C++17, POSIX
threads, and GNU Make. Python 3.9+ is needed only for integration tests, independent
CSV verification, and the benchmark script. CMake 3.16+ is an optional build route.
`numactl` provides optional topology/placement diagnostics; the solver does not
link to libnuma. No network access is needed to build or run this source.

On Ubuntu:

```bash
sudo apt update
sudo apt install build-essential python3 unzip
# Optional build and hardware-inspection tools:
sudo apt install cmake numactl
```

Use a release build for measurements. `-march=native` is optional and produces a
binary intended for the machine on which it is built. The default build avoids
that assumption. Costs, duals, and objective comparisons use signed 64-bit
integers, with explicit overflow headroom; no floating-point tolerance can cause
an optimality claim. Floating point is used only for elapsed-time handling and
heuristic ranking, never for proof comparisons.

## 2. Exact commands to build and run with an example

From the directory containing the delivered archive:

```bash
unzip sop_exact_cpp17.zip
cd sop_exact
make -j32
make -j8 test
./build/sop_exact examples/prob.42.sop
```

The last command has no time limit and uses one worker. A practical run on the
requested 32-core Threadripper is:

```bash
./build/sop_exact examples/prob.42.sop \
  --threads 32 --time-limit 3600 --output results.csv
```

For long searches on the 122 GiB machine, a larger history table is worth testing:

```bash
./build/sop_exact examples/R.200.1000.1.sop \
  --threads 32 --time-limit 3600 --history-mb 16384 \
  --history-mode shared --output results.csv
```

`--history-mb` is a **total** budget, not a per-worker budget. It does not cap
all process memory. Try `--history-mode local` on the same input to compare the
benefit of cross-worker dominance against NUMA traffic. No particular setting
is guaranteed best on every instance.

Optional native build and CMake build:

```bash
make -j32 BUILD_DIR=build-native NATIVE=1
./build-native/sop_exact examples/rbg048a.sop --threads 32 --time-limit 60

cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release -DSOP_NATIVE=ON
cmake --build build-cmake -j32
ctest --test-dir build-cmake --output-on-failure
```

Build flags are captured when compiling an object. Use a different build directory
or clean before changing them. Tests keep their checks active in release builds.

Independent feasibility/cost check of recorded output:

```bash
python3 scripts/verify_csv.py examples/prob.42.sop results.csv
```

This script validates the permutation, all original precedences, and the objective.
It expects a header row and does **not** independently verify the solver's optimality proof.

Reproducible local benchmark runs, executed sequentially:

```bash
python3 scripts/benchmark.py examples/prob.42.sop examples/rbg048a.sop \
  examples/R.200.1000.1.sop --threads 1 16 32 --time-limit 60 \
  --history-mb 4096 --directory benchmarks/my_threadripper_run
```

The destination must be new. It retains each run's CSV, stderr log, command,
compiler identification, affinity mask, and input/binary SHA-256 hashes.

All options:

| Option | Default | Meaning |
| --- | --- | --- |
| positional file | required | Exactly one `.sop` input; `--` ends option parsing. |
| `--time-limit SECONDS` | unlimited | Finite nonnegative wall-clock budget; fractional values accepted. **0 means immediate timeout.** |
| `--threads N` | 1 | 1–1024 worker threads; 32 is the intended starting point for 32 physical cores. |
| `--output FILE.csv` | none | Also append the result to this file. Stdout remains enabled. |
| `--no-header` | off | Suppress headers on both destinations. |
| `--history-mb MB` | 256 | Total bounded history allocation budget; 0 disables history. Rounded down by table/shard sizing. |
| `--history-mode shared/local` | shared | Cross-worker sharded history or independent local histories. |
| `--tail-dp N` | 12 | Solve residual suffixes of at most N nodes by exact DP; 0 disables; maximum 20. |
| `--heuristic-seconds S` | automatic | Initial heuristic budget, additionally capped at 5% of remaining global time. 0 disables the initial portfolio, not baseline construction or occasional search probes. |
| `--affinity auto/none` | auto | Linux topology-aware worker pinning, or leave placement to the OS/external tools. |
| `--component-bound on/off` | on | Enable the extra assignment-component connectivity bounds. |
| `--seed N` | 1 | Unsigned 64-bit heuristic seed. Parallel interleavings and time-based budgets remain nondeterministic. |
| `--help`, `--version` | — | Used alone, display help or version. |

Exit codes: 0 for a completed result row, including timeout/interruption or a
well-formed infeasible instance; 2 for invalid arguments/input; 3 for runtime or
output failures. A failed additional file write can occur after a valid stdout
row has already been emitted. SIGINT and SIGTERM request cooperative cancellation
and a final incumbent row. SIGKILL cannot be handled.

## 3. Input and output format descriptions

### Input

Header keywords are parsed as metadata, case-insensitively. `NAME`, `TYPE`,
`COMMENT`, `DIMENSION`, `EDGE_WEIGHT_TYPE`, and `EDGE_WEIGHT_FORMAT` do not
contribute matrix numbers. `COMMENT` is unrestricted text. `DIMENSION` is required;
when present, type/weight/format must be `SOP`, `EXPLICIT`, `FULL_MATRIX`.
Unknown header keywords are ignored. Conflicting recognized duplicate headers
are errors. Spaces around the colon, CRLF, and an initial UTF-8 BOM are supported.

`EDGE_WEIGHT_SECTION` occupies its own line. It is followed by the repeated
dimension, then **exactly N × N whitespace-separated integers**, regardless of
line wrapping. An optional `EOF` token and trailing whitespace are accepted.
Missing values, extra values, mismatched repeated dimension, invalid integers,
nonzero diagonal, and unsupported dimensions are rejected.

```text
NAME: multiple-endpoint-example
TYPE: SOP
DIMENSION: 3
EDGE_WEIGHT_TYPE: EXPLICIT
EDGE_WEIGHT_FORMAT: FULL_MATRIX
EDGE_WEIGHT_SECTION
3
0 1 99
99 0 99
1 99 0
EOF
```

Its optimal path is `2 0 1`, cost 2. There are no fixed endpoints in that example.

For any off-diagonal `T[i][j] < 0`, **j must precede i**. Every negative signed
64-bit integer is a precedence marker; its magnitude is irrelevant. Nonnegative
entries are costs, including values such as 10,000,000. No distance symmetry,
triangle inequality, or hardcoded first/last index is assumed. Cyclic precedence
is reported as infeasible. An acyclic input with these complete-matrix semantics
always has a feasible topological Hamiltonian path.

To keep dual updates and bound sums safely inside int64, a nonnegative cost must
not exceed `floor(INF / (16 * (N + 1)^2))`, where `INF = floor(INT64_MAX / 4)`.
Values above that limit are rejected explicitly rather than rounded or saturated
into feasible costs. This still permits edge costs above 143 billion at N=1000.

The included `rbg048a.sop` has **50** nodes: the `DIMENSION` field, not the digits
in the filename, is authoritative. The three supplied inputs are included in
`examples/`; their contents were not modified.

### Output

```csv
Instance,Instance Size,Time,Enumerated Nodes,Best Cost,Best Path,Optimal
```

- `Instance`: basename of the input file, CSV-escaped when needed.
- `Instance Size`: matrix dimension.
- `Time`: elapsed steady-clock seconds from just before opening the instance
  through preprocessing, heuristics, search/proof, worker joins, and final
  validation. CSV serialization and output I/O follow this timestamp.
- `Enumerated Nodes`: sum across workers of entered B&B states plus newly computed
  non-root suffix-DP states. The tail root is already a B&B state and is counted
  once. Memo hits, heuristic moves, assignment augmentations, and candidate
  bounds rejected before child entry are not enumerations. A direct root proof
  counts one state; a pre-search timeout or cycle detection counts zero.
- `Best Cost`: independently recomputed path cost, or `-1` when none was found.
- `Best Path`: all original 0-based indices in order, separated by spaces; empty
  if no feasible path was found.
- `Optimal`: `yes` only for a feasible solution with a completed proof before the
  deadline; otherwise `no`. Proven infeasibility uses `-1`, empty path, `no`,
  with its distinct explanation on stderr.

Stdout receives a header on every run unless `--no-header` is set. A named output
file is appended, with a header only when new/empty and headers enabled. On POSIX,
an advisory file lock protects each header/row append among cooperating solver
processes. Output may not alias the input, including via hard links. Diagnostics,
every incumbent improvement with its new cost, and per-worker counters go to stderr.

Deadlines are cooperative: parsing is completed to validate the instance, inner
loops poll cancellation, and worker cleanup/final validation take a little time.
Consequently `Time` can slightly exceed the requested budget. Once timeout or
interruption is recorded, an unfinished search cannot publish `Optimal=yes`.
A proof already finished before the deadline remains valid if cleanup extends
beyond it. A huge input or memory allocation can cause a larger overrun; this is
not a hard real-time program.

## 4. Names, descriptions, and reasoning for algorithmic choices

The exact core is **prefix branch-and-bound with history dominance**, influenced
by the SOP work of Shobaki and Jamal (2015) and Gonggiatgul, Shobaki, and
Muyan-Özçelik (2023). This is an original implementation, not their code or a
reproduction of their complete algorithms. The full technique inventory,
classifications, references, and correctness arguments are in `ALGORITHMS.md`.

The input DAG is closed transitively using bitsets. Impossible consecutive arcs
are removed. Cost/precedence-identical vertices are ordered canonically. A dummy
node converts every allowed source-to-sink Hamiltonian path into a cycle at zero
additional cost; choosing endpoints remains part of the optimization.

The lower bound is an **integer Hungarian assignment relaxation**. A child fixes
one outgoing arc and removes its row/column. Reusing the matching and duals means
at most one augmenting path is required for that deletion. Another augmentation
may be required to restrict the new prefix endpoint to ready successors.
Reversible, once-per-field undo records avoid matrix copies at each DFS node.
Reduced costs cheaply reject branches. Assignment-component incoming/outgoing
minima and a reduced-cost MST supply further valid connectivity bounds.

Upper bounds come from topological greedy construction, randomized restarts,
precedence-safe block exchanges (full adjacent-block exchanges through N=128,
Or-opt lengths 1–3 above that), and iterated local search. A feasible assignment
completion can close an entire subtree exactly. Remaining suffixes of up to 12
vertices use **Held–Karp subset dynamic programming** by default. Heuristics
influence speed and incumbent quality; they never justify an optimality claim.

Exact-key history retains the least prefix cost for `(visited set, last vertex)`.
Hash collisions are checked against all key words. A bounded four-way table may
evict entries safely. A short-window interchange rule removes prefixes with a
cheaper or canonical equal-cost reorder of their final internal vertices. It is
applied before history insertion to prevent a discarded prefix from suppressing
its own replacement.

Workers use depth-first search locally and steal queued subtrees, preferring the
same NUMA node. Their mutable search state, graph replicas, matching scratch,
and local history are allocated after pinning. Shared history is sharded and
first-touched by its owning workers. An atomic incumbent cost enables fast
pruning; a lock protects validated path publication. Outstanding-work accounting
includes executing and queued tasks, preventing an empty queue from being
mistaken for proof completion.

### Threadripper 2990WX and memory

The 2990WX has four dies, with two compute dies lacking directly attached DRAM
([Yarp, 2023](https://www2.eecs.berkeley.edu/Pubs/TechRpts/2023/Archive/EECS-2023-47.pdf)).
First touch cannot make all memory local to those compute dies. The solver uses
the process affinity mask and Linux sysfs topology rather than assuming CPU IDs,
four memory-bearing nodes, or a particular BIOS configuration. Physical cores
are selected across exposed NUMA nodes before SMT siblings. With missing sysfs
information, it falls back to allowed CPU IDs; failed pinning is logged.

Linux controls actual page placement and memoryless-node fallback. The code does
not force `mbind`, change system NUMA balancing, or override an existing process
memory policy. See the Linux kernel's
[NUMA memory policy](https://www.kernel.org/doc/html/latest/admin-guide/mm/numa_memory_policy.html)
and [memoryless-node discussion](https://www.kernel.org/doc/html/v5.0/vm/numa.html).
Inspect the actual machine before comparing configurations:

```bash
lscpu -e=CPU,CORE,SOCKET,NODE
numactl --hardware
# While a solver is running, substitute its process ID:
numastat -p PID
```

Start with 32 workers and automatic affinity. Compare shared versus local history
at the same memory budget. If an external `taskset`/`numactl` policy manages CPUs,
use `--affinity none`. More workers do not guarantee linear speedup; initial
parsing/root assignment are serial, search trees can be narrow, and shared-memory
traffic or different incumbent discovery orders can dominate.

Static input/graph storage is O(N²). Each worker has an O(N²) graph replica and
O(N²) worst-case undo storage along a DFS path, plus its DP scratch. With the
default tail threshold 12, DP tables occupy about 0.46 MiB per active call;
threshold 20 can require about 189 MiB **per worker**, so increase it cautiously.
The history budget is additional. No all-subsets table is allocated for N=1000.

### Architecture and proof boundaries

| Module | Responsibility |
| --- | --- |
| `instance` | Strict parser, original-matrix validator, DAG/closure/symmetry/arc preprocessing. |
| `assignment` | Primal/dual matching, reversible restrictions, connectivity bounds, exact matching completions. |
| `heuristics` | Feasible constructions/local search and validated incumbent publication. |
| `history`, `dominance` | Exact-key prefix dominance and bounded-window interchange dominance. |
| `tail_dp` | Exact suffix recurrence and reconstruction. |
| `parallel` | Topology, pinning, synchronized work stealing, and outstanding-task accounting. |
| `solver` | Search orchestration, branching, pruning, portfolios, and final status. |
| `common`, `cli`, `main` | Integer/time utilities, cancellation, option validation, CSV, and executable entry. |

Proof means exhaustive exact search with admissible bounds and safe dominance,
or an exact root certificate/DP result. It is not merely matching a known answer,
finding the same path repeatedly, or satisfying a heuristic stopping criterion.
This release does not emit a portable proof trace for an independent proof
checker. The mathematical argument and tests are supplied, while an optimality
claim still relies on correct execution of this implementation.

No branch-and-cut/MIP backend, ant colony system, full Lin–Kernighan search,
checkpoint/resume mechanism, or paper-specific thread-restart scheme is claimed
or implemented. `DEVELOPMENT.md` explains these choices and the actual validation
performed. Inputs in `examples/` were supplied by the user; no ownership or
redistribution rights beyond that contribution are asserted.
