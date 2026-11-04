**Caveat before the table.** This describes what the code *resembles*. I'm not recalling how I wrote it, so a "custom" label means I don't recognise a specific source, not that the idea is new. All sources are cited from memory with no web check, so treat titles and years as approximate and verify them with the search terms. I name a source only where I'm reasonably sure it exists, and I flag the rest. Where I list "Claude (this session)" as author, the specific combination or detail is mine, but the underlying idea usually has precedents, which I note.

Classification key: **E** = established named algorithm, **A** = adaptation of an established one, **C** = custom heuristic or engineering choice.

## A. Preprocessing and problem model

| # | Technique | Where | Class | Confidence and why | Likely source and search terms |
|---|---|---|---|---|---|
| 1 | Transitive closure by Warshall with bitset rows | `buildProblem` (instance.cpp) | E (bitset form is A) | High. It is the standard Boolean-matrix closure loop, with 64-bit word OR instead of per-bit work. | Warshall, "A theorem on Boolean matrices," 1962. Search: *Warshall transitive closure bitset*. |
| 2 | Cycle detection via closure diagonal | `buildProblem` | E | High. A node that precedes itself in the closure means a cycle. | Standard; same Warshall sources. |
| 3 | Transitive reduction (covering relation / Hasse diagram) via `succ[a] ∩ pred[b] = ∅` | `buildProblem` (`covering`, `hasseEdges`) | A | Medium-high. The test is the usual "no intermediate node" characterization, done with bitsets. | Aho, Garey, Ullman, "The transitive reduction of a directed graph," 1972. Search: *transitive reduction DAG Hasse*. |
| 4 | Static arc elimination: forbid `i→j` if `j` must precede `i`, or if something is forced between them | `buildProblem` (cost = `kForbidden`) | A | Medium. Arc elimination from precedences is standard SOP preprocessing, but I'm unsure the "forced in between" rule matches a specific paper. | Ascheuer, Jünger, Reinelt, 2000 (branch & cut for ATSP with precedences). Also Gouveia & Pesneau, 2006 (precedence-constrained ATSP formulations). Search: *SOP preprocessing infeasible arcs transitive closure*. |
| 5 | Dummy source/sink nodes with zero-cost arcs to handle multiple possible starts and ends | `buildProblem` (`addedSource`, `addedSink`) | A | Medium. Super-source/sink is a generic modeling trick; I don't know a SOP paper that does it exactly this way. | Generic network-modeling folklore. Search: *super source super sink Hamiltonian path reduction*. |
| 6 | Path-to-cycle transformation: closing arc `end→last` at cost 0 | AP setup in `augment` / `enterChild` | E | High. This is the standard way to apply TSP/ATSP relaxations to Hamiltonian-path problems. | Used throughout the ATSP-with-precedences literature, e.g. Ascheuer et al. 2000. Search: *Hamiltonian path to cycle dummy arc ATSP*. |
| 7 | Kahn-style predecessor counters to track "ready" nodes | `rem[]`, `hasseSucc` in bnb.cpp; `construct` in heuristic.cpp | E | High. This is the in-degree bookkeeping from topological sorting, applied incrementally and undone on backtrack. | Kahn, "Topological sorting of large networks," 1962. |

## B. Lower bound (assignment problem)

| # | Technique | Where | Class | Confidence and why | Likely source and search terms |
|---|---|---|---|---|---|
| 8 | Assignment-problem relaxation as the lower bound | `augment`, `computeRootAP`, `enterChild` | E (AP bound for TSP-like problems) | High for ATSP; medium for the path-with-precedence use. AP bounds are the classic ATSP B&B bound. | Little, Murty, Sweeney, Karel, 1963; Carpaneto & Toth, 1980; Carpaneto, Dell'Amico, Toth, 1995. For SOP, see Escudero 1988 and Ascheuer et al. Search: *assignment relaxation asymmetric TSP branch and bound*. |
| 9 | Shortest-augmenting-path Hungarian with dual potentials (u, v) | `Worker::augment` | E | High. The loop (minv, way, used, delta update, root dummy column) matches the common competitive-programming "Hungarian with potentials" implementation. | Kuhn 1955; Munkres 1957; Edmonds & Karp 1972 (potentials); Jonker & Volgenant 1987. Search: *Jonker Volgenant shortest augmenting path*, *Hungarian algorithm potentials e-maxx*. |
| 10 | Column reduction plus greedy tight-arc matching as the root initialization | `computeRootAP` | A | Medium. It resembles Jonker-Volgenant's column reduction, but I only implemented a simplified version. | Jonker & Volgenant 1987 (initialization phases). |
| 11 | Warm-starting a child's duals and matching from the parent (delete row/col, retarget column j to the closing arc, repair at most 2 free rows) | `enterChild` | A | Medium. Reusing duals after small changes is standard AP reoptimization. I don't recognise a published description of this exact patch, so the specifics may be mine. | Related: Carpaneto & Toth 1980 (AP-based B&B reoptimization). Search: *assignment problem reoptimization after deleting row column*, *dual feasible warm start Hungarian*. Source uncertain. |
| 12 | Monotone dual objective tracked as `obj += delta`, with mid-augmentation cutoff when `g + obj ≥ UB` | `augment` | A | Medium. The monotonicity is a known property of primal-dual AP methods. Using it for early cutoff against an incumbent is a natural step, but I can't point to a paper. | Search: *dual ascent early termination assignment bound cutoff branch and bound*. Source uncertain. |
| 13 | Reduced-cost bound for each child, `g + obj + rc(last,j)` | `expand` | A | Medium-high. It is reduced-cost reasoning applied to the arc just chosen. | Reduced-cost fixing: Crowder, Johnson, Padberg, 1983 (zero-one programming). Search: *reduced cost fixing*. |
| 14 | Children sorted by increasing reduced cost, with a `break` at the first one that fails the bound | `generateCands`, `expand` | A / C | Medium. Sorting plus break is simple and standard in spirit, but I don't know a specific source for this exact rule. | Search: *branching order reduced cost assignment bound*. Source uncertain. |
| 15 | Completion check: if the optimal AP is a single cycle that obeys precedences, it is the best completion and the node closes | `tryCompletion` | A | Medium-high. "AP solution is a tour, so it's optimal" is classic for TSP. The precedence feasibility check is the adaptation. | Eastman 1958; Little et al. 1963; Carpaneto & Toth 1980. Search: *assignment problem solution forms Hamiltonian cycle optimal*. |

## C. Search tree, dominance and memory

| # | Technique | Where | Class | Confidence and why | Likely source and search terms |
|---|---|---|---|---|---|
| 16 | Branching by extending the path with the next node (forward-construction DFS) | `expand`, `enterChild` | E | High. This is the standard tree for SOP and TSP-path B&B. | Shobaki & Jamal, 2015 (SOP B&B, *Computational Optimization and Applications*, as I recall). Search: *Shobaki Jamal exact algorithm sequential ordering problem*. |
| 17 | History-based dominance on state (visited set, last node) | `expand` + `MemoTable::checkAndInsert` | E / A | High on the idea, medium on lineage. It is the Held-Karp state-equivalence argument used as a pruning table. | Held & Karp, 1962; Bellman, 1962. SOP B&B with history domination: Shobaki & Jamal 2015, and the parallel 2023 follow-up (exact title unsure). Also Hernádvölgyi, ~2003 (SOP with generated lower bounds, title approximate). Search: *history-based domination sequential ordering*. |
| 18 | Zobrist hashing of the visited set and last node, updated incrementally | `Shared::zs1..zl2`, `Frame::sh1/sh2` | E | High. XOR of random per-element keys, updated in O(1). | Zobrist, "A new hashing method with application for game playing," 1970. |
| 19 | Lock-free table entries using the XOR check (`a = key ^ data`) so torn writes read as misses | `MemoTable` | E | High. This is the "lockless transposition table" idea from parallel chess engines. | Hyatt & Mann, "A lock-less transposition table implementation for parallel search chess engines," ICGA Journal, ~2002. |
| 20 | Bucket of 4 entries per cache line; evict the deepest-depth entry or an empty slot | `MemoTable::checkAndInsert` | A | Medium-high. Bucketed tables with depth-based replacement are common in game-tree search. Here "depth" means prefix length, so evicting deeper states keeps the more valuable shallow ones. | Breuker, Uiterwijk, van den Herik, "Replacement schemes for transposition tables," ICCA Journal, ~1994. |
| 21 | 14-bit tag plus 64-bit key for collision safety; table disabled if costs could overflow the 40-bit field | `MemoTable`, `runBranchAndBound` | C | High that this is my engineering choice. Tag-plus-key is a common pattern, but the field widths are my own sizing. | Author: Claude (this session). |
| 22 | Skipping the table for states with fewer than 5 unvisited nodes (`kMemoMinRemaining`) | `expand` | C | High. Tuning constant to limit table traffic near leaves. Not tuned empirically in this session. | Author: Claude (this session). |
| 23 | Leaf shortcut: when one node remains, evaluate the final arc directly | `expand` | C | High. Trivial special case. | Author: Claude (this session). |

## D. Parallelism

| # | Technique | Where | Class | Confidence and why | Likely source and search terms |
|---|---|---|---|---|---|
| 24 | Shared task pool, with a busy-counter termination test under a mutex and condition variable | `Shared::getTask/finishTask` | E | High. This is the standard pool plus idle detection for parallel tree search. | Gendron & Crainic, "Parallel branch-and-bound algorithms: survey and synthesis," Operations Research, 1994. Search: *parallel branch and bound termination detection*. |
| 25 | On-demand work donation: a busy thread splits its shallowest unexplored siblings when another thread is idle and the pool is empty | `Worker::maybeDonate`, `expand` | A | Medium. It is related to work stealing, but donation is sender-initiated and the "shallowest frame first" rule is a common heuristic. I don't know a source for this exact policy. | Blumofe & Leiserson, "Scheduling multithreaded computations by work stealing," JACM, 1999 (related). Search: *work sharing vs work stealing*, *dynamic load balancing parallel DFS*. |
| 26 | A task is a path prefix plus sibling candidates; the receiver replays the prefix to rebuild its duals | `Task`, `runTask` | C | Medium. Recomputing state from a prefix is a known pattern, but I can't name a source. | Author: Claude (this session). |
| 27 | No donation from small subtrees (`kDonateMinRemaining = 10`) | `maybeDonate` | C | High. Granularity-control tuning constant. | Author: Claude (this session). |
| 28 | Shared atomic incumbent cost with a lock-protected path copy | `Incumbent` | E | High. Standard shared-bound pattern. | Gendron & Crainic 1994 (shared incumbent). |
| 29 | Hybrid: ILS helper threads (threads ÷ 8) run during the B&B and restart from the incumbent | `solve` (solver.cpp), `IlsWorker::run` | A / C | Medium. This pairs a heuristic with an exact search in the same spirit as the B&B+LKH hybrid you mentioned. I don't know that paper's exact design, so this is resemblance only. | Shobaki et al., 2023 ICPP-W B&B+LKH hybrid (title unsure). Search: *branch and bound LKH hybrid sequential ordering*. |

## E. Heuristic (ILS)

| # | Technique | Where | Class | Confidence and why | Likely source and search terms |
|---|---|---|---|---|---|
| 30 | Iterated local search framework (perturb, local search, accept if no worse, restart) | `IlsWorker::run` | E / A | High on the framework. The acceptance and restart rules are simple variants. | Lourenço, Martin, Stützle, "Iterated Local Search," *Handbook of Metaheuristics*, 2003; Martin, Otto, Felten, 1991. |
| 31 | Or-opt: move a segment of length 1–8 without reversal | `tryMoveSegment` | E / A | High. Reversal is skipped because the problem is asymmetric. | Or, PhD thesis (Northwestern), 1976; Kanellakis & Papadimitriou, "Local search for the asymmetric TSP," 1980. |
| 32 | Precedence-checked scanning: stop extending a move at the first node that blocks it | `tryMoveSegment` | A / C | Medium. Precedence-aware 3-opt for SOP exists, but this simple blocking test may differ from it. | Gambardella & Dorigo, "An ant colony system hybridized with a new local search for the sequential ordering problem," INFORMS J. Computing, 2000 (SOP-3-exchange). |
| 33 | Local double-bridge kick, as a swap of two adjacent blocks of length up to 30 | `kick` | A | Medium-high. It resembles double-bridge restricted to a window, with a precedence check on the swap. | Martin, Otto, Felten, 1991; Johnson & McGeoch, 1997. |
| 34 | Work list with random pop, in the style of don't-look bits | `touch`, `localSearch` | A | Medium. The mechanism matches don't-look bits, but random pop order is my choice. | Bentley, "Fast algorithms for geometric TSP," 1992. |
| 35 | Randomized nearest-ready-node construction (25% random pick, plus noise on ties) | `IlsWorker::construct` | A / C | Medium. It is nearest-neighbour with randomization, in the spirit of GRASP. | Feo & Resende, GRASP, ~1995. Search: *randomized greedy construction GRASP TSP*. |
| 36 | Stagnation restart: adopt the incumbent, rebuild from scratch, or re-kick the thread's best | `IlsWorker::run` | C | High. Ad hoc diversification rule. The `30 + n` limit is untuned. | Author: Claude (this session). |
| 37 | Phase budget: brief ILS on all threads first (≤ 1 s), then B&B | `solve` | C | High. Simple scheduling heuristic. | Author: Claude (this session). |

## F. Data structures, numerics and system level

| # | Technique | Where | Class | Confidence and why | Likely source and search terms |
|---|---|---|---|---|---|
| 38 | Swap-with-last active list with an undo position (sparse-set style) | `removeFromAlist/restoreToAlist` | A | Medium-high. It is a sparse-set removal trick, reversed in LIFO order for backtracking. | Briggs & Torczon, "An efficient representation for sparse sets," 1993. Search: *sparse set swap remove undo*. |
| 39 | Per-depth stacks of duals and matching, copied per child (`memcpy`) | `Ustk/Vstk/Pstk` | C | High. Plain engineering choice to avoid undo logs. | Author: Claude (this session). |
| 40 | Sentinel infinity (`kInf`, `kForbidden`) with guarded subtraction in the Dijkstra-style update | `augment` | C | High. Common numerical hygiene. | Author: Claude (this session). |
| 41 | PRNGs: xorshift64* for the heuristic, splitmix64 for Zobrist keys | `IlsWorker::next`, `splitmix` | E | High on the generators, medium on exact constants. I used the usual multipliers from memory. | Marsaglia, "Xorshift RNGs," 2003; Vigna, 2016 (xorshift*); Steele, Lea, Flood, "Fast splittable pseudorandom number generators," 2014. |
| 42 | NUMA handling: physical-cores-first ordering, round-robin over nodes, thread pinning, per-node first-touch data replicas, `mbind` interleave plus huge pages for the table | `numa.cpp`, `runBranchAndBound` | C | High that it is systems engineering rather than an algorithm. The specific policy is my guess for the 2990WX and is untested on that hardware. | Linux man pages: `sched_setaffinity(2)`, `mbind(2)`, `madvise(2)`. Search: *NUMA first-touch replication read-only data*. |
| 43 | Tolerant `.sop` parsing: keyword headers, repeated-N detection, wrapped matrix, optional EOF | `parseSopFile` | C | High. Format handling per your spec. | Format reference: TSPLIB documentation (Reinelt, 1991 "TSPLIB—A traveling salesman problem library"). |
| 44 | Independent final validation (permutation, precedences, cost recomputed from the raw matrix) | `validatePath`, `main` | C | High. Standard defensive check. | Author: Claude (this session). |

## What to check first

If you verify only a few, these carry the most weight:

- **#11 and #12 (AP warm start, early cutoff).** I'm least sure of their lineage, and they drive most of the node throughput.
- **#17 (history dominance).** Confirm against the Shobaki papers you're reading that the dominance rule matches theirs. I implemented only the exact-state version.
- **#25 and #26 (donation, prefix replay).** These may differ from any published scheme.
- **#42 (NUMA policy).** It has never run on the target CPU.

I can write a deeper breakdown of any of these items.