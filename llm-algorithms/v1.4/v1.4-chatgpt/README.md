# SOP Exact — parallel C++17 Sequential Ordering Problem solver

A self-contained exact solver for the specified TSPLIB-style SOP format, for
**1–1,000 nodes**, with a modular C++17 implementation, bounded history cache,
NUMA-conscious worker placement, validated incumbents, and honest timeout status.

The default is **one worker, no time limit, CSV on stdout, with a header**.
There are no commercial solvers, downloaded runtime packages, or optimization
libraries. GCC 9 is a supported build target; GCC 11 is not required.

This is an exact branch-and-bound solver: with sufficient time and memory it
finishes the proof, and `Optimal=yes` is only emitted after a proof completes.
SOP is NP-hard. A 1,000-node input capacity does **not** promise that arbitrary
1,000-node instances, or even every 50-node instance, can be solved quickly.
No universal fastest-solver or benchmark state-of-the-art claim is made.

## Section 1: Requirements and installations required

Required on Ubuntu:

- A 64-bit C++17 compiler: **g++ 9 or later**.
- GNU Make and the normal C/C++ development tools.
- POSIX threads, supplied by the normal Linux toolchain.

Optional:

- Python 3.8+ for the independent correctness tests and benchmark driver.
- CMake 3.13+ if you prefer CMake to Make.
- `numactl` for inspecting and experimenting with NUMA memory policies.

If these are already installed, nothing else is necessary. On a machine where
you have installation privileges:

```bash
sudo apt-get update
sudo apt-get install build-essential python3
# Optional tools:
sudo apt-get install cmake numactl
```

Check the compiler you already have:

```bash
g++ --version
```

The source deliberately avoids C++20 features, `std::filesystem` linker
dependencies, OpenMP, `libnuma`, and CPU-specific intrinsics. The release build
is tested with GCC 13.3 in this environment; GCC 9 is not installed here, so a
GCC 9 build has not been executed here. All required language/library features
are available in GCC 9's C++17 implementation.

## Section 2: Exact commands to build and run with an example

### Build

Extract the archive and enter its directory:

```bash
unzip sop_exact.zip
cd sop_exact
make -j32
```

If your compiler is explicitly named `g++-9`:

```bash
make -j32 CXX=g++-9
```

To optimize for the CPU on which you are compiling, build **on your Threadripper**:

```bash
make clean
make -j32 CXX=g++-9 CXXFLAGS='-O3 -DNDEBUG -march=native'
```

Do not build with `-march=native` on another CPU and assume that executable will
run on the 2990WX. The ordinary build is portable across compatible x86-64 systems.
Run `make clean` when changing compiler or optimization/sanitizer flags.

Optional CMake build:

```bash
cmake -S . -B cmake-build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-9
cmake --build cmake-build -j32
./cmake-build/sop_solver --help
```

### Run

The executable accepts options before or after the single input filename:

```bash
./sop_solver examples/prob.42.sop

./sop_solver examples/R.200.1000.1.sop \
  --threads 32 --time-limit 3600 \
  --history-mb 32768 --output results.csv

./sop_solver examples/rbg048a.sop \
  --threads 32 --time-limit 300 --output results.csv --no-header
```

The second command budgets **32 GiB for the history cache**, leaving substantial
room in your 122 GiB system for workers and the operating system. This is a cache
budget, not a hard cap on total process memory. Its actual capacity is rounded
down to the supported table size and pages are allocated on demand. Start with
the default 256 MiB or a smaller explicit budget if the machine is shared.

Logs go to stderr. To capture them separately:

```bash
./sop_solver examples/prob.42.sop --threads 32 --time-limit 600 \
  --output results.csv > current-run.csv 2> current-run.log
```

Use `Ctrl+C` or `SIGTERM` to stop a long run gracefully and receive its best
validated path. Cancellation cannot create an optimality proof. `SIGKILL`, a
power failure, or an OS out-of-memory kill cannot be handled gracefully.

### Options

| Option | Default | Meaning |
|---|---|---|
| `--time-limit SECONDS` | Unlimited | Nonnegative wall-clock seconds; zero immediately times out. Fractions are supported. |
| `--threads N` | `1` | Total worker count, including any dedicated heuristic worker. |
| `--output PATH` | None | Append to this CSV **and** print the current run to stdout. |
| `--no-header` | Off | Suppress the header on both destinations. |
| `--history-mb N` | `256` | Total history table budget in MiB; `0` disables history pruning. |
| `--heuristic-seconds S` | `0.5` | Initial improvement budget per worker. Capped at 10% of time remaining at worker startup. `0` disables improvement heuristics, but retains the fast initial feasible path. |
| `--connectivity auto\|off\|always` | `auto` | Extra component lower bound: root plus every eighth search depth with at least eight assignment rows; disabled; or every search state. |
| `--seed N` | `1` | Unsigned 64-bit heuristic seed. |
| `--pin auto\|off` | `auto` | Discover allowed CPUs and pin workers, or let the OS place them. |
| `--help` / `--version` | — | Print usage/version and exit. |

For `N > 16`, at least four requested workers, and a positive heuristic budget,
one worker continues iterated local search while the others perform exact
search. Thus `--threads 32` normally means **31 search workers plus one heuristic
worker**. With `--heuristic-seconds 0`, all 32 workers perform exact search.
Small instances skip local improvement. Parallel runs can differ in search
order, node counts, incumbent paths, and timeout quality despite a fixed seed.

### Threadripper 2990WX / NUMA

Start with **32 workers**, one per physical core, rather than assuming 64 SMT
threads will be faster. Test 16, 32 and 64 on your workload if desired. Speedup
depends on memory traffic, cache contention, available parallel branches, and
the order in which workers discover improved solutions.

The 2990WX has an asymmetric four-die topology: not every compute die has direct
DRAM access. The code does not assume that Linux node numbers or CPU numbers
have a particular mapping. It:

- Respects the process's allowed CPU affinity mask, including scheduler/cpuset restrictions.
- Reads core, socket and NUMA identifiers from Linux sysfs.
- Spreads workers across exposed nodes, using one hardware thread per physical
  core before assigning SMT siblings.
- Allocates each worker's private instance copy and search state after binding,
  giving Linux a useful first-touch placement policy.
- Steals tasks from the same NUMA node first, then other nodes.
- Shares only small incumbent data, task transfers, and a sharded history cache
  on the normal search path. Matrix access and matching updates are worker-local.

Memoryless nodes are left to Linux's nearest-memory fallback policy. The solver
does not hard-bind allocations to a node that might have no memory. First-touch
placement is a policy aid, not a guarantee of local DRAM, and shared history can
still generate remote traffic.

Inspect the actual machine:

```bash
lscpu -e=CPU,NODE,SOCKET,CORE,ONLINE
numactl --hardware
```

Compare the default policy with interleaving if memory traffic limits scaling:

```bash
numactl --interleave=all ./sop_solver examples/R.200.1000.1.sop \
  --threads 32 --time-limit 3600 --history-mb 32768 --output interleaved.csv
```

Interleaving also affects the worker copies, so it is not unconditionally better.
For externally managed placement, use `--pin off`. Affinity failures produce a
stderr diagnostic and fall back to unpinned execution. No particular Linux
governor, BIOS change, or privileged system modification is required.

### Tests and benchmark commands

```bash
make -j32 test
python3 tests/test_solver.py ./sop_solver --cases 1000
python3 tests/benchmark.py ./sop_solver --threads 32 --time-limit 60 \
  --history-mb 1024 --output-dir benchmark-results
```

The normal tests compare small SOPs with independent exhaustive enumeration,
and compare every tested warm matching with a separate subset-DP assignment
oracle. They also test the stronger lower bound against exhaustive completions,
large costs, shuffled endpoints, deadlines, malformed input, signals, 32 workers,
concurrent CSV appends, and 1,000-node parsing and deep recursion.

For AddressSanitizer and UndefinedBehaviorSanitizer:

```bash
make sanitize
```

If LeakSanitizer cannot run in your container's process namespace, use:

```bash
ASAN_OPTIONS=detect_leaks=0 make sanitize
```

This still checks address safety and undefined behavior, but not leaks.
See `docs/VALIDATION.md` for the checks actually executed and measured results.

## Section 3: Input and output format descriptions

### Input

The parser uses header keywords rather than positional header lines. `COMMENT`
is free text. `DIMENSION` and `EDGE_WEIGHT_SECTION` are required. When present,
`TYPE`, `EDGE_WEIGHT_TYPE`, and `EDGE_WEIGHT_FORMAT` must be `SOP`, `EXPLICIT`,
and `FULL_MATRIX`. Missing optional metadata and unknown metadata keywords are
tolerated. Keywords are case-sensitive. Conflicting duplicate known headers
are rejected. Leading/trailing whitespace, CRLF and a UTF-8 BOM are accepted.

After an `EDGE_WEIGHT_SECTION` line, the next integer must repeat `N`. Exactly
`N*N` signed integer values follow, independent of line breaks. An `EOF` token
and trailing whitespace are optional. Truncation, extra matrix data, a wrong
repeated dimension, non-integer values, or a nonzero diagonal are rejected.

```text
NAME: tiny.sop
TYPE: SOP
COMMENT: 0 before 1 before 2
DIMENSION: 3
EDGE_WEIGHT_TYPE: EXPLICIT
EDGE_WEIGHT_FORMAT: FULL_MATRIX
EDGE_WEIGHT_SECTION
3
0 5 10000000
-1 0 7
-1 -1 0
EOF
```

Its optimum is `0 1 2`, costing `12`. Nodes use matrix row/column indices,
starting at zero; the `NAME` metadata does not change indexing.

- `T[i][j] >= 0` is an actual directed edge cost, including zero and very large values.
- `T[i][j] < 0` means **j must appear before i**. Every negative value has this
  meaning; its magnitude is ignored.
- No symmetry, triangle inequality, fixed endpoint numbers, or known optimum
  is assumed.
- All precedence-minimal vertices are considered as possible starts, and all
  precedence-maximal vertices as possible ends.

Arithmetic uses signed 64-bit integers. An edge is rejected if it exceeds
`floor((INT64_MAX/4)/(16*(N+1)))`, preserving a large safety margin for costs and
dual arithmetic. Values outside signed 64-bit range are rejected. No costs are
silently rounded, saturated or treated as an infinity sentinel in the input.

### CSV output

```csv
Instance,Instance Size,Time,Enumerated Nodes,Best Cost,Best Path,Optimal
tiny.sop,3,0.000123,1,12,0 1 2,yes
```

The time above is illustrative, not a benchmark.

| Column | Definition |
|---|---|
| Instance | Input **filename**, not the `NAME` header; RFC-style quoting protects commas and quotes. |
| Instance Size | Header dimension `N`. |
| Time | Steady-clock wall seconds from immediately before opening the input through parsing, preprocessing, heuristic work, exact search, worker shutdown, and final validation. CSV serialization/output I/O is excluded. |
| Enumerated Nodes | One for the attempted root assignment, plus one for each child subproblem whose incremental assignment update starts. Includes bound-pruned and infeasible children. Each delegated child is counted once by the worker that computes it. Per-worker counts are summed. Heuristic iterations, history probes, candidates discarded by the parent's reduced-cost test, and matching augmentations are not extra nodes. Zero is possible if processing ends before the root. |
| Best Cost | Independently recomputed path cost, or `-1` if no feasible path was found. |
| Best Path | Space-separated, 0-based vertex indices, or empty if none was found. |
| Optimal | `yes` only after completing an exact proof before the deadline/cancellation; otherwise `no`. |

A timeout with no feasible path therefore has `Best Cost=-1`, an empty `Best
Path`, and `Optimal=no`. A cyclic precedence graph is infeasible: it uses those
same CSV values and an explicit `[infeasible]` stderr message. `Optimal=yes`
always refers to an actual feasible optimal path, not an infeasibility proof.

The time budget includes input and setup. Input parsing finishes sufficiently
to diagnose malformed files and obtain `N`; search and preprocessing poll the
deadline cooperatively. Shutdown and final validation can make reported wall
time slightly exceed the requested limit. This is not a hard real-time process
killer. A proof completed before the limit remains valid if cleanup takes the
reported total a little past it. The proof completion time is logged separately.

Every improved incumbent logs `[incumbent] cost=... time=...s` to stderr.
Before publication and again before output, the program checks the entire
permutation, every original precedence constraint, and the sum of original
matrix costs. Only validated paths can reduce the search's upper bound.

Without `--no-header`, stdout always receives one header and one row. An output
file receives a header only when new or empty; subsequent runs append. On Ubuntu,
`flock` protects the emptiness check and append against concurrent cooperating
solver processes. A missing terminal newline is repaired before appending.
The input file cannot be used as the output file, including through a hard link.

Exit status `0` means a valid instance was processed and a CSV result written,
including timeout, cancellation or infeasibility. Exit status `1` means invalid
arguments/input, I/O failure, allocation/thread failure, or an internal validity
error. Fatal errors go to stderr and do not produce a misleading result row.

## Section 4: Algorithmic choices and reasoning

### Exact prefix branch-and-bound with an incremental assignment relaxation

The search builds a topological Hamiltonian path one vertex at a time. It
branches over every currently eligible vertex. A precedence closure removes
backward arcs and direct jumps over mandatory intermediate vertices before
search. This also handles endpoint inference without hardcoded indices.

A zero-cost dummy vertex represents the open path. It connects to all possible
sources, and all possible sinks connect back to it. At a search prefix ending
at `last` with unvisited set `U`, the residual assignment has rows
`{last} union U` and columns `U union {dummy}`. Every feasible completion is a
perfect matching, whereas a matching may contain disconnected cycles. Its
minimum cost is consequently an admissible lower bound.

An integral shortest-augmenting-path Hungarian implementation solves the root
relaxation. Appending a vertex deletes one row and one column. The previous
matching and dual variables remain useful: at most two augmentations repair the
matching, including the new frontier's eligibility restrictions. Root work is
O(N^3) in the worst case; a child update is O(N^2), rather than solving an entire
assignment from scratch at every search node.

Feasible dual potentials also produce **reduced-cost pruning** and branch
ordering before child allocation/repair. Search explores lower estimated cost
first while donating alternatives to idle workers.

### Reduced-cost component bound

A residual matching consists of a frontier-to-dummy path and zero or more
cycles. Every actual completion must connect these components. The solver
computes three admissible penalties in reduced-cost space: the sum of cheapest
cycle exits, the sum of cheapest cycle entrances, and an undirected minimum
spanning tree connecting the components. It adds **the maximum** of these
penalties to the assignment bound; adding them together would double-count
costs. This is a bound on connecting relaxation components, not an assertion
that an MST solves SOP.

This O(N^2) extra work can reduce the tree but can also slow cheap nodes. The
default samples it every eighth depth and applies it at the root. `always` and
`off` support workload-specific comparisons. Candidate reduced-cost bounds
use the original assignment value, avoiding double-counting the strengthened
penalty.

### Exact history domination with bounded memory

The relevant future state is `(visited set, last vertex)`. For equal states,
a cheaper or equal prefix dominates a more expensive prefix: the available
suffixes are identical. A shared, 256-shard, four-way associative cache records
the best prefix cost for each state.

Every cache hit compares the full bitset and last vertex, not just a hash.
Collisions, replacement, disabled history, or failed cache allocations can only
lose pruning opportunities. They cannot discard a unique solution. Mutexes
serialize comparisons and updates within each shard. The cache may retain an
in-progress representative safely because representatives are explored normally;
workers do not abandon them for search restarts. The correctness argument is
in `docs/CORRECTNESS.md`.

### Feasible incumbents: greedy construction and iterated local search

A fast eligible-vertex greedy construction establishes a feasible upper bound.
Randomized greedy starts and precedence-preserving block relocation improve it.
Local neighborhoods use block lengths 1, 2, 3, 4, 8 and 16. Random block kicks
escape local minima. The heuristic periodically imports a better shared
incumbent discovered by the exact search. The dedicated heuristic worker, when
enabled, publishes improvements throughout the proof search.

These heuristics **do not establish optimality**. They help the exact algorithm
prune by lowering the cost of a checked feasible solution. A poor heuristic
result affects speed, not correctness.

### Parallel execution and proof completion

Workers perform depth-first search on local state, share a monotonic upper
bound, and donate alternative subproblems to bounded per-worker deques. Idle
workers steal from the front; owners take from the back. Work accounting covers
both queued and actively explored tasks. Donation increments that accounting
before exposing work, so a transient empty queue cannot falsely prove completion.

Optimality is established either by a validated incumbent equaling the global
root lower bound, or by exhaustion of every branch that could beat the incumbent.
Deadline expiration, interrupts, and internal failures cannot create a proof.
This is an algorithmic exact-search proof; the project does not export a separate
machine-checkable proof certificate. The standalone validator independently
checks feasibility and cost, not the entire optimality proof.

### Architecture and memory

| File/module | Responsibility |
|---|---|
| `src/instance.cpp` | Keyword parsing, precedence preprocessing, independent path validation. |
| `src/assignment.cpp` | Exact assignment relaxation, warm repair, duals, component bound. |
| `src/history.cpp` | Bounded, full-key, synchronized domination cache. |
| `src/heuristic.cpp` | Feasible construction, local improvement, perturbation. |
| `src/topology.cpp` | Allowed CPU/core/NUMA discovery and optional thread affinity. |
| `src/solver.cpp` | Incumbent ownership, DFS, task donation/stealing, proof completion. |
| `src/cli.cpp` | Argument validation, CSV formatting and locked append. |
| `src/main.cpp` | Run lifetime, cancellation handlers, final checks and errors. |
| `include/sop/` | Module interfaces and shared types. |
| `tests/` | Independent oracles, regression tests and benchmark driver. |

There is no exponential preallocated `2^N` DP table. Worker matrices and the
maximum DFS matching stack consume O(N^2) space per worker; the history cache
has its own bounded budget. A 1,000-node worker typically needs tens of MiB plus
allocator overhead, while history is likely the largest configurable expense.
No checkpoint/restart file is implemented. Save stdout/CSV and stderr logs for
each timed run.

### Research context

The design follows the established exact SOP approach of assignment bounds,
history domination and parallel branch-and-bound, with a cooperating heuristic.
This is an original implementation, not the authors' software or a reproduction
of all their algorithms. In particular, it does not implement their specialized
thread-restart/resume mechanisms or ACS heuristic. Relevant primary references:

1. Gonggiatgul, Shobaki and Muyan-Özçelik, *A parallel branch-and-bound algorithm
   with history-based domination and its application to the sequential ordering
   problem*, JPDC 172 (2023), 131–143.
   <https://doi.org/10.1016/j.jpdc.2022.10.007>
2. Shobaki and Normington, *Combining a Parallel Branch-and-Bound Algorithm with
   a Strong Heuristic to Solve the Sequential Ordering Problem*, ICPP Workshops
   (2023). <https://doi.org/10.1145/3605731.3608929>
3. Linux kernel NUMA and memory-policy documentation:
   <https://docs.kernel.org/mm/numa.html> and
   <https://docs.kernel.org/admin-guide/mm/numa_memory_policy.html>.
4. AMD, *Ryzen Processor Software Optimization*, GDC 2019:
   <https://gpuopen.com/gdc-presentations/2019/gdc-2019-s2-amd-ryzen-processor-software-optimization.pdf>.

The three example instances are the user-supplied data, copied without matrix
changes. The generated solver source is provided under `LICENSE`; that license
does not relicense the supplied benchmark data.
