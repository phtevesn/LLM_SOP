# Current release: measurements and limitations

This report describes the v4 pass (2026-10-13). Historical records are retained
but are not measurements of the current source.

## Changes and verification

- Removed the 50 ms minimum heuristic warmup. Warmup is 15% of the requested
  time, capped at two seconds; small budgets can now reach exact search.
- Added a 256-DFS-entry cooldown between sibling-donation batches per worker.
  Undonated work stays local: no branches are lost.
- Recheck reduced-cost bounds against the latest incumbent before repairing
  sibling assignments, avoiding work made unnecessary by improvements.
- Strengthened interruption tests to require actual search expansions.
- Added explicit baseline worker counts to the benchmark harness and a
  deterministic source-only ZIP builder with a verified SHA-256 manifest.

`make test` passed on this source: 6,119 exhaustive assignment comparisons;
131 independent engine/oracle comparisons (128 completed proofs in this run);
128 exhaustive small-SOP cases; worker/history/engine variations; a 70-node
exact oracle forced through B&B; 701-node feasibility; parsing and CLI checks.
The interrupted-search tests now verify that exact search actually ran.
Shared-history ownership and task donation are exercised. Proof counts in
time-limited tests vary with machine load.

AddressSanitizer/UndefinedBehaviorSanitizer passed the current engine suite
(131 comparisons, 109 proofs). Leak detection was disabled; leaks were not
checked. The assignment suite also passed AddressSanitizer/UndefinedBehaviorSanitizer
(6,119 states). ThreadSanitizer passed the engine suite (131 comparisons,
111 completed proofs), with no reported races. Sanitizer timings are not
performance measurements. These checks do not constitute a formal proof of
the implementation's correctness.

## Paired measurements

Five inputs, three repetitions each, seed 17, three seconds per input, four
workers for both versions. Processes ran sequentially, alternating version order
each repetition; no compiler/test suite ran concurrently with these final runs.
All 30 paths were independently validated against the original matrix.
The environment is shared, not an isolated benchmark host.

`v3` is the preceding delivered source, preserved as `baseline_v3.cpp`.
`v4` is the packaged current source. Lower costs are better.

| Instance | N | v3 median cost | v4 median cost | v3 median seconds | v4 median seconds | v4 proofs |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| ry48p.1.sop | 49 | 15,839 | 15,839 | 3.119140 | 3.129536 | 0/3 |
| R.700.1000.60.sop | 700 | 245,589 | 245,589 | 0.068588 | 0.072227 | 3/3 |
| synthetic_100_metric_0.01.sop | 100 | 10,610 | 10,610 | 3.056741 | 3.086937 | 0/3 |
| synthetic_200_random_0.15.sop | 200 | 20,963 | 20,825 | 3.011043 | 3.008386 | 0/3 |
| synthetic_40_random_0.02.sop | 40 | 2,165 | 2,165 | 0.513410 | 0.553940 | 3/3 |

The 200-node costs were v3: 20,825 / 20,963 / 21,635; v4: 21,695 / 20,825 /
20,825. The improved median is not a uniform win: v4's worst run was worse.
Three repeats with one seed do not support a statistical performance claim.

On the 40-node case, median task donations fell from 688 to 81 (88% fewer),
but median proof time increased from 0.513 to 0.554 seconds. On the 49-node
case, median DFS entries fell from 1,551,976 to 1,325,922 without a better cost.
**There is no demonstrated overall speedup from donation throttling.** Its
benefit here is bounded task-transfer frequency, not consistently faster proofs.

The 49-node case has a previously observed feasible cost of 15,805 at other
seeds/budgets. These seed-17 runs did not reach it and did not prove optimality.
Do not label 15,839 best known, or 15,805 proven optimal.

The supplied 700-node case still proves 245,589 with 2,684 C++ DP expansions.
An independent Python DP again confirmed this optimum: 2,683 expansions, peak
layer 20, 0.529 seconds in this pass. Counting conventions explain the one-state
difference. The independent implementation uses no C++ assignment bounds,
heuristics, or local/shared dominance. This narrow state space is atypical;
700 nodes alone does not imply a difficult instance.

## Reproduction and identity

From the package root:

```sh
make
g++ -O3 -std=c++17 -Wall -Wextra -Wpedantic -pthread benchmarks/baseline_v3.cpp -o previous_solver
python3 benchmarks/generate_suite.py synthetic_inputs
python3 benchmarks/benchmark.py --baseline ./previous_solver --solver ./sop_solver \
  --baseline-label v3 --solver-label v4 --seconds 3 --repeats 3 \
  --threads 4 --baseline-threads 4 --seed 17 --output rerun \
  /path/to/ry48p.1.sop /path/to/R.700.1000.60.sop \
  synthetic_inputs/synthetic_100_metric_0.01.sop \
  synthetic_inputs/synthetic_200_random_0.15.sop \
  synthetic_inputs/synthetic_40_random_0.02.sop
python3 benchmarks/verify_dp.py /path/to/R.700.1000.60.sop
```

The two supplied matrices are not bundled. Synthetic cases can be regenerated;
they are not published SOPLIB/TSPLIB instances. Current raw CSV paths, logs,
summary, input hashes and executable hashes are in `v4/final/`.

The release ZIP's `MANIFEST.json` hashes every payload file, including current
source, historical sources and measurements; it does not hash itself. Host:
AMD EPYC 9V74, x86_64 Linux, eight-CPU quota; compiler GCC 13.3.0, C++17,
`-O3 -pthread`. Source filenames can affect ELF symbols/build IDs; binaries
compiled on different systems need not have identical executable hashes.

## Historical records — not current-source measurements

- `REPORT_v2.md`, `results/`, `results_single/`, `source_hashes.txt`: original
  v1 versus v2. Statements there that the 700-node runs were unproven do not
  apply to the current layered-DP engine. The old hash file is not the release
  manifest.
- `v3/`: earlier v2-to-v3 development comparisons, with multiple seeds, eight
  synthetic inputs and 30-second runs. These predate the final v3 warmup
  adjustment and all v4 changes. They include both wins and regressions.
- `v4/pilot/`: cooldown-only exploratory candidate, before the final sibling
  checks and short-budget change. Some runs overlapped correctness tests.
  These are not valid final-source speed measurements.

## Remaining limitations

This is a complete runnable exact/heuristic portfolio, not a demonstrated
world-best SOP solver. Sparse cases can remain unproved and return inferior
incumbents at short budgets. A full public-suite, multiseed comparison against
established research implementations has not been completed. Existing sparse
performance regressions are not all resolved. See the README for cooperative
deadlines, estimated memory budgets and worst-case exponential search.
