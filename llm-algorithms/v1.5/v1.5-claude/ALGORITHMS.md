# ALGORITHMS.md -- inventory of every technique in the program

Conventions used in the tables:

* **Class 1**: `established` = named published algorithm used as is,
  `adaptation` = established algorithm modified for this program, `custom` =
  my own construction.
* **Class 2**: `used` = this algorithm has been used in solving the TSP or SOP;
  `version used` = a version of it has been used in solving the TSP or SOP;
  `not used` = this algorithm has not been used in solving the TSP or SOP (to my
  knowledge).
* Sources are written **from memory and were not re-checked** against the
  literature; where I am unsure I say so. For my own techniques the author is
  "the model".
* A technique that touches several phases is listed once, where it does the
  most work, with a note under the tables.

Reduced-cost space (used by several rows): the assignment relaxation gives duals
`a_i` (rows, all nodes except the end) and `b_j` (columns, all nodes except the
start) with reduced cost `r(i,j) = c(i,j) - a_i - b_j >= 0`. Every start-to-end
path uses each row and column exactly once, therefore
`cost(path) = LB0 + sum of r over its arcs`, with `LB0 = sum a + sum b`.

## TABLE 1 -- PREPROCESSING (before the search starts)

| # | Technique | Where | Description | Class 1 | Class 2 | Source | Reasoning |
|---|---|---|---|---|---|---|---|
| P1 | Tolerant `.sop` tokenizer | `instance.cpp::parseSopFile` | Header lines parsed by keyword; after `EDGE_WEIGHT_SECTION` every integer is read regardless of line wrapping, the repeated `N` is skipped, reading stops at the first non-integer (`EOF`) or end of file. | custom | not used (input handling) | the model | The task requires wrapped matrices and optional `EOF`. |
| P2 | Precedence digraph + Kahn topological sort | `model.cpp::buildModel` | `T[i][j] < 0` gives arc `j -> i` ("j before i"); Kahn's algorithm detects cycles (instance infeasible). | established | version used (precedence graphs in SOP solvers) | Kahn, "Topological sorting of large networks", 1962 | Cheap and exact infeasibility test. |
| P3 | Bitset transitive closure | `model.cpp::buildModel` | Ancestor and descendant sets per node computed in topological order with word-wise OR of bit rows (`O(N * edges / 64)`). | adaptation of Warshall's closure | version used (closure/inference of precedences in SOP preprocessing) | Warshall, "A theorem on Boolean matrices", 1962 | Gives O(1) "must precede" queries used everywhere below. |
| P4 | Start/end derivation and zero-cost dummy nodes | `model.cpp::buildModel`, `Model::toRealPath` | Start = sources of the closure, end = sinks. If several, a dummy start (zero-cost arcs to every source) and/or dummy end (zero-cost arcs from every sink) is appended so all later code sees one start `s` and one end `e`. Dummies are stripped from the output. | custom | version used (dummy node to turn a path problem into a tour problem is common TSP practice) | the model | Satisfies "derive start/end, find the optimal ones" without special cases in the solver. |
| P5 | Static arc elimination | `model.cpp::buildModel` | Arc `(i,j)` removed if `j` must precede `i`, or some node `k` must lie strictly between (`desc(i) & anc(j) != 0`), or `i = e`, or `j = s`. The uneliminated costs are kept in `Model::raw` for the heuristic. | adaptation | used in SOP (arc elimination by precedence in the branch-and-cut preprocessing; recollection, not re-verified) | Ascheuer, Juenger, Reinelt, "A branch & cut algorithm for the asymmetric traveling salesman problem with precedence constraints", 2000 | Removes arcs that no feasible path can use; tightens every bound. |
| P6 | Cover relation (transitive reduction) | `model.cpp::buildModel` | `p` covers `x` if `p` precedes `x` and no node lies between (`desc(p) & anc(x) = 0`); stored as `preds` / `succs` lists. | adaptation | not used (as far as I know) | Aho, Garey, Ullman, "The transitive reduction of a directed graph", 1972 | Minimal edge set: availability counters and feasibility checks touch few entries. |
| P7 | Root assignment bound + duals (Hungarian, shortest augmenting paths) | `ap_bound.cpp::solveAssignmentBound` | Min-cost assignment on rows = nodes except `e`, columns = nodes except `s`, dense `O(N^3)`; keeps the optimal potentials and the matching. Aborts (zero duals) on the stop flag. | established | used (AP relaxation for ATSP/SOP) | Kuhn 1955; Munkres 1957; Jonker & Volgenant, "A shortest augmenting path algorithm for dense and sparse linear assignment problems", 1987; Carpaneto & Toth, "Some new branching and bounding criteria for the asymmetric TSP", 1980 | Strong bound when precedence is sparse; the duals feed bounding, fixing and heuristics. Value on R_200_1000_1 (1392) matched scipy's assignment solver. |
| P8 | Reduced-cost arc fixing + CSR arc lists | `exact_search.cpp::SharedSearch::buildArcLists` | Arcs with `r > UB - 1 - LB0` cannot be in an improving path and are dropped; remaining arcs stored in compressed rows sorted by reduced cost (out-lists and in-lists). Also detects "some node has no admissible arc" (then the incumbent is optimal). | adaptation | version used (reduced-cost fixing is textbook; applied to ATSP B&B) | Nemhauser & Wolsey, "Integer and Combinatorial Optimization", 1988 | Sorted lists allow early loop exits; fixing shrinks the graph before search. |
| P9 | Candidate lists (K=12) by raw or reduced cost | `heuristic.cpp::buildHeuristicData` | For each node the K best feasible predecessors/successors; ranked by raw cost first, rebuilt by reduced cost once the AP duals exist. | adaptation | version used (neighbour lists; reduced-cost ranking is similar in spirit to alpha-nearness) | Johnson & McGeoch 1997; Helsgaun, "An effective implementation of the Lin-Kernighan traveling salesman heuristic", 2000 | Restricts local-search moves to promising arcs. |
| P10 | NUMA / CPU topology discovery | `numa_topology.cpp::discover` | Reads `/sys/devices/system/node/*` (CPUs, memory per node) and sibling lists; builds a pin order: memory-owning nodes first, round-robin, one hardware thread per core before SMT siblings. | custom | not used | the model | The 2990WX has dies without local memory; nothing hard-coded. Not exercised on real hardware (sandbox had one core). |
| P11 | Command-line parsing | `main.cpp` | Positional file, `--time-limit`, `--threads`, `--output`, `--no-header` plus extras. | custom | not used | the model | Interface required by the task. |

Notes: the Zobrist tables used by the dominance table are created in the
constructor of `SharedSearch` but listed in 2d. Initial UB construction is in 2b.

## TABLE 2 -- COMPUTATION (finding solutions and proving optimality)

### 2a. Search strategy

| # | Technique | Where | Description | Class 1 | Class 2 | Source | Reasoning |
|---|---|---|---|---|---|---|---|
| S1 | Depth-first branch and bound over path prefixes | `exact_search.cpp::Worker::runTask/push/pop/enter` | A node is a prefix `s -> ... -> v`; children are the unvisited nodes whose cover-predecessors are all visited (counters `cnt`). Iterative stack with an undo log. | established (B&B) | used (SOP/TSP) | Land & Doig 1960; Escudero, "An inexact algorithm for the sequential ordering problem", 1988 (B&B on SOP; recollection) | Natural for SOP: precedence is enforced by availability, no cycles to eliminate. |
| S2 | Child ordering by reduced cost | `Worker::runTask` | Children are tried in ascending reduced arc cost (lists are pre-sorted); the scan stops at the first arc exceeding the budget. | adaptation | version used (nearest-first ordering) | the model (nearest-neighbour style ordering) | Finds good solutions early and allows early loop exit. |
| S3 | Iterative deepening on the reduced-cost budget | `exact_search.cpp::SharedSearch::run` | Budget B = 0, 1, 2, 3, 5, ... (B <- max(B+1, 1.5B)). Iteration B explores exactly the prefixes whose bound is `<= B` (and `<= UB-1-LB0`). A completed iteration proves "no path of cost <= LB0+B" (logged as a raised lower bound). Hash keys are salted per iteration. | adaptation of IDA* | version used (IDA* on SOP with abstraction bounds) | Korf, "Depth-first iterative-deepening", 1985; Hernadvolgyi, "Solving the sequential ordering problem with automatically generated lower bounds", 2003 (recollection) | Turns the search into heuristic + exact: on R_200_1000_1 the optimum was found during the eighth iteration (index 7) although the ILS bound at that point was far worse. |

### 2b. Upper bounding / heuristics

| # | Technique | Where | Description | Class 1 | Class 2 | Source | Reasoning |
|---|---|---|---|---|---|---|---|
| U1 | Randomized nearest-neighbour construction | `heuristic.cpp::IlsWorker::construct` | From `s`, repeatedly pick among the 3 cheapest *available* nodes (70/20/10 %), by raw cost, or by reduced cost after the duals exist. | established | used (TSP/SOP) | Rosenkrantz, Stearns, Lewis 1977 (NN analysis) | Always precedence-feasible; cheap diversification. |
| U2 | AP cycle patching with deferral and cheapest feasible reinsertion | `IlsWorker::constructAp` | Optimal AP = one s-e path + cycles. Cycles are spliced into the path at the cheapest (path arc, cycle arc) pair; the sequence is scanned keeping nodes that do not conflict with kept descendants; conflicting nodes are reinserted one by one at the cheapest position inside the interval allowed by their kept ancestors/descendants. Randomized variants pick among the best 3. | adaptation | version used (patching for ATSP) | Karp, "A patching algorithm for the nonsymmetric TSP", 1979; Rosenkrantz et al. 1977 (cheapest insertion) | Gave near-bound solutions on ATSP-like instances (N=500 synthetic: 1471 vs bound 1457 immediately). |
| U3 | Precedence-aware segment-insertion local search | `IlsWorker::tryMove/feasibleMove/applyMove/localSearch` | Move segment `p[i..j]` (length <= 30, no reversal) behind a candidate node; cost delta in O(1); feasibility by comparing the target with the first successor / last predecessor positions of the segment (cover edges suffice). First move chosen = best improving candidate of the segment. | adaptation | used in SOP (Or-opt / 3-exchange without reversal) | Or, PhD thesis 1976; Gambardella & Dorigo, "An ant colony system hybridized with a new local search for the SOP", 2000 | Asymmetric costs forbid segment reversal; precedence tested cheaply. |
| U4 | Work stack of touched nodes | `IlsWorker::pushNode/localSearch` | Only nodes whose neighbourhood changed are re-examined (don't-look-bit idea). | adaptation | version used (TSP) | Bentley, "Fast algorithms for geometric traveling salesman problems", 1992 | Makes ILS iterations cheap. |
| U5 | Iterated local search with local kicks | `IlsWorker::kick/run` | Kick = 2-4 random feasible segment relocations whose target lies within +-20..80 positions; local search; accept if not worse, otherwise restore; after a stall restart from the shared best (strong kick) or a fresh construction. Local kicks were much better than global ones (see DEVELOPMENT.md). | established (ILS), custom kick shape | version used (ILS for TSP) | Lourenco, Martin, Stuetzle, "Iterated local search", 2003 | Simple, parallel, no precedence-specific machinery. |
| U6 | Warm-up scheduling with stall window | `solver.cpp::solveModel` | ILS runs on all threads while the main thread solves the AP; warm-up lasts `0.003*N` s (clamped) and is extended while the incumbent keeps improving, capped by 30 s or 25 % of the time limit. | custom | not used | the model | Avoids a fixed guess for the heuristic phase; skipped for N <= 12. |

### 2c. Lower bounding

| # | Technique | Where | Description | Class 1 | Class 2 | Source | Reasoning |
|---|---|---|---|---|---|---|---|
| L1 | Cheapest-allowed-in-arc bound, incremental | `Worker::push/pop` | For every unvisited node the cheapest reduced in-arc from an allowed predecessor (unvisited or current); one pointer per node into its sorted in-list, advanced only for nodes pointing at the node just left, with an undo log. A node with no allowed predecessor is a dead end. | adaptation | version used (column-reduction bounds in TSP B&B) | Little, Murty, Sweeney, Karel, "An algorithm for the traveling salesman problem", 1963 | O(out-degree) per move; also serves as a cheap filter before L2. |
| L2 | Incremental assignment bound | `Worker::apStep` | Per depth the optimal assignment of the remaining problem is kept (mates and potentials). A move removes the current row and the chosen column; if their partners differ, one shortest augmenting path (Dijkstra with a binary heap on reduced costs, potentials updated, search capped by the remaining budget) restores optimality. Bound = dual objective. Snapshots per depth are plain array copies. | adaptation | version used (AP bound in ATSP/SOP B&B); the single-augmentation prefix variant is mine | Jonker & Volgenant 1987; Dijkstra 1959; Carpaneto & Toth 1980 | Detects Hall-type infeasibility the in-arc bound cannot (first rbg048a iterations: 5.55M -> 13k and 38.9M -> 151k nodes). |

### 2d. Pruning, dominance, symmetry/redundancy elimination

| # | Technique | Where | Description | Class 1 | Class 2 | Source | Reasoning |
|---|---|---|---|---|---|---|---|
| D1 | Strict bound pruning | `Worker::push`, `enter` | A child is pruned if `g_red + bound > min(UB-1-LB0, B)`; only completions with cost `>=` the incumbent are discarded, so strict improvement is never lost. | established | used | Land & Doig 1960 | Basis of the proof logic (2f). |
| D2 | Held-Karp dominance through a shared transposition table | `transposition_table.cpp::probeAndStore`, `Worker::enter` | State = (set of visited nodes, last node). Reaching the same state with reduced cost `>=` a stored one is pruned; cheaper arrivals overwrite. Path cost is identical to reduced cost up to a constant for a given state, so reduced cost is compared. | adaptation | version used (DP dominance for TSP/SOP) | Held & Karp, "A dynamic programming approach to sequencing problems", 1962 | Removes permutations of the same set; pure DP over precedence ideals is infeasible (see DEVELOPMENT.md). |
| D3 | Zobrist hashing with two independent 64-bit tables | `exact_search.cpp` (constructor, `push`, `pop`) | XOR of per-node random keys for the visited set, XOR of a per-last-node key; 128-bit identity per state. | established | not used (to my knowledge) | Zobrist, "A new hashing method with application for game playing", 1970 | O(1) incremental keys. A collision could cause a wrong prune (probability negligible, not zero). |
| D4 | Lock-less XOR check word | `TranspositionTable::probeAndStore` | Each 32-byte slot stores `key1, key2, data, key1^key2^data`; torn concurrent writes fail the check and count as empty. | adaptation | not used | Hyatt & Mann, "A lock-less transposition table implementation for parallel search chess engines", 2002 | Lock-free sharing across threads. |
| D5 | Two-slot buckets: depth-preferred + always-replace | `TranspositionTable::probeAndStore` | Slot 0 keeps shallow (large-subtree) states; slot 1 always replaced; lossy table, only speed is affected. | adaptation | not used | Breuker, Uiterwijk, van den Herik, "Replacement schemes and two-level tables", 1996 | Bounded memory. |
| D6 | Per-iteration hash salt | `SharedSearch::run`, `Worker::initRoot` | A fresh random salt is XORed into every key of a budget iteration. | custom | not used | the model | Entries from a smaller budget are invalid for a larger one; salting avoids clearing a multi-GB table. |

### 2e. Parallelization and NUMA handling

| # | Technique | Where | Description | Class 1 | Class 2 | Source | Reasoning |
|---|---|---|---|---|---|---|---|
| E1 | Dynamic work splitting with prefix tasks | `SharedSearch::acquire/donate`, `Worker::tryDonate/runTask` | Shared queue of path-prefix tasks; `demand` counts idle workers; a busy worker donates the unexplored siblings of its shallowest open frame (checked against the prefix at that frame's time); receivers replay the prefix. Termination when all workers idle and the queue is empty. | adaptation | not used | Rao & Kumar, "Parallel depth first search", 1987 | Subtree sizes are unpredictable; splitting on demand balances load without a static partition. |
| E2 | Shared atomic incumbent with logging | `incumbent.cpp::Incumbent::offer` | Lock-free cost read for pruning, mutex-protected update of cost and path, a stderr line `new best cost ...` per global improvement. | custom | not used | the model | All threads prune with the best cost found by any thread. |
| E3 | NUMA-aware thread pinning | `numa_topology.cpp::pinCurrentThread` | Thread `i` pinned to the `i`-th CPU of the discovered order (see P10). | custom | not used | the model | Predictable placement; first touch then lands on the right node. Untested on the target CPU. |
| E4 | Interleaved lazily mapped transposition table | `numa_topology.cpp::numaAllocInterleaved`, `TranspositionTable` ctor | `mmap` with `MAP_NORESERVE`, `MADV_HUGEPAGE`, `mbind(MPOL_INTERLEAVE)` over memory-owning nodes; pages are only touched when used. | custom | not used | the model | Spreads bandwidth of the random-access table; no start-up cost for small instances. Untested on NUMA hardware. |
| E5 | Per-thread state allocated after pinning | `Worker::run/initRoot` | All DFS arrays, snapshots and the undo log are allocated by their owner thread. | custom | not used | the model | Local memory for the hot data. |
| E6 | Hybrid thread allocation | `solver.cpp::solveModel` | Warm-up ILS on all threads while the main thread solves the AP; during the exact phase `max(1, T/8)` ILS threads keep running (for T >= 4). | custom | not used | the model | A hopeless proof still improves the incumbent; a good incumbent shrinks the budget. |
| E7 | Watchdog thread for the time limit | `solver.cpp::solveModel` | Sets the global stop flag at the deadline; exact workers also poll every 128 nodes. | custom | not used | the model | Limit applies even inside long phases. |

### 2f. Optimality proof and termination logic

| # | Technique | Where | Description | Class 1 | Class 2 | Source | Reasoning |
|---|---|---|---|---|---|---|---|
| F1 | Exhaustion proof | `SharedSearch::run/acquire` | `Optimal = yes` iff the final iteration ended with all workers idle, empty queue and no worker interrupted by the stop flag. The final iteration is the one with budget `>= UB-1-LB0` (budget unrestricted). | custom | version used (B&B optimality by exhaustion) | the model | Combined with strict pruning (D1) this is a complete proof relative to the 128-bit hash assumption. |
| F2 | Early exits | `SharedSearch::run`, `main.cpp` | If some node has no admissible arc within the gap, the incumbent is optimal immediately; cyclic precedence gives `-1`, empty path, `no`. | custom | not used | the model | Cheap termination cases. |
| F3 | Time-limit handling | `solver.cpp`, `exact_search.cpp::acquire` | On timeout the best incumbent is reported with `no`; if nothing was found: `-1`, empty path, `no`. | custom | not used | the model | Required output behaviour. |

## TABLE 3 -- POSTPROCESSING

| # | Technique | Where | Description | Class 1 | Class 2 | Source | Reasoning |
|---|---|---|---|---|---|---|---|
| O1 | Dummy removal | `Model::toRealPath` | Drops dummy start/end from the working path. | custom | not used | the model | Output uses real nodes only. |
| O2 | Independent validity check | `output.cpp::validatePath`, `main.cpp` | Re-checks permutation, every `T[i][j] < 0` constraint on the raw matrix, recomputes the cost from scratch. A failing path is never printed (`-1`, empty, `no`, logged as internal error). | custom | not used | the model | Guards against bugs anywhere upstream. |
| O3 | CSV formatting and writing | `output.cpp::csvRowLine/emitCsv` | Row to stdout (header unless `--no-header`); with `--output`, append, header only for a new or empty file; names with commas quoted. | custom | not used | the model | Output format required by the task. |

## Cross-phase notes

* AP root solve (P7) also provides the lower bound used in logging and in D1.
* Reduced costs are produced in Table 1 but consumed in 2a-2d and 2b.
* Transposition-table sizing (default quarter of free RAM, max 16 GiB) lives in
  `solver.cpp` and is part of E4.
