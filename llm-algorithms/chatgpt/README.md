# SOP solver — adaptive exact search and concurrent improvement

A complete **C++17**, standard-library-only Sequential Ordering Problem solver. It returns a validated Hamiltonian path, improves it within a per-file time budget, and distinguishes a **proved optimum** from a time-limited incumbent. Node indices and endpoint possibilities come from the matrix; no original endpoint is hardcoded.

The strongest change in this version is **algorithm selection**: tightly ordered instances use exact layered dynamic programming over precedence ideals, while wider instances use parallel branch-and-bound with dynamic Hungarian assignment bounds. The latter now has shared dominance, cooperative work donation, local prefix dominance, and a live global bound. Feasible-solution search combines relocation, block exchange, exact window optimization, ruin-and-recreate, and ant-colony-inspired construction.

On the supplied `R.700.1000.60.sop`, this version proves cost **245,589** optimal in roughly **0.07 seconds** in the current paired measurements. The sparse `ry48p.1.sop` case remains seed-sensitive under short budgets; the solver reports that honestly instead of treating a heuristic incumbent as a proof. An independent Python exact solver confirms the same optimum. This result does not imply that every 700-node instance is easy; the supplied instance has an unusually narrow precedence state space. Full measured results and qualifications are in `benchmarks/REPORT.md`.

## Requirements, build, and run

Use a 64-bit C++17 compiler and standard library with threads; GCC 9+ or Clang 10+ recommended. There are **no external solver packages, downloads, licenses, or runtime dependencies**. Python 3.9+ is optional, for tests and benchmarks only.

From the extracted `sop_solver` directory:

```sh
g++ -std=c++17 -O3 -pthread -Wall -Wextra -Wpedantic sop_solver.cpp -o sop_solver
./sop_solver --time-limit 60 examples/reordered.sop
./sop_solver --time-limit 300 --threads 4 --output results.csv first.sop second.sop
./sop_solver -t 60 *.sop > results.csv 2> status.log
```

Alternatively, `make` builds it. On macOS substitute `clang++` after installing the Xcode command-line tools. On Windows use WSL, or MSVC from a Developer Command Prompt:

```bat
cl /std:c++17 /O2 /EHsc sop_solver.cpp /Fe:sop_solver.exe
sop_solver.exe --time-limit 60 --threads 4 examples\reordered.sop
```

Do not compile every `.cpp` together: the tests and historical baselines have separate main functions.

```text
sop_solver [--time-limit SECONDS] [--output FILE.csv] [--threads N]
           [--memory-mb MB] [--seed N] [--engine auto|bnb|dp] FILE.sop ...
```

| Option | Default | Meaning |
| --- | --- | --- |
| `--time-limit`, `-t` | 60 | Wall-clock seconds per input, including preprocessing; zero and fractions accepted |
| `--output`, `-o` | stdout | CSV destination; an existing output is overwritten |
| `--threads` | min(4, reported hardware threads), at least 1 | Search workers, 1–64; when at least 2, one is an improvement worker |
| `--memory-mb` | 256 | Approximate history-cache target in MiB; zero disables history. DP has a separate bounded workspace described below |
| `--seed` | 20261012 | Nonnegative random seed up to INT64_MAX |
| `--engine` | auto | Automatic portfolio, branch-and-bound only, or prefer exact layered DP |
| `--help`, `-h` | — | Usage |
| `--` | — | All remaining arguments are filenames, including names beginning with `-` |

`auto` attempts layered DP when more than 85% of original vertex pairs are transitively ordered. This attempt receives up to 25% of the requested budget, capped at five seconds. `bnb` skips it. `dp` attempts DP using the remaining time budget. An aborted DP attempt **never proves optimality**; if memory limits end it before the deadline, remaining time can be used by branch-and-bound.

Files run sequentially. One-worker branch-and-bound periodically gives short slices to the heuristic. Multithreaded and time-limited runs can differ across machines even with a fixed seed.

Exit code 0 means every file produced a validated solution, including time-limited results. Exit code 1 means an error occurred. A bad file gets an error row and diagnostic; subsequent files still run. An output path aliasing an input, including through hard links or symbolic links, is rejected.

## Input format and semantics

Header keys are parsed by keyword, with whitespace or a colon separating key and value. Keys are case-sensitive. `DIMENSION` is required; conflicting duplicate dimensions are rejected. `EDGE_WEIGHT_FORMAT`, if present, must be `FULL_MATRIX`. `NAME`, `TYPE`, `EDGE_WEIGHT_TYPE`, `COMMENT`, and unknown headers do not supply matrix values. COMMENT is free text, including numbers and other header keywords.

`EDGE_WEIGHT_SECTION` occupies its own line, followed by N again and **exactly N*N signed decimal integers**. Entries may wrap across any line boundaries. Trailing whitespace, end-of-file without a marker, or one optional `EOF` token are accepted. Other trailing tokens, truncation, nonintegers, and dimension mismatches are rejected.

Original vertices are `0..N-1` by row/column order:

- `T[i][j] >= 0`: directed cost i→j, including zero.
- `T[i][j] < 0`: **j must precede i**. Every negative marker has this meaning and is never a cost.
- Diagonal entries must be zero and are otherwise ignored.
- Large costs remain ordinary costs. A direct first-to-last edge can be used if it really belongs to a valid path, for example in a two-node instance.
- The result is a Hamiltonian **path**, with no return edge.

A unique precedence-DAG source and sink must be the first and last original vertices. If several endpoints are possible, all feasible choices remain in the search. Internal zero-cost virtual endpoints unify these cases and are removed from output.

`examples/reordered.sop` starts at 2 and ends at 1; it includes a zero-cost edge and omits EOF. The supplied `ry48p.1.sop` has **49 vertices according to its DIMENSION**, despite its filename.

## Algorithm and correctness

### Precedence preprocessing

Construct the precedence DAG and bitset transitive closure; reject cycles. Eliminate backward arcs, self-arcs, and direct adjacency a→b whenever an obligatory intermediate vertex k satisfies `a ≺ k ≺ b`. These exclusions preserve every feasible path.

### Exact layered DP for narrow state spaces

A state contains the complete visited bitset, last vertex, cheapest prefix cost, and a predecessor record for reconstruction. Extend each state only with available vertices whose predecessors have all been visited. Merge equal `(visited set, last vertex)` states **before expanding the next layer**.

Only the cheapest prefix can matter because identical states have identical feasible suffixes. Unlike depth-first history pruning, this layer ordering prevents repeatedly expanding a state as successively cheaper prefixes arrive. If all layers finish, the best complete path is optimal. Prefixes whose nonnegative cost already reaches the validated incumbent can be discarded; exhausting all improving states also proves that incumbent optimal.

The state workspace is capped using a conservative per-state estimate, targeting `max(8 MiB, memory-mb/2)`. Hitting the cap or DP deadline abandons the attempt without a proof. The workspace is freed before fallback search. This is exact bounded-attempt DP, **not beam truncation mislabeled as exact**.

The 700-node supplied instance has over 99.7% of pairs transitively ordered. Its independent Python DP needs a peak of just 20 states in a layer, which explains the large speed improvement. Instance size alone is a poor difficulty predictor.

### Dynamic Hungarian assignment bounds

For a prefix ending at a, assignment rows are a and all unvisited vertices except the virtual end; columns are all unvisited vertices. Every feasible completion induces a matching. The minimum matching may admit disconnected cycles and nonlocal order violations, so it is a lower bound rather than a feasible solution.

Solve the root assignment with a primal/dual Hungarian shortest-augmenting-path algorithm. Branching through a→b deletes row a and column b. A matched arc needs no augmentation; otherwise one augmentation repairs the optimum. This reuses O(N) matching/dual state and has O(N²) worst-case repair work rather than solving a fresh O(N³) assignment at every node. Dual reduced costs order branches and safely filter children before repair.

### Local and shared dominance

At an identical visited set and endpoint, a cheaper prefix dominates a more expensive one. All keys contain the **entire bitset and endpoint**, compared exactly. Hash collisions cannot cause incorrect pruning; there is no 64-node cutoff.

One quarter of the history target is divided among worker-local caches; three quarters is allocated to a shared table with 64 independently locked shards. Under each shard lock, one equal-cost arrival owns expansion. A strictly cheaper arrival supersedes the stored prefix; the older worker checks for strict supersession between children and can abandon redundant work. Equal-cost owners cannot mutually cancel. Shared pruning depends on the fact that every owning search obligation remains active/queued until completed or interrupted.

A full cache/shard is cleared, which loses pruning opportunities but cannot invalidate a proof. The budget is an estimate, not a hard process RSS cap.

There is also a history-independent local dominance rule: move the penultimate prefix vertex left across incomparable vertices while keeping the endpoint fixed. If this strictly reduces prefix cost, the original prefix cannot belong to an optimal full path. All comparisons use exact integer costs; equal-cost local moves never prune.

### Dynamic parallel work distribution and lower bounds

Start with the root task. Idle workers request work; an active DFS keeps its most promising child and donates unexplored siblings, including their current repaired assignment states. A synchronized best-bound pool dispatches donated work. Donation is limited to subtrees with at least 12 vertices remaining and occurs in response to demand. After a split, that worker performs at least 256 further DFS entries before donating again, amortizing assignment copies and queue locks. Undonated branches remain local; the cooldown changes scheduling, never completeness. Reduced-cost bounds are rechecked against the latest incumbent before sibling assignment repairs.

This is cooperative work donation rather than forcibly interrupting another thread's call stack. Every subtree remains local, queued, completed, or explicitly marked unfinished. Termination accounts for active and queued tasks; an empty queue alone is not completion. Exceptions propagate after workers are stopped and joined.

Workers periodically publish the minimum valid bound over their current branch and unvisited local siblings. The final lower bound considers active/interrupted tasks and queued tasks, capped by the incumbent. This can improve on the root assignment bound. If all improving work finishes, or a valid global bound meets the incumbent, optimality is proved. A cutoff never counts as completing an unfinished task.

### Feasible-solution improvement

A warmup of 15% of the requested time (capped at two seconds, with no minimum that can exhaust short budgets) and a continuing heuristic worker alternate:

- Greedy/randomized topological construction.
- Directed single-vertex relocation.
- Non-reversing adjacent-block 3-edge exchange, with O(1) precedence checks from a rectangle table. All block lengths are considered through 120 original vertices; above that, at least one exchanged block is limited to length 3.
- Exact optimization of contiguous windows of up to 14 vertices, preserving outside endpoints.
- Small ruin-and-recreate perturbations and feasible adjacent swaps, sometimes retaining a nonbest local minimum to escape the current basin.
- Ant-colony-inspired construction with bounded pheromone, incumbent reinforcement, local evaporation, and exploitation/roulette choice.

This component is an original portfolio, not a faithful implementation of the entire published EACS-SA algorithm. Heuristic operations never establish optimality. Every published improvement and the final path are independently validated against the original matrix.

## Output

CSV retains exactly the requested six columns:

```csv
Instance,Instance Size,Time,Enumerated Nodes,Best Cost,Best Path
"reordered.sop",4,0.010238,0,4,"2 0 3 1"
```

This example illustrates the schema; time and counts depend on engine and run. A root-bound proof can require zero DFS entries.

| Column | Meaning |
| --- | --- |
| Instance | Input basename, properly CSV-quoted |
| Instance Size | Original DIMENSION N |
| Time | Wall-clock seconds from input parsing through final validation; excludes CSV writing |
| Enumerated Nodes | Expanded layered-DP states plus DFS entries admitted past deadline checks, across all workers; includes immediately pruned DFS entries; excludes assignment augmentations, heuristic moves, and heuristic window-DP states |
| Best Cost | Recomputed original-matrix sum over consecutive original vertices |
| Best Path | Original indices in order, space-separated, 0-based |

Status goes to **stderr**, preserving the exact CSV schema:

```text
R.700.1000.60.sop: OPTIMAL (proven); cost=245589; start=0; end=699; lower_bound=245589; gap_percent=0; ...
```

Unfinished exact search reports `TIME_LIMIT (best found; not proven optimal)`. Retain status with `2> status.log`. Additional counters include dominance cuts, local prefix cuts, assignment repairs, bound-pruning events, and donated tasks. Their counts are not directly comparable to earlier versions' search work.

`gap_percent = 100*(cost-lower_bound)/cost`, or zero for zero cost. It is a **bound-based certificate of remaining uncertainty**, not the actual error relative to an unknown optimum. When no completed relaxation is available, zero is a valid lower bound because all usable costs are nonnegative. A complete proof sets the lower bound to the solution cost.

Malformed, cyclic, or rejected instances produce an ERROR diagnostic and a CSV row with blank cost/path and zero enumerated nodes. Dimension is included when parsing succeeded. Internal validation failure is also an error, never successful output.

## Limits and resource behavior

- SOP remains NP-hard. A sparse 49-node instance can be much harder than a dense 700-node instance. Proven optimality within a fixed time is not guaranteed, and performance against every published solver has not been established.
- The deadline is cooperative. Parsing, allocation, transitive closure, initial feasible construction, final validation, and cleanup finish even with a tiny budget. Long individual operations or thread/cache cleanup can slightly overrun the limit. Zero still returns a feasible path for valid acyclic input.
- Problem data is O(N²). Layered DP is exponential in the worst case but capped by its state budget. Branch-and-bound may need O(N²) matching/stack storage per exact worker, plus histories and heuristic workspaces. More workers can increase memory use or reduce speed under a CPU quota.
- DIMENSION is defensively capped at 10,000; that is not a practical-size guarantee. Intended use is tens through hundreds of nodes. Available memory and stack can become limiting earlier.
- Nonnegative entries must be at most `(INT64_MAX/4)/(N+2)`. This conservatively prevents overflow in costs, bounds, and intermediate comparisons. Oversized or invalid integers are rejected.
- Cache eviction, task transfer, contention, and nondeterministic scheduling affect speed. Exact keys, valid bounds, and completion accounting preserve correctness.
- Published research motivated the design, but this package is not a claim of world-best performance or a bit-for-bit reproduction of any research code.

## Verification and benchmark reproduction

```sh
make test
```

This runs:

- `test_solver.py`: exhaustive small-SOP oracles, thread/history/engine variants, a 70-node exact oracle forced through B&B, 701-node feasibility, parsing and CLI regressions.
- `test_bounds.cpp`: exhaustive matching comparisons for dynamic Hungarian states, primal/dual equality, dual feasibility, and reduced-cost bounds.
- `test_engines.cpp`: independent Held-Karp oracles, multithreaded shared-history and task-donation checks, and deliberately interrupted clustered instances with known optima to check live bounds.

An independent, simple exact Python implementation confirms the large narrow instance:

```sh
python3 benchmarks/verify_dp.py /path/to/R.700.1000.60.sop
```

It uses only raw precedence masks and layered state merging: no Hungarian bound, incumbent heuristic, local pruning, or shared history. Its state cap raises an error without claiming optimality on wider inputs.

Compare the current version to the immediately previous delivered source snapshot:

```sh
g++ -std=c++17 -O3 -pthread benchmarks/baseline_v3.cpp -o previous_solver
python3 benchmarks/benchmark.py --baseline ./previous_solver --solver ./sop_solver \
  --baseline-label previous --solver-label final --seconds 30 --repeats 1 --threads 4 --baseline-threads 4 \
  --output benchmark_results /path/to/ry48p.1.sop /path/to/R.700.1000.60.sop
```

The current paired runs use `--seed 17 --seconds 3 --repeats 3`; see the report for the exact five-input subset. Historical multiseed comparisons in `v3/` used `baseline_v2.cpp` and an earlier v3 candidate, not the current source. The optional seed override and `--baseline-threads` require a baseline supporting those options; `baseline_v1.cpp` does not.

Generate the additional synthetic suite (explicitly **not** SOPLIB/TSPLIB):

```sh
python3 benchmarks/generate_suite.py synthetic_inputs
python3 benchmarks/benchmark.py --baseline ./previous_solver --solver ./sop_solver \
  --baseline-label previous --solver-label final --seconds 3 --repeats 1 --threads 4 --baseline-threads 4 \
  --output synthetic_results synthetic_inputs/*.sop
```

Every benchmark output path is independently validated. Current paired measurements are in `benchmarks/v4/final/`. Earlier `v3/`, `results/`, and `results_single/` records describe historical versions, with the original v2 discussion preserved in `REPORT_v2.md`. The old `source_hashes.txt` is historical, not a current release manifest.

Create a clean source archive with `python3 package_release.py`. It includes a SHA-256 manifest and excludes executables, caches, and temporary files. The delivered ZIP includes all source, tests, documentation, example input, and benchmark records; benchmark input matrices must be supplied separately or regenerated for synthetic cases.

## Research basis

The following sources inform the architecture; their performance claims are not attributed to this implementation:

- Jamal et al. (2017), *Solving the sequential ordering problem using branch and bound*: dynamic Hungarian relaxation and local domination. [Institutional record](https://iris.unimore.it/handle/11380/1177202), [DOI](https://doi.org/10.1109/SSCI.2017.8280805).
- Gonggiatgul, Shobaki, and Muyan-Özçelik (2023), *A parallel branch-and-bound algorithm with history-based domination and its application to the sequential ordering problem*. [Manuscript](https://par.nsf.gov/servlets/purl/10379553).
- Shobaki et al. (2023), *Combining a Parallel Branch-and-Bound Algorithm with a Strong Heuristic to Solve the Sequential Ordering Problem*. [Manuscript](https://par.nsf.gov/servlets/purl/10463085).
- Libralesso et al., *Tree search algorithms for the Sequential Ordering Problem*: DP-inspired merging and search strategies particularly effective on dense precedence graphs. [Paper](https://arxiv.org/abs/1911.12427). The author-maintained [TreeSearchSolver example](https://github.com/fontanf/treesearchsolver) also reports cost 245,589 for the same named 700-node instance. Its timing is from a different environment and is not a controlled comparison.
- Skinderowicz (2017), *An improved Ant Colony System for the Sequential Ordering Problem*. [Paper](https://arxiv.org/abs/1705.01076). The construction used here is ACS-inspired; it does not implement that paper's complete EACS-SA system.
