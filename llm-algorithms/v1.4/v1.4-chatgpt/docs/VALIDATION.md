# Validation record

Executed on 2026-10-26 (America/Los_Angeles) in a Linux x86-64 container with
GCC 13.3.0. The host exposes nine logical CPUs on an Intel Xeon Platinum 8573C
and one NUMA node. This is **not** a Threadripper 2990WX measurement. The shared
container is not a controlled benchmark environment, and some other validation
work overlapped the start of the benchmark batch. Do not use these timings as
promised performance or as a comparison with published solvers.

## Correctness tests executed

`make -j8 test` passed on the final release source:

- **1,200 random assignment instances / 7,010 residual states:** warm matching
  values matched an independent subset-DP assignment oracle. Every checked
  matching was bijective, matched arcs were dual-tight, and all finite arcs
  satisfied dual feasibility. Component bounds never exceeded independently
  enumerated feasible completion optima.
- **180 independently generated SOPs**, each solved with one and four workers,
  matched an independent exhaustive topological-path oracle. Cases include
  unconstrained, sparsely constrained, dense, and totally ordered instances;
  shuffled labels; zero costs; 64-bit costs; and all three connectivity modes.
- **402 counted solver invocations** passed the combined oracle/CLI suite,
  plus the separate concurrent append and signal subprocesses. Tests include
  CSV escaping, append/header rules, no terminal newline, cyclic precedence,
  malformed/truncated matrices, missing EOF, time limits and invalid options.
- A **1,000-node reversed forced chain** was proven optimal with costs above
  32-bit range. A second 1,000-node instance forced at least 990 actual search
  nodes and deep recursion, with an independently known six-node-tail optimum.
- Larger 24-node instances with 4,096 feasible orders exercised the heuristic
  and worker configurations of 1, 4, and **32 requested threads**, with costs
  independently enumerated. These are correctness checks, not 32-core scaling
  measurements on a nine-CPU host.
- Eight processes appended concurrently to one CSV with exactly one header.
  SIGTERM after the first incumbent retained a feasible path and unproven status.

## Sanitizer checks executed

Build configuration for address/undefined-behavior checks:

```bash
g++ -Iinclude -std=c++17 -O1 -g -pthread \
  -fsanitize=address,undefined -fno-omit-frame-pointer src/*.cpp -o sop_solver_asan
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  python3 tests/test_solver.py ./sop_solver_asan --cases 60
```

Result: **60 random SOP cases / 162 counted executions**, including the larger
and CLI tests, passed without address or undefined-behavior reports.
LeakSanitizer could not inspect the container's process namespace and was
disabled after it reported that environment limitation. **Leak checking is not
claimed.**

ThreadSanitizer configuration:

```bash
g++ -Iinclude -std=c++17 -O1 -g -pthread -fsanitize=thread \
  src/*.cpp -o sop_solver_tsan
TSAN_OPTIONS=halt_on_error=1 \
  python3 tests/test_solver.py ./sop_solver_tsan --cases 20
```

Result: **20 random SOP cases / 82 counted executions**, including larger cases,
passed without a data-race report. Sanitizer builds covered the final search,
matching, history, heuristic and CSV implementations; the final release also
contains an additional early stop check before allocating worker state.
A separate three-second ThreadSanitizer run with 32 workers on the supplied
200-node instance also completed without a race report and returned an
independently validated timeout incumbent.

GCC 9 and the optional CMake build were not executable in this environment.
The project uses the GCC 9-supported C++17 subset, and the Make build has been
executed successfully. Multi-node NUMA performance and 122 GiB memory scaling
must be measured on the target system; they are not established by these tests.

## Supplied-instance benchmark

The final release was run with **eight total workers**, a **30-second limit per
instance**, default heuristics/connectivity, and a 256 MiB history budget:

```bash
python3 tests/benchmark.py ./sop_solver --threads 8 --time-limit 30 \
  --history-mb 256 --output-dir docs/benchmarks
```

| Instance | N | Wall seconds | Enumerated nodes | Best cost | Proven optimal |
|---|---:|---:|---:|---:|---|
| prob.42.sop | 42 | 30.024365 | 27,267,761 | 246 | No — time limit |
| rbg048a.sop | 50 | 30.031705 | 47,804,937 | 351 | No — time limit |
| R.200.1000.1.sop | 200 | 12.746805 | 1,543,263 | 1,404 | Yes |

Each returned path was independently checked in Python against the supplied
matrix. The first two costs are **feasible upper bounds**, not claims of known
optima. The third optimality statement comes from completed exact enumeration
with sound pruning, not comparison with a hardcoded benchmark answer.

`benchmarks/results.csv` contains all complete paths. `benchmarks/*.log` contains
incumbent updates, root bounds, search counts and proof/timeout status.
`benchmarks/metadata.json` records platform information and input/executable
SHA-256 hashes. Parallel scheduling and host load affect repeatability.

The observed difference between the 50-node and 200-node instances illustrates
why node count alone cannot predict exact SOP difficulty. These three cases do
not establish superiority to other exact implementations, nor practical
solvability of arbitrary inputs up to 1,000 nodes.
