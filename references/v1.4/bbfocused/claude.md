**Caveat.** This describes what the code resembles. I'm not recalling how I wrote it, and I did no web check, so every title, author and year below is from memory and approximate. I say so where I'm unsure. Classification key: **E** = established named algorithm, **A** = adaptation of an established one, **C** = custom heuristic or engineering choice. "Claude (this session)" as author means the specific combination or parameter is mine, not that the underlying idea is new.

## A. Search structure

| # | Technique | Where | Class | Confidence and why | Source and search terms |
|---|---|---|---|---|---|
| 1 | **Depth-first branching by path extension.** Each node is a partial path from the start. Its children append one precedence-ready node reachable by an allowed arc. | `Worker::expand`, `enterChild`, `generateCands` (bnb.cpp) | E | High. Forward-construction DFS is the standard tree for TSP-path and SOP branch-and-bound. | Shobaki & Jamal, "An exact algorithm for the sequential ordering problem and its application to switching energy minimization in compilers," ~2015. Also Hernádvölgyi, ~2003 (title approximate). Search: *branch and bound sequential ordering problem path extension*. |
| 2 | **Branching restricted to feasible candidates.** Only unvisited nodes with no unvisited predecessors (`rem[j]==0`) and a non-forbidden arc are tried. | `generateCands` | E | High. Precedence-feasible extension is inherent to any SOP tree search. | Same as #1. |
| 3 | **Child ordering by increasing reduced cost.** Candidates are sorted by `cost(last,j) − u[last] − v[j]`, so the arc the assignment relaxation likes best is explored first. | `generateCands` | A | Medium. Ordering by reduced cost is a known branching heuristic. I can't name a paper that does it for this tree. | Search: *branching order reduced cost assignment relaxation*, *Carpaneto Dell'Amico Toth ATSP*. Source uncertain. |
| 4 | **Leaf shortcut.** When only the end node is unvisited, the final arc is added and the path is compared with the incumbent. No bound is computed. | `expand` (`acount == 1`) | C | High. Trivial special case. | Author: Claude (this session). |

## B. Lower bound

| # | Technique | Where | Class | Confidence and why | Source and search terms |
|---|---|---|---|---|---|
| 5 | **Assignment-problem (AP) relaxation on the remaining problem.** A closing arc `end→last` at cost 0 makes the completion a cycle. The optimal assignment over `R ∪ {last}` is a lower bound on the completion cost. | `enterChild`, `augment`, `computeRootAP` | E / A | High for the AP bound in ATSP-type B&B, medium for the path-with-precedence form. The arc-restriction rules (e.g. `last` only to ready nodes) are my adaptation. | Little, Murty, Sweeney & Karel, "An algorithm for the traveling salesman problem," 1963. Carpaneto & Toth, 1980 (ATSP B&B). Carpaneto, Dell'Amico & Toth, "Exact solution of large-scale asymmetric TSP," ACM TOMS, ~1995. Ascheuer, Escudero, Grötschel & Stoer, "A cutting plane approach to the SOP," SIAM J. Optimization, ~1993. Search: *assignment relaxation asymmetric TSP branch and bound*. |
| 6 | **Shortest-augmenting-path Hungarian algorithm with dual potentials.** It solves the AP and returns row and column duals. | `Worker::augment` | E | High. The structure (minimum slack array, way pointers, delta update, dummy root column) matches the standard textbook and competitive-programming form. | Kuhn, 1955. Munkres, 1957. Edmonds & Karp, 1972. Jonker & Volgenant, "A shortest augmenting path algorithm for dense and sparse linear assignment problems," 1987. Search: *Hungarian algorithm potentials shortest augmenting path*. |
| 7 | **Root AP from scratch.** Column reduction, greedy matching of tight arcs, then augmentation of unmatched rows. | `computeRootAP` | A | Medium. It resembles the Jonker-Volgenant initialization, but I implemented a simplified version. | Jonker & Volgenant, 1987. |
| 8 | **Incremental dual warm start between parent and child.** The child copies the parent's duals and matching. It deletes the row and column of the old `last`, retargets column `j` to the closing arc, and repairs at most two unmatched rows. | `enterChild` | A | Medium-low on lineage. Reusing AP solutions across B&B nodes is known, but I can't recall a published description of this exact patch. The specifics may be my own derivation. | Possibly related: Carpaneto, Dell'Amico & Toth, ~1995. Search: *assignment problem reoptimization branch and bound dual feasible warm start*. Source uncertain. |
| 9 | **Infeasibility pruning.** If an augmenting search finds no perfect matching, the node is cut. | `augment` (`return false`) | E | High. An infeasible relaxation implies an infeasible node. | Standard B&B (Land & Doig, 1960). |

## C. Upper bound (incumbent)

| # | Technique | Where | Class | Confidence and why | Source and search terms |
|---|---|---|---|---|---|
| 10 | **Initial UB from an iterated local search (ILS).** A short parallel ILS runs before the tree search. | `solve` (solver.cpp), `IlsWorker` (heuristic.cpp) | A | High that this is the pattern "heuristic first, exact second". The ILS itself is a simple version. | Lourenço, Martin & Stützle, "Iterated Local Search," *Handbook of Metaheuristics*, ~2003. Resembles the heuristic-plus-B&B hybrid in Shobaki et al., ICPP-W 2023 (exact title unsure). Search: *branch and bound LKH hybrid sequential ordering*. |
| 11 | **Helper ILS threads during the search.** From 8 threads up, `threads/8` of them keep running ILS in the background and sometimes restart from the shared best. | `solve`, `IlsWorker::run` | C | Medium. Concurrent heuristic and exact threads is a common pattern, but the 1-in-8 split is my arbitrary choice. | Author: Claude (this session). |
| 12 | **Incumbents found by the tree search itself.** Leaves and "completion check" hits update the incumbent. Children are visited best-first by reduced cost, which makes DFS act like a dive. | `expand`, `tryCompletion` | A | Medium. DFS dives finding incumbents is standard. The effectiveness here comes from the AP-guided ordering. | Search: *depth-first branch and bound primal heuristic dive*. |
| 13 | **AP tour completion check.** If the optimal assignment is one cycle that satisfies all precedences, that cycle is the best completion of the node and the node is closed. | `tryCompletion` | A | Medium-high. "AP solution is a tour, so it's optimal" is classic for TSP. The precedence feasibility check is the adaptation. | Eastman, 1958. Little et al., 1963. Carpaneto & Toth, 1980. Search: *assignment relaxation subtour-free solution optimal*. |

## D. Pruning rules

| # | Technique | Where | Class | Confidence and why | Source and search terms |
|---|---|---|---|---|---|
| 14 | **Bound-versus-incumbent pruning.** A node is cut when `g + LB ≥ UB`. The search looks only for strictly better solutions. | `enterChild`, `expand` | E | High. This is the basic B&B rule. | Land & Doig, 1960. |
| 15 | **Reduced-cost child bound with an early `break`.** A child's bound is `g + obj + rc(last,j)`, available in O(1) before any matching work. Since children are sorted, the loop stops at the first failure. | `expand` | A | Medium-high. It is reduced-cost reasoning applied to the arc just chosen. I don't know a specific source for the break-on-sorted-list form. | Crowder, Johnson & Padberg, "Solving large-scale zero-one linear programming problems," 1983 (reduced-cost fixing). Search: *reduced cost fixing*. |
| 16 | **Mid-augmentation cutoff.** The dual objective rises monotonically during augmentation, so the node is cut as soon as `g + obj ≥ UB`. | `augment` (`obj += delta`) | A | Medium-low on lineage. Monotone duals in primal-dual AP methods are known. Using them for early cutoff is a natural step, but I can't cite it. | Search: *dual ascent early termination bound cutoff*. Source uncertain. |
| 17 | **Pre-check before the full node computation.** The cheap parent-based bound (#15) runs first, then the history lookup (#18), and only then the O(m²) matching update. | `expand`, `enterChild` | C | High. This is an ordering of checks by cost. | Author: Claude (this session). |

## E. Dominance (history table)

| # | Technique | Where | Class | Confidence and why | Source and search terms |
|---|---|---|---|---|---|
| 18 | **History-based dominance on (visited set, last node).** An arrival is pruned if the same state was already reached with equal or lower prefix cost. A cheaper arrival overwrites the entry and is explored again. | `expand`, `MemoTable::checkAndInsert` | E / A | High on the idea. Medium on the exact lineage and on whether it matches their variant. The state-equivalence argument is Held-Karp's. Published SOP B&B uses it as a pruning rule. | Held & Karp, 1962. Bellman, 1962. Shobaki & Jamal, ~2015. Parallel follow-up in the *Journal of Parallel and Distributed Computing*, ~2023 (title unsure). Hernádvölgyi, ~2003. Search: *history-based domination sequential ordering problem*. |
| 19 | **Zobrist-hashed, lock-free table with XOR verification.** It uses a 64-bit key plus a 14-bit tag, buckets of 4, and evicts the deepest entry. | `MemoTable`, Zobrist keys in `runBranchAndBound` | E / A | High on each component. The combination is my engineering choice. | Zobrist, 1970. Hyatt & Mann, lock-less transposition tables, ICGA Journal, ~2002 (citation approximate). Breuker, Uiterwijk & van den Herik, replacement schemes for transposition tables, ~1994. Search: *lockless transposition table xor trick*. |
| 20 | **Table threshold.** States with fewer than 5 unvisited nodes are not stored. The table is also disabled if path costs could overflow its 40-bit field. | `expand`, `runBranchAndBound` | C | High. Tuning constant and safety guard. Not tuned empirically. | Author: Claude (this session). |

## F. Parallel B&B

| # | Technique | Where | Class | Confidence and why | Source and search terms |
|---|---|---|---|---|---|
| 21 | **Shared task pool with a busy-counter termination test.** Search is complete only when the pool is empty and no worker is busy. | `Shared::getTask/finishTask` | E | High. Standard pool and idle-detection pattern. | Gendron & Crainic, "Parallel branch-and-bound algorithms: survey and synthesis," Operations Research, 1994. |
| 22 | **On-demand donation of shallowest unexplored siblings.** A busy thread splits off work only when a thread is idle and the pool is empty. Subtrees with fewer than 10 unvisited nodes are never split. | `maybeDonate` | A | Medium. It is related to work sharing and work stealing. The shallowest-first rule is a common heuristic, but I can't name a source for this exact policy. | Blumofe & Leiserson, "Scheduling multithreaded computations by work stealing," JACM, 1999 (related). Search: *dynamic load balancing parallel depth-first branch and bound*. |
| 23 | **Task as prefix plus sibling list, with replay.** The receiver walks the prefix to rebuild its duals and matching, then explores the donated siblings. | `Task`, `runTask` | C | Medium. Recomputing state from a prefix is a known pattern. I can't name a source. | Author: Claude (this session). |
| 24 | **Shared atomic incumbent.** The cost is read lock-free, and improvements take a lock. | `Incumbent` | E | High. Standard shared-bound pattern. | Gendron & Crainic, 1994. |

## What is not in the B&B

- **Branching style:** no subtour-elimination branching or branching on arcs.
- **LP and cuts:** no LP relaxation, cutting planes or Lagrangian bounds.
- **Search order:** no best-first search and no strong branching.
- **Arc elimination:** I tried reduced-cost arc elimination during development and removed it. It didn't reduce node counts on `prob_42` and slowed the search.
- **Dominance:** only exact-state dominance, not the more general rules that may exist in the literature.

## Where to verify first

- **#8 and #16:** these are where I'm least sure of the lineage.
- **#18:** compare against the Shobaki papers you're reading. Their lower bound and dominance details may differ from mine, and I don't remember their lower-bound technique well enough to compare.
- **#22 and #23:** these may differ from any published scheme.

I can go deeper on any of these items.