# Development record

This is the record of the project delivered in this archive, not of previous
solver projects or papers. Measurements below were produced by running this
implementation. Algorithmic reasoning and untested configurations are identified
separately. Development spanned November 13–14, 2026 (Pacific time).

## Initial plan and alternatives considered

The initial plan was to build a dependency-free C++17 exact solver with a small
auditable core, then test each proof-sensitive mechanism independently before
running supplied instances. The planned modules separated original input and
validation, precedence preprocessing, matching bounds, heuristics, dominance,
parallel scheduling, exact suffix DP, and command-line/output behavior.

| Alternative | Decision and reason |
| --- | --- |
| Full Held–Karp subset DP | Dropped as the general solver: O(N·2^N) storage/time is incompatible with N=1000. Kept for bounded residual tails, and as an independent small-instance test oracle. |
| Plain DFS enumeration / plain B&B | Kept as the completeness framework, but dropped as the only performance technique: too many equivalent prefixes and weak pruning. |
| B&B with only a heuristic upper bound | Dropped as the final design: a good path alone does not make proof efficient. Retained feasible greedy/local-search paths as incumbents. |
| Independent row/column-minimum bounds | Retained as initialization inside the assignment algorithm, not as the sole search bound. They ignore competition for destinations. |
| Fresh Hungarian solve at every node | Dropped before implementation. Reusing a matching after one row/column deletion needs at most one augmentation, with a second possible repair for frontier filtering. |
| Incremental assignment B&B plus history and suffix DP | Selected. It combines polynomial relaxations, memory-bounded state reuse, cheap DFS, and exact small-tail solving without solver-library dependencies. |
| MILP/branch-and-cut backend with subtour and precedence cuts | Considered but not implemented. It can be very competitive, especially with weak precedence, but adds a major solver dependency and a separate cut/callback correctness surface. This project does not claim to outperform it. |
| Dense all-subsets transposition table | Dropped. A bounded exact-key set-associative table supports arbitrary N and safely sacrifices pruning on eviction. |
| Hash-only dominance | Rejected on correctness grounds. Every match compares the complete visited bitset and last vertex. |
| One global best-first priority queue | Dropped in favor of local DFS and work stealing: it can retain a large frontier and centralize synchronization. Branch ordering still prefers promising candidates. |
| Lock-free scheduler | Not implemented. Mutex deques and explicit outstanding-task accounting are easier to audit; no scaling advantage is claimed without measurements. |
| Fixed start node 0 and fixed end node N−1 | Rejected. Source/sink sets and a zero-cost dummy model support all feasible endpoint choices. |
| Unconditionally interleave all memory / hardcoded NUMA node IDs | Rejected. The 2990WX includes memoryless dies, and Linux/BIOS topology exposure varies. Use observed topology, after-pinning first touch, graph replication, and local-first stealing. |
| Ant colony optimization / full Lin–Kernighan / paper-specific thread restarts | Considered but not implemented. The supplied feasible greedy, block exchange, and iterated-local-search portfolio keeps the proof boundary clear. |
| Specialized whole-instance two-chain DP | Implemented only as an independent test oracle, not a production algorithm. It is polynomial for that restricted class; the general solver makes no claim to beat such specialized algorithms. |

The references in `ALGORITHMS.md` motivated established techniques. The custom
component-bound composition, variable-endpoint model, numeric contract, window
rule, synchronization, and integration are described as this implementation's
choices rather than attributed wholesale to a paper.

## Changes during implementation and what triggered them

1. **Bound state became reversible rather than copied per DFS child.** Assignment
   snapshots are copied only for delegated tasks. Local descent stores undo
   records. A further audit added a transaction stamp so each field is logged
   once per child; repeated potential adjustments otherwise allow unnecessarily
   large undo trails. This was a design correction, not a measured crash.
2. **The assignment now restricts the current endpoint's outgoing row.** Initial
   timed trials exposed a significant gap between root relaxation and feasible
   paths. After a branch, only ready successors can immediately follow the prefix.
   Filtering that one row is safe and admits one extra augmenting-path repair.
3. **Local search was broadened for smaller instances.** The initial version only
   moved blocks of lengths 1–3. For N≤128, full order-preserving adjacent-block
   exchanges were added, plus feasible adjacent-swap perturbations. Larger inputs
   retain the quadratic short-block neighborhood. This improved some trial
   incumbents; there was no controlled broad benchmark establishing a universal gain.
4. **Exact twin reduction and bounded-window interchange dominance were added.**
   They remove label symmetry and locally dominated prefixes. The window rule is
   deliberately executed before history insertion: inserting a prefix and then
   discarding it in favor of an equal-cost replacement can create an incorrect
   circular suppression. That ordering risk was found by reasoning, not by
   observing a wrong optimum.
5. **The overflow contract was tightened after a numeric audit.** Objective sums
   are O(N·max_edge), but warm-start dual potentials can accumulate O(N²·max_edge)
   changes along a deep path. The final parser limits each cost to
   `INF / (16*(N+1)^2)`, with int64 arithmetic throughout. Near-limit costs,
   INT64_MIN precedence markers, and rejection above the cap are tested. The
   earlier linear-in-N cap was not retained.
6. **Whole-instance timing was finalized in `main` after `solve` returns.** This
   includes destruction of solver-owned state before CSV serialization. Actual
   output I/O is necessarily outside the timestamp written into that output.
7. **Testing crossed bitset word boundaries.** Full subset oracles cannot cover
   130 nodes, so an independent polynomial two-chain recurrence was added to
   verify exact answers at N=64, 65, and 130. A separate 1000-node two-chain timeout
   smoke test exercises the general search and worker-local representations.
8. **The heuristic slice now starts after worker initialization.** A large history
   allocation could otherwise consume the entire small warmup deadline. Workers
   share the barrier-release timestamp; global instance timing still includes
   all initialization. This was found during review of large-memory settings.
9. **Signal cancellation uses a lock-free atomic.** A final concurrency review
   replaced `volatile sig_atomic_t` with `atomic<sig_atomic_t>` and an always-lock-free
   static assertion. Volatile alone does not establish synchronization with other
   worker threads. The signal handler only performs a relaxed atomic store; this
   change was prompted by code review, not a reported sanitizer race.
10. **Publish the first topological incumbent before expensive reductions.** The
    validated order is now offered immediately after DAG sorting, so a later
    timeout in transitive closure or arc reduction retains a feasible solution
    already found. An explicit callback test checks this early publication.

## Bugs, failed assumptions, and environmental dead ends

- A malformed-input test initially tried to delete a text pattern that was absent
  from the fixture. The unchanged valid file correctly succeeded and the test
  failed. The fixture now removes the final token structurally and asserts that
  every invalid fixture differs from its source.
- A live-timeout test assumed a particular unconstrained 55-node random input
  would remain unsolved for 150 ms. Its assertion failed. Hardness is not a valid
  timing contract, so the fixture was changed to a constrained 120-node case;
  exact zero-deadline behavior and signal cancellation are tested separately.
  This failure was not treated as evidence of an incorrect solver optimum.
- Numeric parsing was audited for multiple signs: `+-0` could be accepted after
  stripping a leading plus. The parser now rejects doubled/mixed signs, with a
  regression case.
- A clean GCC build diagnosed the general `std::sort` implementation's unrolled
  insertion-sort path on the fixed two/three-element dominance window. The
  window ordering now uses explicit compare/exchange operations. The bounds are
  evident in the code, and the final clean build is checked for warnings.
- LeakSanitizer aborted with its documented ptrace/environment restriction.
  AddressSanitizer and UndefinedBehaviorSanitizer were rerun with leak detection
  disabled and `halt_on_error=1`. **Leak checking was not completed.**
- `lscpu` could not inspect the environment's missing CPU sysfs topology. Logs
  therefore show node `-1` and the allowed CPU-ID fallback. Real NUMA placement
  on the Threadripper was not validated here.
- CMake was unavailable in the development environment. The CMake configuration
  is supplied, but its commands were not executed here. GNU Make was the tested
  build route.
- One intermediate benchmark batch was superseded after the executable was
  rebuilt between its run launches. It is not included as a source-consistent
  batch. The retained delivery batch was run after freezing the executable.

No algorithmic correctness failure was observed in the executed oracle suites.
That statement is limited to the tests described below; it is not a formal
verification of every possible execution.

## What was actually compiled and run

Environment: Linux x86-64, reported kernel 6.18.57, glibc 2.41, GCC 14.2.0;
process affinity exposed nine logical CPUs (IDs 0–8). This was **not** the user's
2990WX. Release builds used `-std=c++17 -O3 -DNDEBUG -pthread` plus warnings,
without `-march=native`. The final clean build has no compiler warnings; the
earlier small-array sorting diagnostic is described above.

The retained test logs are under `benchmarks/validation/`. Counts below are suite
sizes, not claims of that many statistically independent production workloads:

- Core C++ tests: 160 random DAG instances, 1543 incremental restrictions,
  exhaustive residual assignment and raw-matrix SOP comparisons, primal/dual
  invariant checks, exact rollback checks, component-bound admissibility,
  suffix-DP reconstruction, deliberately colliding history hashes, and 80
  feasible local-search checks.
- Black-box tests: the standard 180-case random suite and an expanded 600-case
  suite were run against an independent raw-matrix subset-DP oracle. Modes varied
  1/4 threads, DP on/off, component bounds on/off, zero/bounded history, and
  shared/local history. The later release suite additionally checked exact
  two-chain answers at 64, 65, and 130 nodes.
- Contract checks: wrapped/CRLF/EOF-less matrices, numeric comments, quoted
  filenames, free endpoints, cycles, malformed inputs, new/empty/existing CSVs,
  `--no-header`, eight concurrent appenders, input/output aliases, time zero,
  live timeout, SIGINT, large integer costs, the final numeric cap, and negative
  markers including INT64_MIN.
- Size/concurrency checks: a shuffled unique 1000-node chain was solved exactly;
  a non-unique two-chain 1000-node input was run for three seconds with four
  workers and its returned path independently checked; a 32-worker live-timeout
  smoke test was executed. Running 32 workers on nine visible CPUs is an
  oversubscription test, **not a 32-core speedup measurement**.
- Sanitizers: address/undefined-behavior core and 80-case black-box suites passed
  with leak detection disabled; a four-worker ThreadSanitizer smoke run on
  `prob.42.sop` reported no races. These are bounded checks, not proof of race
  freedom. A further one-second four-worker ThreadSanitizer run passed on the
  final source, including the atomic signal flag and early-incumbent changes.
- All three supplied examples were run. Returned paths in the recorded release
  results were also passed through `scripts/verify_csv.py` independently.

Representative executed commands:

```bash
make -j4 test
python3 tests/integration.py --solver build/sop_exact --cases 600
python3 tests/large_smoke.py --solver build/sop_exact

make -j4 BUILD_DIR=build-asan SANITIZE=address,undefined \
  build-asan/core_tests build-asan/sop_exact
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ./build-asan/core_tests
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  python3 tests/integration.py --solver build-asan/sop_exact --cases 80

make -j4 BUILD_DIR=build-tsan SANITIZE=thread build-tsan/sop_exact
./build-tsan/sop_exact examples/prob.42.sop --threads 4 \
  --time-limit 0.5 --history-mb 4
```

## Measured example results

The directory `benchmarks/verified/` records the final-source benchmark batch.
Each run used four workers, a 60-second limit, a 256 MiB shared-history budget,
default heuristics/DP/bounds, and automatic affinity. Runs in the batch were
sequential. Its recorded binary hash matches `build-final/sop_exact`, the final
tested executable (binaries themselves are not distributed in the source archive).

| Input | N | Wall seconds (final-source 60-second batch) | Enumerated units | Best cost | Optimality proven |
| --- | ---: | ---: | ---: | ---: | --- |
| `prob.42.sop` | 42 | 60.023773 | 34619922 | 256 | no |
| `rbg048a.sop` | 50 | 60.017407 | 15091955 | 351 | no |
| `R.200.1000.1.sop` | 200 | 29.421626 | 879115 | 1404 | yes |

A longer final-source `prob.42.sop` run used the same defaults and four workers
with a **180-second** limit. It proved optimum **243** in **128.043492 seconds**,
processing **86581221** enumerated units. Its exact command, path, stderr, and
matching final-binary hash are in `benchmarks/verified_long/`. The initial
heuristic budget was the default; it was not increased for this run.

The completed pre-audit batch in `benchmarks/final/` used the same command-line
settings. It predates the final numeric/cancellation/early-incumbent adjustments.
The intermediate `benchmarks/delivery/` batch is also retained as development
history; it predates the final signal-atomic and early-incumbent changes. These
earlier snapshots are not substituted for final-source results:

| Input | N | Wall seconds (pre-audit batch) | Enumerated units | Best cost | Optimality proven |
| --- | ---: | ---: | ---: | ---: | --- |
| `prob.42.sop` | 42 | 44.300124 | 102306067 | 243 | yes |
| `rbg048a.sop` | 50 | 60.000198 | 58603388 | 351 | no |
| `R.200.1000.1.sop` | 200 | 8.780902 | 791398 | 1404 | yes |

Paths, incumbent histories, per-worker counters, commands, compiler/platform
information, and input/binary hashes are retained with the batches. The 50-node
cost is an upper bound from that run, not a claimed optimum. The root relaxation
in that run was 337; no stronger final global lower bound is reported.

Early development CSVs are also retained (`initial.csv`, `strengthened.csv`,
`development.csv`). For example, the first 15-second `prob.42` trial returned
256 without proof; the initial and strengthened `rbg048a` trials returned 352
and 351 without proof. Those runs used 64 MiB histories and sometimes overlapped
with build/test activity. They motivated changes; **they are not controlled
speedup measurements or ablation studies**.

The very large enumeration counts include DP states as defined in the README;
they are not counts of full Hamiltonian paths. Different thread schedules and
heuristic timing can change both costs seen before timeout and total search work.

## Reasoned properties and untested boundaries

The admissibility, dominance, symmetry, and termination arguments in
`ALGORITHMS.md` are mathematical reasoning about the code, supported by finite
tests; the program was not checked by a formal proof assistant. O(N²) layout/undo
and O(k·2^k) suffix-DP memory estimates are analytic estimates, not measured peak
resident memory.

Untested or not established:

- Runtime and scaling on the actual 32-core 2990WX, its memoryless NUMA nodes,
  BIOS modes, SMT=64 settings, and a 122 GiB memory budget.
- 16 GiB and larger history budgets; tuned shared-versus-local history speedups;
  real Linux page-placement/remote-memory measurements.
- CMake execution, Clang builds, other GCC versions, other operating systems,
  native-CPU builds, and fault injection for allocation/thread-creation failures.
- Exhaustive solution of arbitrary hard 1000-node inputs; all benchmark families;
  superiority to published SOP implementations or branch-and-cut solvers.
- LeakSanitizer in a compatible environment; long-duration ThreadSanitizer
  coverage; independent replayable proof certificates.
- Recovery after SIGKILL, power loss, disk-full mid-row, or checkpoint/resume.
  Advisory locks do not protect against programs that ignore the locks.

These limitations do not change the mathematical exactness criterion: successful
exhaustion, exact leaf coverage, or a tight root certificate before timeout is
required for `yes`. They do limit performance and production-hardening claims.
