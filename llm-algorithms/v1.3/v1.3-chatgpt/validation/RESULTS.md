# Validation results

Executed on the development Linux x86-64 environment with GCC 13.3.0 and
OpenMP. The environment reported 9 available processing units. It was not the
user's 32-core Threadripper system. Tests are functional evidence, not a controlled
hardware speed comparison; sanitizer testing overlapped part of the benchmark
window. No 32-core speedup is claimed.

## Exactness and interface tests

The optimized final source passed all 261 invocations in `tests/test_solver.py`.
The test compares against independent subset-DP and exhaustive-permutation
oracles and includes a shuffled 1,000-node forced chain. See README for coverage.

The same 261 invocations also passed with AddressSanitizer and
UndefinedBehaviorSanitizer enabled (`ASAN_OPTIONS=detect_leaks=0`).
No sanitizer diagnostics were reported.

## Supplied instance runs

Each invocation used `--threads 8 --time-limit 30`, the default seed, default
16 MiB estimated history budget per worker, and 32 heuristic starts. Instances
ran sequentially. Names do not determine dimension: rbg048a.sop contains 50 nodes.

| Instance | N | Seconds | Processed states | Cost | Optimality proved |
|---|---:|---:|---:|---:|---|
| rbg048a.sop | 50 | 30.047019 | 51574555 | 352 | no |
| prob.42.sop | 42 | 30.000128 | 35544935 | 261 | no |
| R.200.1000.1.sop | 200 | 15.032463 | 2695353 | 1404 | yes |

The 200-node instance was proved optimal at cost **1404**. The two other runs
hit their time limits; their reported costs are upper bounds, not established
optima. Their root assignment lower bounds were 337 (rbg048a) and 174 (prob.42).
The 200-node root assignment lower bound was 1392.

All three returned paths were checked again by a separate Python script against
the original uploaded matrices for permutation, every precedence constraint,
and exact summed cost. Full paths and counts are in `benchmarks.csv`; stderr
logs are alongside it. Timing and node counts vary with system load and task order.

A branch-and-bound completion establishes optimality algorithmically; this
package does not produce a standalone formal proof certificate.
