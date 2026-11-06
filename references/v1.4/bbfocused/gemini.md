# Branch-and-Bound Component Inventory

This inventory describes what each algorithmic component of the Branch-and-Bound process resembles based on direct inspection of the code, rather than recalling how the code was written.

Preprocessing and bitset mechanics are omitted. No URLs are fabricated, and uncertainty or multiple foundational sources are noted explicitly.

## Contents

1. [GRASP randomized topological construction](#1-grasp-randomized-topological-construction-initial-upper-bound)
2. [Precedence-constrained VND](#2-precedence-constrained-variable-neighborhood-descent-vnd-upper-bound-refinement)
3. [Ruin-and-recreate LNS](#3-ruin-and-recreate-large-neighborhood-search-lns-upper-bound-intensification)
4. [Root dual lower bounds](#4-root-dual-lower-bounds-and-early-optimality-termination)
5. [Bidirectional meet-in-the-middle DP](#5-bounded-bidirectional-meet-in-the-middle-dp)
6. [Prefix pool generation](#6-shallow-depth-2-prefix-pool-generation)
7. [Static degree lower bound](#7-incremental-static-inout-degree-lower-bound)
8. [Dynamic degree lower bound](#8-lazy-dynamic-inout-degree-lower-bound)
9. [History dominance pruning](#9-prefix-equivalence-and-history-dominance-pruning)
10. [DFS backtracking](#10-depth-first-search-backtracking)
11. [Parallel BnB and LNS](#11-cooperative-parallel-work-stealing-bnb-and-concurrent-lns)

## 1. GRASP Randomized Topological Construction (Initial Upper Bound)

- **Where:** `heuristics.cpp` (`construct_randomized`, lines 12–55; `run_portfolio`, lines 246–253); invoked at the start of `solver.cpp` (`solve`, lines 32–51).
- **Classification:** Adaptation of an established technique.
- **Confidence:** High.

To seed the Branch-and-Bound global upper bound (`best_cost`), the solver builds feasible topological tours step-by-step from ready vertices using a value-based Restricted Candidate List (RCL) with threshold `min_c + alpha * (max_c - min_c)` (`alpha = 0.0` on start 0 and `alpha = 0.20` thereafter). This adapts the Feo–Resende GRASP construction heuristic to precedence-constrained posets.

**Likely sources:** Thomas A. Feo and Mauricio G. C. Resende, *A Probabilistic Heuristic for a Computationally Difficult Set Covering Problem* (~1989, *Operations Research Letters*); and *Greedy Randomized Adaptive Search Procedures* (~1995, *Journal of Global Optimization*).

**Search terms:** `"Greedy Randomized Adaptive Search Procedures" Feo Resende 1995 "Restricted Candidate List"`

## 2. Precedence-Constrained Variable Neighborhood Descent (VND, Upper-Bound Refinement)

- **Where:** `heuristics.cpp` (`local_search`, lines 57–195).
- **Classification:** Adaptation of an established technique.
- **Confidence:** High.

Each constructed or perturbed upper-bound tour is refined to local optimality by cycling through four ordered neighborhoods with a first-improvement pivot rule (`goto next_pass`): 1-node Or-Opt shifts, SOP-2-Opt segment reversals, Don't-Look-Bits SOP-3-Exchange adjacent block swaps, and short-block (`L in {2, 3, 4}`) Or-Opt shifts. Each neighborhood aborts its inner scan (`break`) as soon as a precedence conflict is reached.

This combines Mladenović and Hansen's VND framework with Savelsbergh's and Gambardella and Dorigo's precedence-preserving SOP neighborhoods, plus Bentley's Don't-Look-Bits.

**Likely sources:** Nenad Mladenović and Pierre Hansen, *Variable Neighborhood Search* (~1997); İlhan Or (~1976, *Or-Opt*); G. A. Croes (~1958, *2-Opt*); Martin W. P. Savelsbergh (~1990); Luca Maria Gambardella and Marco Dorigo, *HAS-SOP: Hybrid Ant System for the Sequential Ordering Problem* (~1997/2000); and Jon Louis Bentley (~1992, *Don't-Look-Bits*).

**Search terms:** `"Variable Neighborhood Descent" Mladenovic Hansen`; `"SOP-3-exchange" Gambardella Dorigo`; `"don't look bits" Bentley 1992`

## 3. Ruin-and-Recreate Large Neighborhood Search (LNS, Upper-Bound Intensification)

- **Where:** `heuristics.cpp` (`ruin_and_recreate`, lines 197–235; `run_portfolio`, lines 263–280); `solver.cpp` (lines 731–762).
- **Classification:** Adaptation of an established technique.
- **Confidence:** High.

To escape local optima and continuously tighten the global upper bound before and during Branch-and-Bound, the solver randomly removes `k in [3, min(12, max(3, N/4))]` interior vertices, shuffles them, and greedily reinserts each vertex into its valid precedence interval `[min_pos, max_pos]` in the partial tour. Random coin-flip tie-breaking is used, followed by VND local search.

This adapts Shaw's Large Neighborhood Search and Schrimpf et al.'s Ruin-and-Recreate principle to precedence-constrained paths.

**Likely sources:** Paul Shaw, *Using Constraint Programming and Local Search Methods to Solve Vehicle Routing Problems* (~1998); and Gerhard Schrimpf, Johannes Schneider, Hermann Stamm-Wilbrandt, and Gunter Dueck, *Record Breaking Optimization Results Using the Ruin and Recreate Principle* (~2000).

**Search terms:** `"Large Neighborhood Search" Shaw 1998`; `"Ruin and Recreate" Schrimpf 2000`

## 4. Root Dual Lower Bounds and Early Optimality Termination

- **Where:** `sop_instance.cpp` (`compute_ap_relaxation`, lines 233–301; `compute_hk_relaxation`, lines 303–420); `solver.cpp` (`solve`, lines 53–61).
- **Classification:** Adaptation of established techniques.
- **Confidence:** High.

Before branching, the solver computes two dual relaxations at the root node:

1. An `O(N^3)` shortest-augmenting-path Hungarian Assignment Problem relaxation (`root_ap_lb`, yielding reduced costs `rcost`).
2. A subgradient-optimized Directed Held–Karp MST Lagrangian relaxation (`root_hk_lb`, yielding scaled potentials `pi_out`, `pi_in`, and symmetrized matrix `sym_hk`).

It immediately terminates with proven optimality when the heuristic upper bound satisfies `best_cost <= max(root_ap_lb, root_hk_lb)`. This combines the classic Balas–Toth AP relaxation and Held–Karp 1-tree Lagrangian relaxation, adapted to SOP paths.

**Likely sources:** Harold W. Kuhn (~1955); Roy Jonker and Anton Volgenant (~1987); Egon Balas and Paolo Toth (~1985); Michael Held and Richard M. Karp (~1970/1971); and Laureano F. Escudero, M. Guignard, and K. Malik (~1994).

**Search terms:** `"Shortest Augmenting Path" Jonker Volgenant 1987`; `"Held and Karp" 1971 subgradient MST`; `"Lagrangian" "Sequential Ordering Problem" Escudero 1994`

## 5. Bounded Bidirectional Meet-in-the-Middle DP

- **Where:** `solver.cpp` (`solve`, lines 63–75; `run_parallel_bidirectional_mitm`, lines 139–573).
- **Classification:** Adaptation of established techniques plus a custom hybrid policy.
- **Confidence:** High.

For `N <= 128`, the solver first runs a breadth-wise bounded bidirectional DP, expanding the smaller of the forward-prefix frontier or backward-suffix frontier. It prunes states whose cost plus a mask-factored composite lower bound exceeds `ub`; the bound combines complement Prim's MST (`mst_complement`), unvisited in/out-degree sums, and an AP reduced-cost bound.

Transitions are restricted to a single forced vertex when `inf_in_count == 1` or `inf_out_count == 1`. If both frontiers exceed `MAX_LAYER_MASKS = 350000`, DP is aborted and the solver hands off to depth-first Branch-and-Bound via `run_parallel_bnb_and_lns`.

The bounded bidirectional DP adapts Morin and Marsten (~1976) and Righini and Salani (~2006), with Pohl's (~1971) smaller-frontier rule. The `350000`-mask DP-to-DFS-BnB handoff is a custom hybrid switching policy.

**Likely sources:** Thomas L. Morin and Roy E. Marsten (~1976); Giovanni Righini and Matteo Salani (~2006); Ira Pohl (~1971). The handoff threshold is attributed to Gemini in the original analysis.

**Search terms:** `"Branch-and-Bound Strategies for Dynamic Programming" Morin Marsten 1976`; `"Bounded Bi-Directional Dynamic Programming" Righini Salani 2006`

## 6. Shallow Depth-2 Prefix Pool Generation

- **Where:** `solver.cpp` (`run_parallel_bnb_and_lns`, lines 584–615).
- **Classification:** Adaptation of an established technique.
- **Confidence:** High.

To initialize depth-first Branch-and-Bound, the solver generates all precedence-feasible 2-vertex prefixes `(s, v)`, prunes any prefix whose edge cost plus the remaining static degree sum already meets or exceeds `best_cost`, and sorts surviving subtrees by ascending `prefix_cost`.

Decomposing the root into a pool of shallow subproblems and ordering them best-first so workers explore promising branches early is a standard parallel Branch-and-Bound initialization strategy.

**Likely sources:** Vipin Kumar and V. Nageshwara Rao (~1987); Bernard Gendron and Teodor Gabriel Crainic (~1994).

**Search terms:** `"Parallel Branch-and-Bound" Gendron Crainic 1994`; `shallow prefix task pool parallel branch and bound`

## 7. Incremental Static In/Out-Degree Lower Bound

- **Where:** `solver.cpp` (`run_parallel_bnb_and_lns`, lines 578–598, 687, 698–702).
- **Classification:** Adaptation of an established technique.
- **Confidence:** High.

The solver pre-sums the global static minimum incoming (`tot_in`) and outgoing (`tot_out`) arc costs across all vertices, then incrementally subtracts `min_in1[v]` and `min_out1[v]` as each vertex `v` is added to the current prefix (`r_in`, `r_out`).

This enables an `O(1)` lower-bound check upon entering node `u`, `pcost + max(r_in, r_out + min_out1[u]) >= cur_ub`, and a lookahead filter before recursing on child `v`, `pcost + ec + max(nxt_in, nxt_out) >= best_cost`. Incremental degree-sum bounding is a classic TSP/SOP relaxation derived from row/column minimum reductions.

**Likely sources:** John D. C. Little, Katta G. Murty, Dura W. Sweeney, and Caroline Karel (~1963); and SOP Branch-and-Bound work by Yaroslav Shylo, Oleg Prokopyev, Jayant Rajgopal, and J. Jamal et al. (~2015–2020).

**Search terms:** `"Little Murty Sweeney Karel" 1963 row column reduction`; `"Sequential Ordering Problem" branch and bound static lower bound degree`

## 8. Lazy Dynamic In/Out-Degree Lower Bound

- **Where:** `solver.cpp` (`compute_dyn_lb`, lines 619–667; called at line 690).
- **Classification:** Adaptation of an established technique.
- **Confidence:** High.

Only after a search node survives the `O(1)` static bound and history-table check does `compute_dyn_lb` compute a tighter `O(N)` lower bound over the unvisited subgraph. For each unvisited vertex `w`, it first checks in `O(1)` whether `w`'s static cheapest partner (`min_in1_src[w]` or `min_out1_dst[w]`) is still unvisited or equals the current endpoint `u`.

The solver scans `w`'s cost-sorted adjacency list (`adj_in[w]`, `adj_out[w]`) only when that static partner has already been visited, pruning immediately if any required unvisited degree has no feasible arc (`>= INF`). This lazy two-tier invalidation check is a well-known optimization of dynamic row/column reduction bounds in SOP Branch-and-Bound.

**Likely sources:** Little, Murty, Sweeney, and Karel (~1963); Monet / Hernádvölgyi (~2003–2004); and Jamal, Shylo, et al. (~2017–2020). The specific SOP paper that first added the `min_in1_src` validity check is uncertain; it may be a natural implementation refinement.

**Search terms:** `"Sequential Ordering Problem" branch and bound dynamic lower bound sorted adjacency`; `dynamic row column reduction branch and bound ATSP`

## 9. Prefix Equivalence and History Dominance Pruning

- **Where:** `history_table.hpp` (`PrefixDominanceEntry`, `ShardedHistoryTable`, lines 13–66); `solver.cpp` (lines 576, 604, 688, 706).
- **Classification:** Adaptation of established techniques.
- **Confidence:** High.

Two prefixes visiting the same vertex subset `vis` and ending at the same vertex `u` have identical sets of valid suffix completions. Therefore, `hist.check_and_update` prunes a Branch-and-Bound node when its `prefix_cost` is greater than or equal to the best cost previously recorded for `(vis, u)`.

The table maintains incremental Zobrist subset hashes (`h ^ inst_.zobrist[v]`), maps states into `2^20` slots guarded by 4,096 striped mutexes, probes four adjacent slots, and overwrites the home slot on a full-window miss. This combines SOP prefix-dominance pruning with a lock-striped, bounded-probe transposition table from game-tree search.

**Likely sources:** Yaroslav Shylo, Oleg Prokopyev, and Jayant Rajgopal (~2012–2015); J. Jamal, G. Shylo, et al. (~2017); Albert L. Zobrist (~1970); Dennis M. Breuker, Jos W. H. M. Uiterwijk, and H. Jaap van den Herik (~1994); and Maurice Herlihy and Nir Shavit (~2008).

**Search terms:** `"History Utilization" OR "History Table" "Sequential Ordering Problem" Branch and Bound`; `"Prefix Equivalence" branch and bound SOP`; `"Replacement Schemes for Transposition Tables" Breuker`

## 10. Depth-First Search Backtracking

- **Where:** `solver.cpp` (`bnb_worker` / `dfs`, lines 675–711); relies on cost-sorted `adj_out[u]` from `sop_instance.cpp` (lines 199–202).
- **Classification:** Adaptation of an established technique.
- **Confidence:** High.

Inside each subtree, the solver performs recursive in-place DFS backtracking (`vis.set(v)`, `cur_path.push_back(v)`). It iterates over outgoing neighbors `v` in ascending order of `cost[u][v]`, and prunes candidates when `v`'s predecessor bitset is not yet a subset of `vis` (`!inst_.preds[v].is_subset_of(vis, inst_.nwords)`) or when the unique terminal node would be visited before depth `N - 1`.

Depth-first backtracking with nearest-neighbor child ordering and bitset readiness filtering is the standard search backbone of exact SOP Branch-and-Bound solvers.

**Likely sources:** Little, Murty, Sweeney, and Karel (~1963); Balas and Toth (~1985); Norbert Ascheuer et al. (~1993); and Shylo et al. (~2015).

**Search terms:** `"Branch and Bound" "Sequential Ordering Problem" bitset precedence readiness DFS`

## 11. Cooperative Parallel Work-Stealing BnB and Concurrent LNS

- **Where:** `solver.cpp` (`run_parallel_bnb_and_lns`, lines 616–617, 713–770); `shared_state.hpp` (`update_incumbent`, lines 49–61).
- **Classification:** Adaptation of established techniques.
- **Confidence:** High.

When running on at least four threads, the solver splits the pool into three-quarters exact Branch-and-Bound workers (`bnb_threads`) and one-quarter heuristic workers (`lns_threads`). BnB workers dynamically claim depth-2 subtrees via lock-free `task_idx.fetch_add(1)`, while LNS workers continuously run Ruin-and-Recreate LNS plus VND on the current global incumbent until `bnb_finished` is set.

All threads read the shared upper bound through lock-free `std::memory_order_relaxed` atomic loads and publish improvements through a double-checked mutex lock (`update_incumbent`). Heuristic improvements can therefore immediately prune active BnB subtrees.

**Likely sources:** Teodor Gabriel Crainic, Bertrand Le Cun, and Catherine Roucairol (~2006); Vipin Kumar and V. Nageshwara Rao (~1987); and Douglas C. Schmidt and Tim Harrison (~1997, *Double-Checked Locking*).

**Search terms:** `"Parallel Branch-and-Bound" cooperative metaheuristic upper bound Crainic`; `parallel branch and bound concurrent LNS incumbent sharing`

---

Let me know which Branch-and-Bound component you would like to explore in a deeper write-up.
