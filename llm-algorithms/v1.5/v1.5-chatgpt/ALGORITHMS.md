# Algorithm inventory and correctness arguments

The three tables below inventory the implemented techniques. A technique has one
home table; cross-phase calls refer back to that entry. Standard container
operations are described with the technique they implement, rather than listed
as separate solver algorithms. Ancillary verification/benchmark tooling is
identified explicitly in Table 3 and is not part of a normal timed solve.

To keep the seven-column tables readable, classification values have these exact
meanings:

- **E**: established named algorithm.
- **A**: adaptation of an established algorithm.
- **C**: custom algorithm.
- **Used**: this algorithm was used in solving the TSP or SOP.
- **Version**: a version of this algorithm was used in solving the TSP or SOP.
- **New**: this exact custom algorithm has not been used in solving the TSP or SOP
  in the prior sources identified here. This marks an implementation-specific
  choice, not a claim that no similar idea has ever existed. Plumbing receives
  this label when no prior solver-specific attribution is made.

Sources name authors, titles, and years; `[R#]` expands to a linked reference at
the end. A source cell containing only `the model` denotes the custom
implementation/composition. Such a row may still use an established building
block whose own row/source appears separately.

## TABLE 1 — PREPROCESSING

| Technique name | Where it appears in code (file and function/class) | Description of the algorithm | Classification 1 | Classification 2 | Source (author, title, year) | Reasoning for this implementation |
| --- | --- | --- | --- | --- | --- | --- |
| P01 — Command-line contract validation | `src/cli.cpp`: `parse_options`, `number`, `seconds` | Parse one filename and named options; enforce numeric bounds; reject input/output aliases, unsupported modes, and extra filenames. | C | New | the model | Fail clearly before an expensive run or accidental input modification. |
| P02 — Keyword header and exact-count matrix parsing | `src/instance.cpp`: `Instance::read`, `integer`, `trim`, `upper` | Separate metadata from values; validate repeated N; read exactly N² integers across arbitrary line boundaries; accept optional EOF and reject malformed/trailing content. | C | New | the model | Implements the supplied file contract, including free-text comments and nonstandard negative markers. |
| P03 — Exact integer model with explicit headroom | `include/sop/common.hpp`: `Cost`, `INF`, `plus`; `src/instance.cpp`: `Instance::read` | Use int64 objectives/duals; cap finite input costs relative to N; propagate an infinity sentinel only for forbidden/infeasible alternatives. | C | New | the model | Avoid overflow and floating-point proof tolerances without treating a merely large cost as forbidden. |
| P04 — Precedence DAG and Kahn topological analysis | `src/instance.cpp`: `Graph::build` | Convert negative T[i][j] to j→i; topologically sort using indegrees and a minimum-label ready queue; detect cycles, sources, sinks, and whether only one canonical order exists. | A | Version | A. B. Kahn, *Topological sorting of large networks*, 1962 [R01] | Detect infeasibility, derive endpoints, and obtain a baseline feasible order; a unique order permits an immediate exact conclusion. |
| P05 — Exact twin-label symmetry reduction | `src/instance.cpp`: `Graph::build`, local `twins` predicate | Row/column multiset fingerprints nominate pairs; exact normalized row/column comparisons verify interchangeable nodes and equal mutual costs. Add increasing-label precedence within each twin class. Hashes never establish equivalence. | C | Version | the model | Eliminate interchangeable permutations, including all-equal instances, without changing the optimum. |
| P06 — Bit-parallel DAG transitive closure | `src/instance.cpp`: `Graph::build`; `Graph::before`, `available` | In topological order, OR predecessor bitsets from direct predecessors; transpose reachability to successor bitsets. | A | Version | Kahn, *Topological sorting of large networks*, 1962 [R01] (topological-order foundation; bitset propagation adaptation here) | Compact N≤1000 precedence tests, implied-constraint discovery, and fast feasibility checks. |
| P07 — Impossible-adjacency elimination | `src/instance.cpp`: `Graph::build` | Forbid self arcs, arcs reversing transitive precedence, and i→j whenever some k must occur strictly between i and j. Detect the latter by intersecting succ(i) and pred(j). | C | Version | the model | Strengthen every assignment relaxation without excluding a feasible Hamiltonian path. |
| P08 — Dummy-node path-to-cycle reduction | `src/instance.cpp`: `Graph::build` | Add dummy d with zero arcs d→every remaining source and every remaining sink→d; forbid other dummy arcs. | C | Version | the model | Apply the established dummy-vertex reduction with variable endpoints and the canonical DAG. No original index is privileged. |
| P09 — Dense immutable search layout | `include/sop/instance.hpp`: `Instance`, `Graph`; `src/instance.cpp`: `Graph::build` | Keep original costs for validation; construct a row-major reduced cost matrix, compact closure words, and direct adjacency lists. | C | New | the model | O(N²) storage and predictable scans; separate original semantics from search reductions. Worker replication belongs to C23. |

The root assignment and root connectivity bound are first evaluated before
branching, but their main work is during search: they appear only in Table 2.
Fingerprint generation belongs to C19; P05 uses its hash primitive only to nominate
twins. P05 is executed before Kahn's analysis; the order of inventory rows is
conceptual, not a literal execution trace.
The first feasible topological order is passed to C09 immediately, before closure
and arc reduction, so later preprocessing cancellation preserves that incumbent.

## TABLE 2 — COMPUTATION

| Technique name | Where it appears in code (file and function/class) | Description of the algorithm | Classification 1 | Classification 2 | Source (author, title, year) | Reasoning for this implementation |
| --- | --- | --- | --- | --- | --- | --- |
| **2a. Search strategy** | — | — | — | — | — | — |
| C01 — Precedence-feasible prefix branch-and-bound | `src/solver.cpp`: `Worker::visit`, `branch`, `push`, `pop` | Extend a prefix with each unvisited zero-remaining-indegree node. DFS retains low memory; prefix cost plus admissible residual bounds rejects non-improving subtrees. | A | Used | Shobaki and Jamal, *An exact algorithm for the sequential ordering problem…*, 2015 [R02]; Gonggiatgul et al., *A parallel branch-and-bound algorithm with history-based domination…*, 2023 [R03] | Exact enumeration naturally exposes the `(visited,last)` state and satisfies precedence by construction. |
| C02 — Reduced-cost branch ordering | `src/solver.cpp`: `Worker::visit` candidate sort | Order by assignment fixing bound, then edge cost, successor count, and node label. Keep the first branch local when donating work. | C | Version | the model | Search promising choices early without omitting any feasible non-pruned choice. |
| C03 — Precedence-constrained Held–Karp suffix DP | `src/tail_dp.cpp`: `solve_tail`; `tests/integration.py`: independent `oracle` | Memoize exact remaining cost by `(tail subset,last)` with prerequisite masks; store choices for reconstruction. Default cutoff 12, maximum 20. The test oracle implements its own raw-matrix recurrence. | A | Used | M. Held and R. M. Karp, *A Dynamic Programming Approach to Sequencing Problems*, 1962 [R04] | Avoid repeated tree work near leaves without an infeasible full 2^N table. |
| **2b. Upper bounding / heuristics** | — | — | — | — | — | — |
| C04 — Ready-set greedy and randomized restricted candidates | `src/heuristics.cpp`: `greedy`; `src/solver.cpp`: `solve`, search probes | Build feasible topological paths using ordinary or assignment-reduced edge cost, an unlock-count tie term, and occasional selection among the best few ready nodes. | C | Version | the model | Obtain an incumbent early and diversify endpoint/order choices cheaply. |
| C05 — Precedence-safe Or-opt, lengths 1–3 | `src/heuristics.cpp`: `improve`, N>128 branch | Relocate a short, unreversed block. Direct predecessor/successor positions delimit feasible insertion boundaries; evaluate the three-edge cost change. | A | Version | İ. Or, *Traveling salesman-type combinatorial problems and their relation to the logistics of regional blood banking*, 1976 [R05] | Quadratic neighborhood scans remain useful at larger N; all costs may be asymmetric. |
| C06 — Order-preserving adjacent-block exchange | `src/heuristics.cpp`: `improve`, N≤128 branch | For cuts a<middle<end, exchange adjacent blocks while retaining their internal orders. Stop extension when a direct precedence crosses the blocks; accept a negative three-edge delta. | A | Version | L. M. Gambardella and M. Dorigo, *An Ant Colony System Hybridized with a New Local Search for the Sequential Ordering Problem*, 2000 [R06] | A SOP-3-exchange-style neighborhood is broader than short relocation on small instances. This is not the full published HAS-SOP algorithm. |
| C07 — Iterated local search perturbation | `src/heuristics.cpp`: `heuristic_portfolio` | On selected trials copy the current best path, swap adjacent incomparable nodes randomly, and reapply local improvement; other trials restart greedily. | A | Version | H. R. Lourenço, O. C. Martin, T. Stützle, *Iterated Local Search*, 2003 [R07] | Escape local optima without creating precedence violations. |
| C08 — Budgeted parallel heuristic portfolio and search probes | `src/heuristics.cpp`: `heuristic_portfolio`; `src/solver.cpp`: `solve`, `Worker::visit` | Share a warmup deadline, use worker-specific seeds, cap individual local searches, and occasionally probe from a search prefix with its current duals. | C | New | the model | Bound initial overhead and continue improving the incumbent during long proofs. Reuses C04–C07. |
| C09 — Independent validation and incumbent publication | `src/instance.cpp`: `Instance::validate`; `src/heuristics.cpp`: `Incumbent::offer`, `path` | Check the permutation and every original negative entry; recompute original edge cost. Publish strict improvements under a path mutex and release/acquire atomic cost; log each accepted cost. | C | New | the model | A heuristic, reduction, or reconstruction bug cannot silently inject an invalid upper bound. Called again after search; not re-listed in Table 3. |
| **2c. Lower bounding** | — | — | — | — | — | — |
| C10 — Integer Hungarian assignment relaxation | `src/assignment.cpp`: `Assignment::initialize`, `augment`, `reduced` | Row/column dual reductions, a greedy zero matching, then primal-dual shortest augmenting paths produce an optimal residual perfect matching and equal dual bound. | A | Used | H. W. Kuhn, *The Hungarian method for the assignment problem*, 1955 [R08] | A stronger relaxation than independent row minima, exact in integer arithmetic and warm-startable. |
| C11 — Incremental row/column deletion with reversible updates | `src/assignment.cpp`: `force`, `set`, `mark`, `rollback`, `load` | Fix last→next; remove its row/column. If not already matched, repair the single unmatched row/column pair. Log each changed field at most once per child transaction and undo on DFS return. | A | Version | Kuhn, *The Hungarian method for the assignment problem*, 1955 [R08] (assignment method; dynamic update/undo adaptation here) | At most one augmentation for a deletion; avoid per-node dense-matrix reconstruction and unbounded repeated undo records. |
| C12 — Dynamic frontier eligibility restriction | `src/assignment.cpp`: `restrict_frontier`; `src/solver.cpp`: `Worker::branch` | Restrict the new last row to precedence-ready unvisited nodes; allow dummy closure only when none remain. Repair its matching with at most one further augmentation. | C | Version | the model | Remove assignment choices that cannot be the immediate next path edge. |
| C13 — Dual reduced-cost fixing bound | `src/solver.cpp`: candidate construction and `Worker::branch`; `src/assignment.cpp`: `reduced` | `prefix_cost + assignment_bound + reduced(last,next)` is a lower bound on any completion using that next edge. | A | Version | Kuhn, *The Hungarian method for the assignment problem*, 1955 [R08] (dual foundation) | Reject many children before reoptimizing their assignments. |
| C14 — Assignment-component contraction and cross costs | `src/assignment.cpp`: `component_extra` | Union-find with rank/path compression contracts matched cycles and the residual last-to-dummy path. Compute the least directed reduced cost between components. | C | Version | the model | Expose the connectivity absent from the assignment relaxation; the union-find is only a component-construction subroutine. |
| C15 — Reduced-cost component MST | `src/assignment.cpp`: `component_extra`, Prim loop | Symmetrize component cross costs by taking the cheaper direction; use dense Prim to lower-bound the reduced cost needed to connect all components. | A | Version | R. C. Prim, *Shortest Connection Networks and Some Generalizations*, 1957 [R09] | Every feasible completion induces a connected contracted graph; its crossing edges contain a spanning tree. |
| C16 — Component incoming/outgoing minima | `src/assignment.cpp`: `component_extra`, `out_sum`, `in_sum` | When there are multiple assignment components, sum the cheapest outgoing crossing edge per component and, separately, the cheapest incoming crossing edge per component. | C | Version | the model | A connected completion must enter and leave every contracted component, including the component containing both residual endpoints. |
| C17 — Safe bound combination and selective evaluation | `src/assignment.cpp`: `component_extra`; `src/solver.cpp`: `Worker::visit`, `solve` | Add `max(MST,sum_in,sum_out)` to the assignment bound. Evaluate at root, every eighth prefix level, and all residual sizes ≤80 (unless disabled). | C | New | the model | The bounds overlap and must not be added together; selective evaluation controls O(N²) overhead. |
| **2d. Pruning, dominance, and symmetry/redundancy elimination** | — | — | — | — | — | — |
| C18 — Bounded exact-key history dominance | `src/history.cpp`: `HistoryTable::dominated`, `SharedHistory::dominated`; `src/solver.cpp`: `Worker::visit` | Store least prefix cost for the exact visited bitset and last vertex. Four-way buckets use age-based replacement; shared mode locks a shard for compare/update; local mode uses private tables. | A | Used | Shobaki and Jamal, *An exact algorithm…*, 2015 [R02]; Gonggiatgul et al., *A parallel branch-and-bound algorithm with history-based domination…*, 2023 [R03] | Reuse equivalent-prefix information while bounding memory. Full key comparison prevents hash-collision pruning; eviction cannot invalidate exactness. |
| C19 — Incremental mixed fingerprints | `src/common.cpp`: `mix64`; `src/instance.cpp`: `Graph::build`; `src/solver.cpp`: `push`, `pop`, key computation | XOR deterministic per-node fingerprints for the visited set, mix in the last vertex, and select history buckets/shards with a SplitMix64-style finalizer. Also seed worker PRNGs and nominate twin candidates. | A | Version | A. L. Zobrist, *A New Hashing Method With Application for Game Playing*, 1970 [R10]; G. L. Steele Jr., D. Lea, C. H. Flood, *Fast Splittable Pseudorandom Number Generators*, 2014 [R11] | O(1) fingerprint updates; correctness still depends on exact comparisons, never hash uniqueness. |
| C20 — Short-window interchange dominance | `src/dominance.cpp`: `window_dominated`; `src/solver.cpp`: `Worker::visit` | Enumerate at most six orders of the last 2–3 internal prefix vertices, keeping the boundary and final vertex fixed. A feasible cheaper or lexicographically smaller equal-cost order dominates the current prefix. | C | Version | the model | Detect useful dominance before the alternate prefix has been explored; run before C18 to avoid circular equal-cost suppression. |
| **2e. Parallelization and NUMA handling** | — | — | — | — | — | — |
| C21 — Demand-driven work stealing | `src/parallel.cpp`: `Scheduler::take`, `pop`, `publish`, `wants_work`; `src/solver.cpp`: `Worker::branch` | Workers pop their own deque's back and steal other fronts, preferring the same NUMA node. Donate complete child snapshots when workers need work; cap queue growth through demand checks. | A | Version | R. D. Blumofe and C. E. Leiserson, *Scheduling multithreaded computations by work stealing*, 1999 [R12]; Gonggiatgul et al., *A parallel branch-and-bound algorithm…*, 2023 [R03] | Dynamically balance irregular trees without a global priority-queue bottleneck. Uses mutex deques, not a claim of lock-free Cilk behavior. |
| C22 — Reproducible worker-local pseudorandom streams | `src/heuristics.cpp`: `greedy`, `heuristic_portfolio`; `src/solver.cpp`: `Worker::rng_` | Use C++17 `std::mt19937_64`, seeded through C19, for restricted candidates and safe perturbations. | A | Version | M. Matsumoto and T. Nishimura, *Mersenne Twister: A 623-Dimensionally Equidistributed Uniform Pseudo-Random Number Generator*, 1998 [R13] (MT family; standard 64-bit variant here) | Avoid a shared RNG lock. Fixed seeds do not make time-limited parallel schedules deterministic. |
| C23 — Topology-aware placement and first touch | `src/parallel.cpp`: `placements`, `pin`; `src/solver.cpp`: worker construction; `src/history.cpp`: `SharedHistory::initialize` | Honor the allowed CPU mask, distribute physical cores across exposed NUMA nodes before SMT siblings, bind workers, then allocate/copy their graph and mutable state. First-touch history shards on owners; align counters/queues to reduce false sharing. | A | Version | Linux kernel contributors, *NUMA Memory Policy* and *What is NUMA?*, online documentation [R14]; Gonggiatgul et al., *A parallel branch-and-bound algorithm…*, 2023 [R03] (parallel SOP context) | Reduce migration/remote access while allowing Linux's fallback on memoryless dies. No assumed node numbering or forced system policy. |
| **2f. Optimality proof and termination logic** | — | — | — | — | — | — |
| C24 — Feasible assignment completion certificate | `src/assignment.cpp`: `completion`; `src/solver.cpp`: `solve`, `Worker::visit` | Follow the matching from the current last/dummy; accept only if it covers every remaining node once in precedence order and closes to dummy. Its cost equals the residual bound. | C | Version | the model | A feasible relaxation optimizer is an exact optimum for that subtree; also supplies an upper bound through C09. |
| C25 — Outstanding-work proof accounting | `src/parallel.cpp`: `Scheduler::finish`, `publish`, `arrive`; `src/solver.cpp`: `solve`, `Worker::visit` | Count queued plus executing tasks. Exhaustion is certified only after the last active unit finishes; alternatively prove when a feasible incumbent equals the certified root bound, or P04 found a unique canonical order. | C | Version | the model | Prevent premature proof while another worker owns a live subtree. Every unpruned possibility is covered, delegated, or solved by C03/C24. |
| C26 — Monotonic deadline, cooperative cancellation, and failure propagation | `src/common.cpp`: `Control`; `src/main.cpp`: signal handler; `src/solver.cpp`: worker exception handling | Poll a steady-clock deadline/signal flag; atomically record timeout/interruption/proof. Poll within long kernels, join workers, and propagate exceptions as failures. Never promote an interrupted search to a proof. | C | New | the model | Truthful anytime status with no detached workers or hidden failed subtrees. |

P05 is the symmetry-reduction technique; it is not repeated under 2d. C09 owns
incumbent synchronization as well as validation, so it is not duplicated under
2e. C10–C17 also provide the root bound before the first branch. Full subsets in
the independent test oracle are an alternate invocation of C03's recurrence,
not another production solver mode.

## TABLE 3 — POSTPROCESSING

| Technique name | Where it appears in code (file and function/class) | Description of the algorithm | Classification 1 | Classification 2 | Source (author, title, year) | Reasoning for this implementation |
| --- | --- | --- | --- | --- | --- | --- |
| Q01 — Result snapshot and counter aggregation | `src/solver.cpp`: `solve` finish lambda and worker join/aggregation | After joining workers, sum their enumeration counts, retrieve the locked incumbent, invoke C09 again against the original matrix, derive optimal status from C25/C26, and capture total elapsed time. | C | New | the model | A consistent final row with a fresh cost check; validation itself is inventoried only as C09. |
| Q02 — CSV serialization and escaping | `src/cli.cpp`: `csv`, `write_result` | Emit the specified seven columns, quote commas/newlines/quotes, double embedded quotes, use classic-locale numeric formatting and space-separated original indices. | A | Version | Y. Shafranovich, *Common Format and MIME Type for Comma-Separated Values (CSV) Files*, 2005 [R15] | Machine-readable output; uses RFC-style quoting with Unix LF line endings. |
| Q03 — Header-aware synchronized append | `src/cli.cpp`: `append_file`, `write_result` | Always emit stdout; additionally open the file in append mode, obtain a POSIX advisory lock, inspect emptiness, and write the optional header plus row with EINTR/short-write handling. | C | New | the model | Preserve the requested append/header semantics across cooperating processes and surface output errors. |
| Q04 — Independent verification and development harnesses (ancillary) | `tests/core_tests.cpp`; `tests/integration.py`; `tests/large_smoke.py`; `scripts/verify_csv.py`; `scripts/benchmark.py` | Exhaustively enumerate small residual matchings and raw-matrix SOP paths, check primal/dual and rollback invariants, compare two-chain instances by an independent count-pair DP, test black-box behavior, independently revalidate CSV paths, and run sequential recorded benchmarks with hashes. The full-subset oracle is C03. | C | New | the model | Make exactness claims testable and separate measured results from proposed performance. The two-chain recurrence is test-only, not an additional production solver. These tools do not certify arbitrary-run optimality. |

## Correctness arguments specific to this implementation

### Feasible paths, endpoint reduction, and twins

A negative entry imposes j before i, so a feasible path is exactly a linear
extension of the input DAG. A cyclic DAG has none. In an acyclic DAG a topological
order exists, and every forward edge in that order has a nonnegative input cost;
otherwise its negative marker would demand the reverse order.

Every feasible first node is a source and every feasible last node is a sink.
Adding the dummy edges gives a one-to-one correspondence between feasible paths
and dummy-rooted feasible cycles, preserving cost. Arc i→j is impossible if j
precedes i or if a mandatory intermediate node lies between them.

For a verified twin class, all members have identical relations/costs to outside
nodes and equal mutual directed costs. Relabeling those members in any path
preserves every original constraint and objective value. Sorting their appearances
by label therefore retains an optimum. Only exact comparisons authorize this
reduction. The remaining proofs apply to the augmented, canonical DAG.

### Assignment and incremental repair

For a prefix S ending at l, residual outgoing rows are `{l} ∪ (V \ S)` and
incoming columns are `(V \ S) ∪ {d}`. At the empty prefix, l=d. A feasible suffix
selects one edge for every such row and column, so the minimum assignment cost is
a lower bound. Dynamic filtering only removes impossible immediate successors of l.

Dual potentials satisfy `u[i]+v[j] <= c[i,j]` on permitted edges. Their sum L is
a lower bound, and the maintained perfect matching uses only zero reduced-cost
edges, certifying equality. On fixing l→v, removing its row/column preserves dual
feasibility. If they were matched together, the residual matching is already
optimal. Otherwise exactly one row and one column are unmatched; a Hungarian
augmentation restores optimality. Frontier filtering can invalidate only the new
last row's match, hence requires at most one additional augmentation. Undo restores
all altered potentials, matches, frontier permissions, and bound.

Forcing any edge i→j costs at least `L + (c[i,j]-u[i]-v[j])`: every completion's
reduced costs are nonnegative and include that edge's reduced cost. Prefix cost
is added once; dummy closure contributes zero.

The input cap also covers dual drift. With m active vertices and maximum finite
edge C, a deletion repair raises the residual dual by at most mC, and the
frontier repair by at most (m−1)C. This follows from residual optimum ≤(m−1)C,
nonnegative parent optimum, and `u[last]+v[next] <= C`. Summing over decreasing m
gives O(N²C) accumulated potential changes on a DFS path. The parser reserves
`16(N+1)^2` headroom below INF; rollback prevents accumulation across sibling
branches. Nonnegative component bounds are also bounded by a feasible remaining
path's cost. These facts justify the conservative numeric acceptance limit.

### Component bounds are additive to the dual, not to one another

The residual matching consists of cycles and (after a nonempty prefix) one path
from l to d. Contract these components. In any feasible suffix the crossing edges
connect all components. Their nonnegative reduced costs therefore pay at least
the minimum spanning tree of the cheaper-direction component graph.

Each component must also be entered and left. This includes the endpoint
component: it contains **both** l and d, so connectivity to another component
requires an exit and a return. The sum of cheapest outgoing crossing costs and,
separately, the sum of cheapest incoming crossing costs are each valid. Distinct
components in each sum charge distinct directed edges. These bounds may charge
the same edges as one another or the MST, so the code adds only their maximum
to L. If a required connection is absent, the corresponding bound is infinity.

The mandatory fixed prefix is not part of these residual costs. Contracting its
two residual endpoints into the same matching component is what makes the
incoming/outgoing argument valid for paths as well as cycles.

### Dominance, including concurrent active entries

For equal `(S,l)`, every allowed suffix is identical. A prefix with cost g₂≥g₁
cannot improve on the same suffix after the cheaper prefix. A lock makes shared
compare/update atomic, full bitsets resolve collisions, and table eviction merely
forgets an opportunity to prune. Equal-cost active prefixes are safe: the first
winner retains responsibility for that state. A strictly cheaper later label
retains its own responsibility; a finite descending chain ends in a surviving
representative. Timeout or worker failure prevents a global proof.

The window rule keeps S and l unchanged and substitutes a feasible internal
order with lower prefix cost, or equal cost and lexicographically smaller prefix.
This is a strict well-founded order on finitely many prefixes. It runs **before**
history insertion: an interchange-discarded equal-cost prefix cannot leave a
history record that discards its replacement. Ordinary lower-bound pruning of a
retained representative is safe because it rules out every improving suffix.

### Exact leaves and global termination

Suffix DP enumerates all precedence-ready next choices and takes an exact minimum,
with a zero dummy-closing edge at the base case. Interrupted DP results are not
used as exact results. A feasible assignment completion has cost equal to its
lower bound and thus solves that subtree directly. The unique-order shortcut
also has exactly one canonical candidate to evaluate.

All other states enumerate every non-pruned ready next node. Donation moves a
complete independent child state into a queue and increments outstanding work
before publication. Its parent remains counted until completion. Consequently
the transition from one outstanding task to zero means no queued or executing
search remains. The validated incumbent is then optimal. Equality with the
certified root lower bound is another sufficient global certificate. A deadline
or signal recorded first suppresses both conclusions. No heuristic score,
probabilistic equality, floating-point gap, or known benchmark answer is a proof.

## References

- **R01.** Arthur B. Kahn. *Topological sorting of large networks*. Communications
  of the ACM 5(11), 558–562, 1962. [DOI](https://doi.org/10.1145/368996.369025).
- **R02.** Ghassan Shobaki and Jafar Jamal. *An exact algorithm for the sequential
  ordering problem and its application to switching energy minimization in
  compilers*. Computational Optimization and Applications 61, 343–372, 2015.
  [DOI](https://doi.org/10.1007/s10589-015-9725-9).
- **R03.** Taspon Gonggiatgul, Ghassan Shobaki, and Pınar Muyan-Özçelik.
  *A parallel branch-and-bound algorithm with history-based domination and its
  application to the sequential ordering problem*. Journal of Parallel and
  Distributed Computing 172, 131–143, 2023.
  [DOI](https://doi.org/10.1016/j.jpdc.2022.10.007).
- **R04.** Michael Held and Richard M. Karp. *A Dynamic Programming Approach to
  Sequencing Problems*. Journal of the Society for Industrial and Applied
  Mathematics 10(1), 196–210, 1962. [DOI](https://doi.org/10.1137/0110015).
- **R05.** İlhan Or. *Traveling salesman-type combinatorial problems and their
  relation to the logistics of regional blood banking*. PhD thesis, Northwestern
  University, 1976. [Bibliographic record](https://cir.nii.ac.jp/crid/1572824500324093312).
- **R06.** Luca Maria Gambardella and Marco Dorigo. *An Ant Colony System Hybridized
  with a New Local Search for the Sequential Ordering Problem*. INFORMS Journal
  on Computing 12(3), 237–255, 2000. [DOI](https://doi.org/10.1287/ijoc.12.3.237.12636).
- **R07.** Helena Ramalhinho Lourenço, Olivier C. Martin, and Thomas Stützle.
  *Iterated Local Search*. In *Handbook of Metaheuristics*, 320–353, 2003.
  [DOI](https://doi.org/10.1007/0-306-48056-5_11);
  [author preprint](https://arxiv.org/abs/math/0102188).
- **R08.** Harold W. Kuhn. *The Hungarian method for the assignment problem*.
  Naval Research Logistics Quarterly 2, 83–97, 1955.
  [DOI](https://doi.org/10.1002/nav.3800020109).
- **R09.** Robert C. Prim. *Shortest Connection Networks and Some Generalizations*.
  Bell System Technical Journal 36(6), 1389–1401, 1957.
  [DOI](https://doi.org/10.1002/j.1538-7305.1957.tb01515.x).
- **R10.** Albert L. Zobrist. *A New Hashing Method With Application for Game
  Playing*. University of Wisconsin Computer Sciences Technical Report 88, 1970.
  [Report](https://research.cs.wisc.edu/techreports/1970/TR88.pdf).
- **R11.** Guy L. Steele Jr., Doug Lea, and Christine H. Flood. *Fast Splittable
  Pseudorandom Number Generators*. OOPSLA, 2014.
  [Author manuscript](https://gee.cs.oswego.edu/dl/papers/oopsla14.pdf).
- **R12.** Robert D. Blumofe and Charles E. Leiserson. *Scheduling multithreaded
  computations by work stealing*. Journal of the ACM 46(5), 720–748, 1999.
  [DOI](https://doi.org/10.1145/324133.324234).
- **R13.** Makoto Matsumoto and Takuji Nishimura. *Mersenne Twister: A
  623-Dimensionally Equidistributed Uniform Pseudo-Random Number Generator*.
  ACM Transactions on Modeling and Computer Simulation 8(1), 3–30, 1998.
  [DOI](https://doi.org/10.1145/272991.272995).
- **R14.** Linux kernel contributors. *NUMA Memory Policy* (rolling documentation,
  consulted 2026) and *What is NUMA?* (Linux 5.0 documentation, 2019).
  [Memory policy](https://www.kernel.org/doc/html/latest/admin-guide/mm/numa_memory_policy.html);
  [memoryless nodes](https://www.kernel.org/doc/html/v5.0/vm/numa.html).
- **R15.** Yakov Shafranovich. *Common Format and MIME Type for Comma-Separated
  Values (CSV) Files*. RFC 4180, 2005. [RFC](https://www.rfc-editor.org/rfc/rfc4180).

References establish algorithmic context, not a claim that every custom
combination reproduces a cited solver. No source code from those solvers is
included. In particular, there is no implementation here of the 2023 paper's
complete thread-restart/history-memory-management scheme or of HAS-SOP's ant
colony system.
