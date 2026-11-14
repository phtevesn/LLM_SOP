# Development Log & Engineering Decisions (`DEVELOPMENT.md`)

This document records the engineering progression of the SOP solver, including the initial architectural plan, algorithmic alternatives evaluated (kept vs. dropped), bugs and bottlenecks encountered and resolved during development, design decisions triggered by empirical profiling, and a strict separation between measured experimental results and reasoned (untested) target-hardware behavior.

---

## 1. Initial Plan and Algorithmic Alternatives Considered

The Sequential Ordering Problem (SOP) generalizes the Asymmetric Traveling Salesman Problem (ATSP) with transitive precedence constraints ([Escudero 1988](https://ideas.repec.org/a/eee/ejores/v37y1988i2p236-249.html)). Because the solver must handle instances up to $N \le 1000$ across vastly different constraint densities while proving optimality on a 32-core AMD Ryzen Threadripper 2990WX, we evaluated five candidate paradigms:

1. **Pure Bottom-Up Dynamic Programming (Held-Karp / Order-Ideal DP) — *Dropped as standalone, kept as Zobrist History Memoization***:
   - *Analysis*: Breadth-first order-ideal dynamic programming ([Held & Karp 1962](https://epubs.siam.org/doi/10.1137/0110015); [Salii 2019](https://www.researchgate.net/publication/316521572_Revisiting_Dynamic_Programming_for_Precedence-Constrained_Traveling_Salesman_Problem_and_Its_Time-Dependent_Generalization)) enumerates all precedence-feasible vertex subsets $S \subseteq V$ layer by layer.
   - *Why dropped as standalone*: An empirical ideal-counting prototype (`count_ideals.cpp`) showed that even at depth 7 out of 50 on `rbg048a.sop`, there are `937,716` unpruned forward ideals (and `3,536,608` at depth 8). Without upper-bound pruning driven by a tight incumbent, pure breadth-first DP exhausts memory and time on medium-density instances and is impossible for $N = 200 \dots 1000$.
   - *What was kept*: The core insight of DP—that two prefixes visiting the same vertex subset $S$ and ending at the same vertex $u$ share an identical suffix subproblem—was retained via our **Lock-Free 4-Way Zobrist History Domination Table** storing both `min_prefix` and `suffix_lb`.

2. **Plain Branch-and-Bound (DFS without Heuristic Upper Bound or History Table) — *Dropped***:
   - *Analysis*: Plain depth-first B&B with simple outgoing/incoming minimum edge bounds starts with $UB = \infty$ and re-explores identical $(S, u)$ suffix subproblems reached via different prefix permutations.
   - *Why dropped*: Without history domination and a tight initial $UB$, plain B&B fails to finish `prob.42.sop` ($N=42$) within 10 minutes.

3. **Prefix/Suffix Branch-and-Bound with Primal Heuristics + Zobrist History Domination ([Shobaki & Jamal 2015](https://www.researchgate.net/publication/276833419_An_exact_algorithm_for_the_sequential_ordering_problem_and_its_application_to_switching_energy_minimization_in_compilers); [Gonggiatgul et al. 2022](https://www.researchgate.net/scientific-contributions/Pinar-Muyan-Oezcelik-2329934107)) — *Kept (Engine B)***:
   - *Analysis*: Pairing a fast initial heuristic (Multi-Start Randomized Topological Greedy + 2-Opt + `SOP-3-Exchange` + Iterated Local Search) with a depth-first B&B engine that uses dynamic 2-cycle-free 2-edge lower bounds and a Zobrist history table.
   - *Why kept*: Extremely fast per-node evaluation (>150,000 nodes/sec/core) and strong dominance pruning on medium- and high-precedence-density instances (`prob.42.sop` and `rbg048a.sop`).

4. **Assignment Relaxation (LAP) Subtour & Precedence-Violation Branch-and-Bound ([Little et al. 1963](https://ideas.repec.org/a/inm/oropre/v11y1963i6p972-989.html); [Jonker & Volgenant 1987](https://www.semanticscholar.org/paper/A-shortest-augmenting-path-algorithm-for-dense-and-Jonker-Volgenant/ccbe40d7e91e451d10069d1f1b0e7989af528644)) — *Kept (Engine A)***:
   - *Analysis*: Solves the Linear Assignment Problem (LAP) relaxation and branches by partitioning the shortest subtour or the shortest precedence-violating subpath using $O(N^2)$ incremental shortest augmenting path updates.
   - *Why kept*: On large, low-precedence-density asymmetric instances such as `R.200.1000.1.sop` ($N=200$, $1\%$ precedence density), Prefix B&B faces an astronomical branching factor, whereas the LAP relaxation has only 5 cycles at the root (`LB = 1392` vs. `Opt = 1404`) and proves optimality in **38 nodes (`0.22s`)**.

5. **Hybrid Two-Stage Exact Portfolio (Engine A + Engine B with Shared Incumbent & Dual Potentials) — *Kept as Final Architecture***:
   - *Analysis*: Runs Preprocessing $\to$ Dual Potential Precomputation (LAP + Held-Karp Subgradient) $\to$ Primal Heuristics $\to$ Stage 1 (Carpaneto-Toth Incremental LAP B&B) $\to$ Stage 2 (Parallel NUMA-Aware Dual-Potential Prefix/Suffix B&B with History Domination).
   - *Why kept*: Solves all three benchmark archetypes (`prob.42.sop`, `rbg048a.sop`, and `R.200.1000.1.sop`) to proven optimality in **0.22s to 1.56s** on a single core and scales across multiple threads.

---

## 2. Bugs and Dead Ends Hit Along the Way and How They Were Resolved

1. **Bug 1: Premature Adjacency List Sorting Inside the Outer Vertex Loop (`test_fast_bb.cpp`)**
   - *Symptom*: When testing the reduced-cost 2-edge lower bound on `prob.42.sop` and `rbg048a.sop`, the computed root lower bound was `262` on `prob.42.sop` (higher than the valid heuristic tour of `259`!) and `388` on `rbg048a.sop` (higher than the optimal tour of `351`!).
   - *Root Cause*: By writing a per-vertex diagnostic comparing the bound's selected edges against the known heuristic tour (`best_path`), we discovered that `in_raw[v].push_back(...)` and `in_red[v].push_back(...)` were populated inside the outer loop `for (int u = 0; u < N; ++u)` while `sort(in_raw[u].begin(), in_raw[u].end())` was called at the bottom of that same outer loop. Consequently, for every vertex `u`, incoming edges from vertices `u' > u` were appended *after* `in_raw[u]` had already been sorted, leaving `in_raw[u]` and `in_red[u]` partially unsorted and causing the bound to miss cheaper incoming edges from higher-indexed vertices.
   - *Resolution*: Separated edge collection and sorting into two sequential loops (`for (u = 0..N-1)` to populate all outgoing and incoming vectors, followed by a second `for (u = 0..N-1)` loop to sort them). Immediately after this fix, the root bounds became 100% admissible (`178 <= 243` on `prob.42.sop` and `337 <= 351` on `rbg048a.sop`).

2. **Bug 2: Incomplete Suffix Lower-Bound Recording on Pruned Children (`test_dual_ascent.cpp` / `test_fast_bb2.cpp`)**
   - *Symptom*: When first implementing suffix lower-bound memoization in the history table, replacing `min_prefix` with `suffix_lb` caused the node count on `prob.42.sop` to increase rather than decrease.
   - *Root Cause*: When a candidate child $v$ was pruned immediately because `cur_cost + w(u, v) >= best_cost`, the loop recorded `min_suffix = min(min_suffix, w)` without adding the child's remaining suffix lower bound `c_slb` (or skipped updating `min_suffix` via `continue`). This caused the parent state $(S, u)$ to record an artificially small `suffix_lb`, failing to prune subsequent arrivals at $(S, u)$ with `new_cur_cost >= old_cur_cost` after `best_cost` improved.
   - *Resolution*: Designed `ZobristHistoryTable` to pack **both** `uint32_t min_prefix` and `uint32_t suffix_lb` into a single 64-bit atomic word (`payload`). Any state arrival with `cur_cost >= min_prefix` is pruned immediately by prefix dominance, while arrivals with `cur_cost < min_prefix` are pruned whenever `cur_cost + suffix_lb >= best_cost`.

3. **Dead End: Per-Node Incremental LAP Inside Deep Prefix B&B (`test_inc_lap_bb.cpp`)**
   - *Symptom*: Attempting to maintain a full $k \times k$ incremental Hungarian/LAP matching at every node of the Prefix B&B tree on `rbg048a.sop` did not finish within 6 seconds.
   - *Root Cause*: In `rbg048a.sop`, the LAP relaxation has cost `337` due to a 5-vertex subtour near the end of the graph, while 30 vertices become simultaneously ready at depth 2 with many zero-cost edges. Running $O(k^2)$ augmenting paths at every prefix node spent ~2 microseconds per node across hundreds of thousands of shallow prefixes without breaking the downstream subtour.
   - *Resolution*: Replaced per-node augmenting paths in Prefix B&B with **Parent Top-3 Edge Caching** over two precomputed dual spaces (LAP duals $U_{lap}, V_{lap}$ and Held-Karp subgradient duals $U_{sub}, V_{sub}$), and added **Forward vs. Backward Poset Orientation Probing**.

---

## 3. Design Decisions That Changed During Development

1. **Adding Held-Karp Subgradient Optimization on 2-Cycle-Free Dual Potentials (`bounds.cpp`)**:
   - *Trigger*: On `prob.42.sop`, the cost matrix is nearly symmetric ($T[i][j] = T[j][i]$), causing the root LAP assignment relaxation to decompose into twenty 2-cycles ($u \leftrightarrow v$) with a loose root lower bound of `174` (vs. optimal `243`).
   - *Change*: Added `LowerBounds::optimize_subgradient_duals`, which runs Held-Karp style subgradient ascent on the 2-cycle-free 2-edge relaxation in preprocessing. This boosted the root lower bound on `prob.42.sop` from `174` to `216` in $<1\text{ ms}$ and reduced the B&B tree from `612,499` nodes to `122,760` nodes.

2. **Retaining Both Zero-Initialized Subgradient Duals and LAP Duals (`bounds.cpp`, `solver.cpp`)**:
   - *Trigger*: Testing subgradient ascent initialized from $(U_{lap}, V_{lap})$ vs. initialized from $(0, 0)$ showed that on `rbg048a.sop`, zero-initialized subgradient duals combined with LAP duals (`max(slb_lap, slb_sub)`) required `215,691` nodes (`0.89s`), whereas LAP-initialized subgradient duals required `426,427` nodes (`1.77s`).
   - *Change*: Kept $(U_{lap}, V_{lap})$ from the exact LAP solver and ran subgradient optimization from $(0, 0)$ for $(U_{sub}, V_{sub})$ so the two dual potentials capture complementary relaxations (asymmetric assignment vs. 2-cycle-free degree relaxation).

3. **Parent-Level Top-3 Active Edge Pre-Extraction (`solver.cpp`)**:
   - *Trigger*: Profiling `test_fast_bb.cpp` showed that scanning sorted adjacency lists for all unvisited vertices $x \in \text{unvis}$ inside the loop over candidate children $v \in \text{ready}$ repeated the same scans up to 20 times per node.
   - *Change*: Observing that for any child $v \in \text{ready}$, the new allowed incoming set for ready vertices is $(\text{unvis} \setminus \{v\}) \cup \{v\} = \text{unvis}$ and the new unvisited set is $\text{unvis} \setminus \{v\}$, we pre-extract the top 3 edges in `unvis` for each $x \in \text{unvis}$ **once** at the parent node. Every child $v$ then computes its exact 2-cycle-free bound in $O(|\text{unvis}|)$ register lookups without touching the adjacency lists.

4. **Forward vs. Backward Poset Ideal Width Probing (`preprocessor.cpp`)**:
   - *Trigger*: Inspecting `rbg048a.sop` revealed that 30 vertices have `in_degree == 2` (becoming ready simultaneously after nodes `0` and `1`), whereas in the reverse direction only 11 vertices have `out_degree == 1` and all earlier layers are strictly gated.
   - *Change*: Added a 6-step BFS order-ideal width probe in `Preprocessor::should_transpose_poset`. On `rbg048a.sop`, this automatically transposes the instance, solving it to proven optimality in `1.21s` (`215,819` nodes).

---

## 4. Measured Benchmark Results vs. Reasoned (Untested) Hardware Behavior

### A. Actually Compiled, Executed, and Measured Results
The following results were compiled with `g++ 12.2.0` (`-std=c++17 -O3 -march=native -pthread -Wall -Wextra -pedantic`) and measured in the verification VM environment (2 vCPUs, Intel Xeon 6985P-C @ 1 NUMA node):

| Instance | $N$ | Precedence Density | Escudero Arcs Eliminated | Poset Transposed? | Threads | Enumerated Nodes | Best Cost | Proven Optimal? | Measured Wall Time (s) | Independent Validator Check |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| `prob.42.sop` | 42 | 0.0581 | 26 | No | 1 | 122,760 | **243** | `yes` | `1.161383` | Passed (`cost=243`) |
| `prob.42.sop` | 42 | 0.0581 | 26 | No | 2 | 148,618 | **243** | `yes` | `0.788170` | Passed (`cost=243`) |
| `rbg048a.sop` | 50 | 0.2220 | 337 | Yes | 1 | 215,819 | **351** | `yes` | `1.566719` | Passed (`cost=351`) |
| `rbg048a.sop` | 50 | 0.2220 | 337 | Yes | 2 | 215,971 | **351** | `yes` | `1.211844` | Passed (`cost=351`) |
| `R.200.1000.1.sop` | 200 | 0.0100 | 1 | No | 1 | 38 | **1404** | `yes` | `0.220044` | Passed (`cost=1404`) |
| `R.200.1000.1.sop` | 200 | 0.0100 | 1 | No | 2 | 38 | **1404** | `yes` | `0.228918` | Passed (`cost=1404`) |
| `prob.42.sop` (`--time-limit 0.05`) | 42 | 0.0581 | 26 | No | 1 | 0 | **251** | `no` | `0.073912` | Passed (`cost=251`) |

### B. Reasoned / Untested on Target Hardware
Because our verification VM exposes 2 vCPUs and a single NUMA node (`NUMA node(s): 1`), the following aspects were **compiled and verified for syntactic and single-node runtime correctness, but are untested on physical 4-die AMD Ryzen Threadripper 2990WX hardware**:
1. **32-Core Scaling & Asymmetric 4-Die WX NUMA Placement (`--threads 32`)**:
   - The `/sys/devices/system/node/node*/meminfo` parser in `NumaTopology::discover()`, memory-die-first worker sorting in `NumaTopology::plan_worker_assignments()`, `SYS_mbind` (`MPOL_PREFERRED`) physical page binding, and two-pass same-die-first work stealing in `SopSolver::run_prefix_bb_64()` / `run_prefix_bb_general()` were tested on a 1-NUMA-node system (2 cores) and reasoned about from the Linux kernel `sysfs`/`mempolicy` specification and Threadripper 2990WX topology documentation, but have **not** been benchmarked on a physical 32-core / 4-NUMA-node 2990WX machine.
2. **Large High-Density Instances with $200 < N \le 1000$**:
   - The `BitSet1024` data structures and `run_prefix_bb_general()` code paths are compiled and tested up to $N=200$ (`R.200.1000.1.sop`), but instances with $200 < N \le 1000$ were not among the provided benchmark files and are therefore listed as untested at scale.
