# DEVELOPMENT.md

Everything below is split into **measured** (I compiled and ran it and saw the
numbers), **reasoned only** (not run), and **untested**. Environment used for
all measurements: the development sandbox with **one CPU core** (`nproc` = 1),
about 3 GB RAM, g++ 13.3. No benchmark below comes from the 32-core target
machine. Times of runs with several threads on this single core only show that
the parallel code is correct, not that it is fast.

## 1. Initial plan and alternatives considered

| Alternative | Verdict | Why |
|---|---|---|
| Held-Karp style DP over precedence ideals (state = visited set, last node) | **dropped as the main algorithm, kept as a dominance table** | Measured with a small Python layer-by-layer enumeration of ideals, capped at 4M ideals in total: the cap was exceeded after the layer with 9 visited nodes in `rbg048a` (largest layer so far 3,536,608) and after the layer with 8 visited nodes in `prob_42` (3,340,524), with 40+ layers still to go, so the full DP is far beyond memory. Used only as dominance inside the search (transposition table). |
| Plain branch and bound over prefixes with a cheap bound (cheapest allowed in-arc, reduced costs) | **dropped as insufficient alone** | Measured (first version, 1 thread, 60 s): `prob_42` and `rbg048a` not proven after roughly 80-90M nodes; `R_200_1000_1` not proven after 53M nodes in 120 s. |
| B&B with a heuristic upper bound (ILS) | **kept** | The ILS finds the best known costs of the small samples in about a second (`prob_42`: 243; `rbg048a`: 351). Giving the search the optimal bound 243 up front still did not finish `prob_42` in 60 s with the cheap bound (83.5M nodes), i.e. the bound, not the incumbent, was the problem. |
| Hybrid: ILS threads alongside exact threads | **kept** | With 4+ threads `max(1, T/8)` ILS threads continue during the exact phase. Only the logic was tested (oversubscribed on one core). |
| Iterative deepening on the reduced-cost budget (IDA*-like) | **added, kept** | See section 3. |
| Incremental assignment (AP) bound at every node | **added, kept** | See section 3. |
| AP-subtour branching (Carpaneto-Toth style, branch on cycles of the AP solution) | **reasoned only, not implemented** | Classic and strong for ATSP-like instances; dropped for scope. Might beat prefix search on instances where the AP gap is small and degeneracy is high. |
| Abstraction lower bounds (Hernadvolgyi) | **reasoned only, not implemented** | Would attack the precedence-induced gap (e.g. `prob_42`: AP 174 vs 243) but is a large piece of work. |
| Branch and cut with precedence cuts (Ascheuer et al.) | **reasoned only, not implemented** | Needs an LP solver and separation routines; out of scope. |
| Beam search / iterated beam search for upper bounds | **reasoned only, not implemented** | Candidate for large instances where the ILS is weak (see section 5). |

## 2. Bugs and dead ends

1. **Heuristic dead ends (measured).**
   * ILS with kicks anywhere in the path: `R_200_1000_1` only 1554 after 11 s; with *local* kicks (insertion point within +-20..80 positions) 1507 after 7.7 s; `prob_42` reached 243 after 0.6-4.4 s with global kicks and about 1 s with local kicks.
   * AP patching, attempt 1 (splice cycles ignoring precedence, then repair by a topological pass preferring the earliest original position): on a synthetic N=1000 instance the repaired sequence cost 109,601 (bound 1197); the repair displaces every violating node far away.
   * Attempt 2 (splice whole cycles only where all ancestor/descendant intervals allow it, with rotation checks): failed outright on some cycles; with cycles split into single nodes it still cost 23,645.
   * Final version: patch ignoring precedence, keep non-conflicting nodes, reinsert conflicting ones at their cheapest feasible position. On ATSP-like synthetic instances it is good (see section 4); on constrained synthetic instances it is still poor.
2. **Cheap-bound search stalls on degenerate duals (measured).** With only the in-arc bound the very first budget iteration (B = 0) on `R_200_1000_1` did not finish in 60 s (362,747,905 nodes): hundreds of zero-reduced-cost arcs create exponentially many zero-cost prefixes. Fixed by the incremental assignment bound (3.5 s afterwards).
3. **My assumption about the benchmark structure was wrong.** I first generated test instances with random precedence pairs at about the density I thought `R_200_1000_1` had. Inspection of the file showed 397 negative entries, almost exactly the start/end constraints (199 + 198), closure equal to the direct pairs: it is essentially an ATSP with fixed start and end. My random-DAG instances (and a chain generator) are far more constrained (N=200: bound 1706 vs ILS 5571). Consequence: those synthetic results say little about the real benchmark families; I did not have other real instances.
4. **Test-script bugs (mine, fixed).** The "cycle" special case in `tests/run_tests.sh` was not cyclic (it was a chain) until I changed it; the chain generator first produced contradictory constraints (chains must follow the hidden order).
5. **Background process.** A long run started with `nohup &` was killed when the tool call ended; the log looked as if the solver had stopped at 0.5 s. Long runs were redone in the foreground.
6. **Time-limit overshoot (measured, fixed).** On a synthetic N=1000 instance with `--time-limit 40` the first version ended at 40.55 s (nodes are slow at N=1000 and the clock was polled every 1024 nodes); after polling every 128 nodes the same run ended at 40.02 s.
7. **Debug scaffolding** (environment-variable debug logs) was removed before the final runs; `grep` on the sources shows none left.

## 3. Design decisions that changed, and what triggered each

| Change | Trigger |
|---|---|
| Added iterative deepening on the reduced-cost budget | Measured: with a fixed bound from a mediocre ILS solution the search wandered through huge budgets; budgets of 0, 1, 2, ... make cheap passes first, find solutions by themselves (R_200: optimum found in the eighth iteration although the ILS bound was far off), and raise a proven lower bound as a by-product. |
| Added the incremental assignment bound (kept the in-arc bound as pre-filter) | Dead end 2 above, and measured node counts on `rbg048a` (single thread, early budget iterations): 5.55M -> 13k nodes (B = 0) and 38.9M -> 151k nodes (B = 1). On `prob_42` the proven lower bound after about 20 s rose from 216 to 237. (Figures recorded from console output during development; warm-up settings differed between runs.) |
| ILS kicks made local | Measured slow convergence with global kicks (dead end 1). |
| ILS candidate lists and construction switched to reduced costs after the AP duals exist | Raw-cost candidate lists were poor on random-cost instances; the AP duals arrive after about 0.04-0.16 s, so the ILS publishes new candidate data when they exist. |
| Added AP patching start | Synthetic ATSP-like N=500: the patched start is 1471 vs bound 1457 immediately; N=200 real sample: warm-up bound improved from 1756 to 1468-1584 and total time from 3.5 s to 1.95 s. |
| Warm-up lengthened adaptively (stall window) instead of fixed | At N=1000 a fixed short warm-up left the incumbent very poor while it was still improving quickly. |
| Time polling every 128 nodes | Dead end 6. |
| Zobrist salt per budget iteration instead of clearing the table | Reasoned: clearing a multi-GB table per iteration is wasteful; correctness of the salting was only exercised through the test runs. |

## 4. What was actually compiled, run and measured

**Compiled:** all sources with g++ 13.3, `-std=c++17 -O3 -march=native -Wall -Wextra -pedantic`, no warnings (checked at the points where I rebuilt; final build clean).

**Correctness tests (against `tests/brute_force.py`, an independent subset DP):**
* Final code: 60 random instances, N = 3..11, 1 thread; 40 instances, 4 threads; 3 instances with N = 13-14, 2 threads. All matched in cost and reported `yes`.
* Earlier code versions (before the final heuristic changes): 60 instances at 4 threads, 40 at 8 and at 2 threads, 6 mid-size instances (N = 13-14) at 1 and 4 threads, all matched.
* Covered: unique and multiple sources/sinks (dummy nodes), ties (costs 0..4), zero precedence, wrapped matrices, files with and without `EOF`; N = 1, N = 2, cyclic precedence (`-1`, empty path, `no`).
* The assignment value on `R_200_1000_1` (1392) matched `scipy.optimize.linear_sum_assignment` on the same reduced problem (one comparison).

**Sample instances (1 thread, final code unless noted):**

| Instance | Result | Time | Nodes |
|---|---|---|---|
| `R_200_1000_1` | 1404, proven optimal | 1.95 s | 1,138,263 |
| `R_200_1000_1`, `--threads 4` (on 1 core) | 1404, proven optimal | 2.30 s | 1,150,837 |
| `prob_42` | 243, proven optimal | 108.49 s | 28,489,096 |
| `prob_42` (earlier code, before the AP patching start) | 243, proven optimal | 129.38 s | 37,641,145 |
| `rbg048a`, 290 s limit | 351, **not proven**; proven lower bound 347 (no path of cost <= 346 exists, iteration 6 finished at 214 s) | 290.0 s | 104,329,042 |

I believe 243 and 351 are the published optima of these instances but I did not
check that; the solver itself proved 243. For `rbg048a` the proof would need the
final budget of 13 above the root bound; node counts grew by a factor of about 4
per iteration, so I expect on the order of 10^9 more nodes on one core; I have
not run that.

**Synthetic instances made with `tests/gen_random_sop.py` (true optima unknown unless stated; 1 thread):**
* ATSP-like (only start/end constraints) N = 500: 1457, proven optimal in 1.92 s, 15,289 nodes.
* ATSP-like N = 1000: bound 1178, best found 1294 after 60 s, not proven (10.8M nodes).
* Random-DAG N = 200, density 0.02: bound 1706, best 5571 after 20 s, not proven; chain-structured N = 200 (chain length 5): bound 1622, best 5234 after 60 s; same family N = 1000: bound 1116, best 12050 after 60 s. These are much harder than the real `R_200` and show the weakness on strongly constrained large instances.
* N = 1000 runs completed, validated, and respected the time limit (40.02 s for a 40 s limit); the assignment solve took about 0.13-0.16 s on these random matrices.

## 5. Untested or only reasoned

* **Multi-core speed-up and load balance** on the 32-core machine: not measured. The work-splitting logic ran with 2-8 threads on one core only.
* **NUMA behaviour**: pinning, SMT ordering, `mbind` interleaving, transparent-huge-page advice were never exercised on a NUMA machine (the sandbox reports one node/core); all calls are best-effort and fail silently. Whether the pin order is actually good for the 2990WX is a design argument, not a measurement.
* **g++ 9.4 build** and **CMake build**: not tried.
* **Large transposition tables** (GiB range), table saturation, and the effect of replacement on node counts: not tested; the sandbox default was about 0.75 GB.
* **Memory at N = 1000 with many threads**: estimated at about 24 MB per exact thread for the assignment snapshots plus the table; not measured.
* **Hash collisions**: probability argued (128-bit identity), no collision experiment.
* **Instances with heavy precedence and N >= 100** (the `rbg` family of 100-400 nodes): none available; no results.
* **Proof of `rbg048a`**: not obtained.
* **Quality of the heuristic on large constrained instances**: poor on my synthetic examples (section 4). Ideas not done: diving/limited-discrepancy search with the incremental AP bound for upper bounds, iterated beam search, an Or-3opt or Lin-Kernighan style neighbourhood, AP-subtour branching.
* **Behaviour when the AP solve would take very long** (N = 1000 with adversarial matrices): the solve is interruptible only through the stop flag between rows.

## 6. File map

`src/main.cpp` CLI; `instance.*` parser; `model.*` preprocessing; `ap_bound.*`
Hungarian bound; `heuristic.*` ILS and constructions; `exact_search.*` parallel
B&B; `transposition_table.*` dominance table; `numa_topology.*` NUMA helpers;
`incumbent.*` shared best solution; `solver.*` orchestration; `output.*`
validation and CSV; `tests/` generator, brute force, runner.
