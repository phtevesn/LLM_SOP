# Measured comparison and validation

The rebuilt solver improves the cost on the supplied 700-node instance and matches the baseline on the supplied 49-node instance. **None of these five-second runs proved optimality.** These tests substantiate improvement over this particular baseline; they do not establish a ranking against published state-of-the-art SOP solvers.

## Results

Each run received a five-second per-instance budget. The three-repeat comparison ran each process separately and alternated version order. Both versions used the same supplied input bytes. Every CSV path was independently checked by Python for permutation, every original precedence constraint, and recomputed cost.

| Instance | Configuration | N | Repeats | Median cost | Cost range | Median reported seconds | Proven optimal? |
| --- | --- | ---: | ---: | ---: | --- | ---: | --- |
| ry48p.1.sop | Original baseline, 1 worker | 49 | 3 | 15,805 | 15,805–15,805 | 5.000 | No |
| ry48p.1.sop | Rebuild, 4 workers | 49 | 3 | 15,805 | 15,805–15,805 | 5.051 | No |
| ry48p.1.sop | Rebuild, 1 worker | 49 | 1 | 15,805 | 15,805 | 5.030 | No |
| R.700.1000.60.sop | Original baseline, 1 worker | 700 | 3 | 246,910 | 246,910–246,910 | 5.002 | No |
| R.700.1000.60.sop | Rebuild, 4 workers | 700 | 3 | 245,589 | 245,589–245,589 | 5.003 | No |
| R.700.1000.60.sop | Rebuild, 1 worker | 700 | 1 | 245,673 | 245,673 | 5.002 | No |

On `R.700.1000.60.sop`, the four-worker rebuild saves **1,321 cost units (0.535%)** relative to the baseline in all three repeats. The single-worker rebuild saves **1,237 units (0.501%)** in its one comparison run. This distinguishes an algorithmic improvement from the additional benefit of multiple workers, although the single-worker evidence is only one run.

`ry48p.1.sop` has DIMENSION **49**. Both versions consistently returned 15,805. The cost is reported as a feasible incumbent; no external best-known-cost table is used to claim optimality.

The rebuild's root assignment lower bounds were 12,531 and 235,722 respectively. These certify conservative gaps of approximately 20.715% and 4.018% for the four-worker incumbents. These are bound-based gaps, not the actual errors relative to an unknown optimal solution.

Do not interpret enumerated-node ratios as speedups: the versions use different bounds, filters, preprocessing, and worker counts. Time includes preprocessing and final validation. Cooperative stopping and thread/cache cleanup explain small budget overruns.

## Environment and reproducibility

- CPU reported by the host: AMD EPYC 9V74 80-Core Processor.
- Architecture: x86_64.
- Container CPU quota: `800000 100000` (8 CPU-equivalents for this run).
- Compiler: `g++ (Ubuntu 13.3.0-6ubuntu2~24.04) 13.3.0`.
- Optimized build: `-O3 -std=c++17`; rebuild additionally uses `-pthread`.
- Baseline: exact source originally delivered, retained as `baseline_v1.cpp`.
- Rebuild seed: default 20261012. The baseline also uses its original fixed seed.
- Multiworker comparison: baseline uses one worker; rebuild uses three exact workers and one improvement worker.
- Single-worker comparison: both use one worker; the rebuild interleaves short heuristic time slices with exact search.

Raw results are in `results/` (three repetitions) and `results_single/` (one single-worker repetition). They include full paths, statuses, observed time, and explored-node counts. `environment.json` records input and executable SHA-256 hashes; `source_hashes.txt` records exact source hashes. The runtime environment is shared, so timings are not isolated laboratory measurements.

Build commands and benchmark commands are in the main README. Repeat the command with `--threads 1 --repeats 1` for the single-worker comparison. Original benchmark matrices remain separate from the source package.

## Correctness and robustness checks

Passed:

- 128 randomized SOP instances of sizes 1–8 compared with independent exhaustive permutation optima.
- Additional one-worker and disabled-history/two-worker comparisons on the first 32 cases.
- A 70-node instance with arbitrary endpoint indices and a freely reorderable seven-node block, checked against all 7! block permutations. This exercises states beyond one 64-bit word.
- Feasible paths through 701 nodes, including zero-time requests, and heuristic validation at 40 and 75 nodes.
- 6,119 dynamic assignment states across 100 randomized small inputs, each checked against an exhaustive matching oracle, primal/dual equality, dual feasibility, and reduced-cost child bounds.
- Parsing regressions: wrapped matrices, optional EOF, free-text COMMENT, dimension mismatch, truncation, cycles, extra tokens, negative markers other than -1, and zero costs.
- CSV quoting, multiple inputs, output destination, and refusal to overwrite an input.
- AddressSanitizer and UndefinedBehaviorSanitizer runs of both test suites with no findings. LeakSanitizer was disabled because the container cannot expose the process information it requires; **leak detection was not validated**. No ThreadSanitizer claim is made.

Reproduce sanitizer checks on a compatible GCC/Clang platform:

```sh
g++ -std=c++17 -O1 -g -pthread -fsanitize=address,undefined -fno-omit-frame-pointer test_bounds.cpp -o test_bounds_san
g++ -std=c++17 -O1 -g -pthread -fsanitize=address,undefined -fno-omit-frame-pointer sop_solver.cpp -o sop_solver_san
ASAN_OPTIONS=detect_leaks=0 ./test_bounds_san
ASAN_OPTIONS=detect_leaks=0 python3 test_solver.py ./sop_solver_san
```

## Remaining limits

Two real instances and small randomized correctness tests are not a full SOPLIB/TSPLIB evaluation. This implementation has not been compared directly against the published research solvers cited in the README. In particular, it does not implement their full shared-history management, recursive work stealing, thread restart, or enhanced ant-colony machinery. Claims are limited to the implemented guarantees and the measured results above.
