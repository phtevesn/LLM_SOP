# Exact Parallel NUMA-Aware Solver for the Sequential Ordering Problem (SOP)

A production-grade, parallel C++17 exact solver for the **Sequential Ordering Problem (SOP)** (also known as the Asymmetric Traveling Salesman Problem with Precedence Constraints, [Escudero 1988](https://ideas.repec.org/a/eee/ejores/v37y1988i2p236-249.html)). Given a TSPLIB-formatted `.sop` instance with $N \le 1000$ vertices, the solver finds a minimum-cost Hamiltonian path satisfying all precedence constraints and mathematically proves optimality.

---

## Section 1: Requirements and Installations Required

### Hardware & Target Architecture
- **Target Platform**: Ubuntu / Debian Linux on x86_64 multi-core processors, specifically engineered for the **AMD Ryzen Threadripper 2990WX** (32 physical cores / 64 threads across 4 Zeppelin dies configured as 4 NUMA domains, with 2 direct-attached DDR4 memory dies and 2 compute-only dies connected via Infinity Fabric, [PMC Cache-Aware Matrix Polynomials](https://pmc.ncbi.nlm.nih.gov/articles/PMC7302405/)) with 122 GiB RAM.
- **Portability**: Runs on any Linux x86_64 system (from 1 to 64+ cores) with automatic `/sys/devices/system/node` topology introspection and graceful fallback on single-node machines.

### Software Dependencies
- **Operating System**: Ubuntu 20.04 / 22.04 / 24.04 LTS or Debian 11 / 12 (Linux kernel $\ge 4.15$ for `SYS_mbind` and `pthread_setaffinity_np`).
- **Compiler**: GCC (`g++` $\ge 9.0$, tested with `g++ 12.2.0`) supporting the **C++17 standard** (`-std=c++17`) and POSIX threads (`-pthread`).
- **Build System**: GNU `make` (optional; a direct single-line `g++` compilation command is also provided).
- **External Libraries**: None required beyond the standard C++17 library (`libstdc++`), POSIX threads (`libpthread`), and standard Linux kernel headers (`<sched.h>`, `<sys/mman.h>`, `<sys/syscall.h>`).

### Installation Command (Ubuntu / Debian)
```bash
sudo apt-get update && sudo apt-get install -y build-essential g++ make
```

---

## Section 2: Exact Commands to Build and Run with an Example

### Building the Solver
Using `make`:
```bash
make clean && make -j
```

Or compiling directly with `g++`:
```bash
g++ -std=c++17 -O3 -march=native -pthread -Wall -Wextra -pedantic \
    main.cpp parser.cpp preprocessor.cpp numa_topology.cpp \
    heuristics.cpp bounds.cpp history_table.cpp solver.cpp validator.cpp \
    -o prog -pthread
```

### Command-Line Interface
```bash
./prog [filepath] [--time-limit SECONDS] [--threads N] [--output out.csv] [--no-header]
```
- `filepath`: Path to the input `.sop` instance file (required).
- `--time-limit SECONDS`: Optional wall-clock time limit in seconds (supports floating-point or integer values; default: no time limit).
- `--threads N`: Optional number of worker threads to use (default: `1`). Set `--threads 32` to utilize all 32 physical cores on the AMD Ryzen Threadripper 2990WX.
- `--output out.csv`: Optional CSV file path. When provided, the output row is written to both `stdout` and `out.csv` (appending if `out.csv` already exists, and writing the header row to `out.csv` only if the file is new or empty, unless `--no-header` is passed).
- `--no-header`: Optional flag that suppresses the CSV header line on both `stdout` and `--output`.

### Example Execution
Running on `prob.42.sop` (`N = 42`), `rbg048a.sop` (`N = 50`), and `R.200.1000.1.sop` (`N = 200`):
```bash
./prog prob.42.sop --threads 2 --output results.csv
./prog rbg048a.sop --threads 2 --output results.csv
./prog R.200.1000.1.sop --threads 2 --output results.csv
```

**Example `stderr` Progress Log:**
```text
[Info] Loaded instance 'prob.42.sop' (N=42, threads=2)
[Best Cost Update] New best known cost: 359 (Phase: Heuristic-ILS, Time: 0.0175s)
[Best Cost Update] New best known cost: 251 (Phase: Heuristic-ILS, Time: 0.0345s)
[Best Cost Update] New best known cost: 248 (Phase: Prefix-B&B, Time: 0.0806s)
[Best Cost Update] New best known cost: 247 (Phase: Prefix-B&B, Time: 0.1992s)
[Best Cost Update] New best known cost: 243 (Phase: Prefix-B&B, Time: 0.3223s)
[Validation] Path verified valid (permutation=OK, precedences=OK, recomputed_cost=243, optimal=yes, time=0.788170s)
```

**Example `stdout` / `results.csv` Output:**
```csv
Instance,Instance Size,Time,Enumerated Nodes,Best Cost,Best Path,Optimal
prob.42.sop,42,0.788170,148618,243,0 11 4 13 7 27 34 6 22 19 33 16 25 24 39 1 36 32 18 38 5 2 8 23 3 35 15 9 14 10 20 12 30 31 26 17 28 21 40 29 37 41,yes
rbg048a.sop,50,1.211844,215971,351,0 9 1 3 24 11 20 13 4 2 31 17 22 5 23 7 47 28 8 19 30 27 26 16 6 15 33 34 29 14 12 10 18 21 35 32 25 37 36 38 45 44 46 48 41 42 39 40 43 49,yes
R.200.1000.1.sop,200,0.228918,38,1404,0 118 37 36 ... 180 27 199,yes
```

---

## Section 3: Input and Output Format Descriptions

### Input Format (`.sop`)
The solver accepts TSPLIB-style `.sop` files ([Reinelt 1995](https://www.or.uni-bonn.de/lectures/ws17/co_exercises/programming/tsp/tsp95.pdf)):
1. **Metadata Header**: Keyword-value lines (`NAME`, `TYPE`, `COMMENT`, `DIMENSION`, `EDGE_WEIGHT_TYPE`, `EDGE_WEIGHT_FORMAT`), where `DIMENSION` specifies the number of vertices $N \le 1000$.
2. **Matrix Section**: Begins with the keyword `EDGE_WEIGHT_SECTION`, followed by the repeated dimension integer $N$ (also tolerant of files that omit the repeated $N$), and then $N \times N$ whitespace-delimited integers representing the matrix $T$ (`FULL_MATRIX`) wrapped arbitrarily across lines, followed by an optional `EOF` line.
3. **Matrix Semantics**:
   - Vertices are 0-indexed ($0 \dots N-1$) by row/column order.
   - $T[i][j] \ge 0$: Direct transition cost from vertex $i$ to vertex $j$.
   - $T[i][j] < 0$: Precedence constraint indicating that vertex $i$ depends on vertex $j$, so **$j$ must precede $i$** ($j \prec i$) in any feasible Hamiltonian path.
   - $T[i][i] = 0$: Diagonal entries are ignored.

### Output Format (CSV)
Each run prints a single CSV data row (preceded by the header row unless `--no-header` is specified):
```text
Instance,Instance Size,Time,Enumerated Nodes,Best Cost,Best Path,Optimal
```
- **Instance**: Base filename of the input `.sop` file.
- **Instance Size**: Number of vertices $N$.
- **Time**: Total wall-clock runtime in seconds from start of execution through optimality proof (or timeout).
- **Enumerated Nodes**: Total number of Branch-and-Bound search tree nodes expanded across all worker threads (including any initial BFS frontier expansion nodes, Carpaneto-Toth LAP B&B nodes, and non-dominated Prefix/Suffix B&B states expanded past the Zobrist history table).
- **Best Cost**: Exact total cost of the best valid Hamiltonian path found, or `-1` if no feasible path was found before timeout.
- **Best Path**: Space-separated 0-based vertex indices of the path in visitation order, or empty if no feasible solution was found.
- **Optimal**: `yes` if and only if the solution was mathematically proven optimal before any time limit expired; `no` otherwise.

---

## Section 4: Name and Descriptions of the Reasoning for Algorithmic Choices

1. **Bit-Parallel Warshall Transitive Closure & Escudero Graph Reduction ([Warshall 1962](https://gtl.csa.iisc.ac.in/dsa/node165.html); [Escudero 1988](https://ideas.repec.org/a/eee/ejores/v37y1988i2p236-249.html))**:
   - *Reasoning*: In SOP instances, precedence constraints are transitive ($u \prec w \land w \prec v \implies u \prec v$). Moreover, if there exists any intermediate node $w$ with $u \prec w \prec v$, then $w$ must be visited strictly between $u$ and $v$, making the direct edge $(u, v)$ impossible in any valid tour. Computing the transitive closure via 64-bit word-packed bitsets (`BitSet1024`) in $O(N^3 / 64)$ time and eliminating all reverse-precedence, transitive-bypass, and position-window-incompatible arcs removes hundreds of infeasible edges before search begins (`337` arcs removed on `rbg048a.sop`), tightening all downstream lower bounds.

2. **Forward vs. Backward Poset Order-Ideal Orientation Probing ([Salii 2019](https://www.researchgate.net/publication/316521572_Revisiting_Dynamic_Programming_for_Precedence-Constrained_Traveling_Salesman_Problem_and_Its_Time-Dependent_Generalization))**:
   - *Reasoning*: Any SOP instance $(V, T, \prec)$ is equivalent under path reversal to its transposed instance $(V, T^T, \succ)$. However, real-world precedence DAGs are often strongly asymmetric: on `rbg048a.sop`, expanding order ideals of $\prec$ forward from node `0` produces `3,536,608` ideals at depth 8, whereas expanding order filters backward from node `49` produces only `375` ideals at depth 8—nearly a $10,000\times$ reduction in branching width. A lightweight 6-step BFS width probe in preprocessing automatically detects this asymmetry and transposes the instance when backward expansion is narrower.

3. **Multi-Start Randomized Topological Greedy + 2-Opt + Gambardella-Dorigo `SOP-3-Exchange` + Iterated Local Search ([Gambardella & Dorigo 2000](https://www.semanticscholar.org/paper/An-Ant-Colony-System-Hybridized-with-a-New-Local-Gambardella-Dorigo/21d51d11da77c2d0ce831846763411bb4d85416d))**:
   - *Reasoning*: Branch-and-Bound pruning efficiency depends critically on starting with a near-optimal upper bound ($UB$). Standard TSP 2-opt reversals often violate precedences; pairing bit-parallel precedence-checked 2-opt with Gambardella & Dorigo's path-preserving `SOP-3-Exchange` (which swaps adjacent blocks without inverting internal node order) and Iterated Local Search (ILS) perturbation kicks finds the exact optimum (`351`) on `rbg048a.sop` in `32 ms` and within `3%` (`251` vs `243`) on `prob.42.sop` in `34 ms`.

4. **Hybrid Dual-Engine Exact Search: Carpaneto-Toth Incremental LAP B&B + Dual-Potential 2-Cycle-Free Prefix/Suffix B&B ([Little et al. 1963](https://ideas.repec.org/a/inm/oropre/v11y1963i6p972-989.html); [Jonker & Volgenant 1987](https://www.semanticscholar.org/paper/A-shortest-augmenting-path-algorithm-for-dense-and-Jonker-Volgenant/ccbe40d7e91e451d10069d1f1b0e7989af528644); [Held & Karp 1962](https://epubs.siam.org/doi/10.1137/0110015); [Shobaki & Jamal 2015](https://www.researchgate.net/publication/276833419_An_exact_algorithm_for_the_sequential_ordering_problem_and_its_application_to_switching_energy_minimization_in_compilers); [Jamal et al. 2017](https://scholar.google.com/citations?user=ogP9XKEAAAAJ&hl=en))**:
   - *Reasoning*: As highlighted in the SOP literature ([Tree Search for the SOP, arXiv:1911.12427](https://arxiv.org/pdf/1911.12427)), SOP benchmarks span two distinct structural regimes:
     - **Low-precedence-density / large-$N$ asymmetric instances** (such as `R.200.1000.1.sop`, $N=200$, $1\%$ density): The Linear Assignment Problem (LAP) relaxation has almost no subtours (`5` cycles at the root, `LB = 1392` vs `Opt = 1404`). Branching on the shortest subtour or shortest precedence-violating subpath with $O(N^2)$ incremental shortest-augmenting-path updates proves optimality on `R.200.1000.1.sop` in just **38 nodes (`0.22s`)**.
     - **Medium/high-precedence-density or symmetric instances** (such as `prob.42.sop` and `rbg048a.sop`): The LAP relaxation suffers from 2-cycles ($u \leftrightarrow v$) or dense precedence interactions. For these, we combine two complementary dual potential spaces—the **LAP dual potentials** $(U_{lap}, V_{lap})$ and **Held-Karp subgradient-optimized 2-cycle-free potentials** $(U_{sub}, V_{sub})$—inside a **2-Cycle-Free Dynamic 2-Edge Bound with Ready-Set Filtering and Parent Top-3 Edge Caching**. Pre-extracting the top 3 active edges per unvisited vertex once at the parent node allows every candidate child's exact 2-cycle-free bound to be evaluated in $O(k)$ register operations without rescanning adjacency lists.

5. **Lock-Free 4-Way Set-Associative Zobrist History Domination with Suffix Lower-Bound Memoization ([Zobrist 1970](https://research.cs.wisc.edu/techreports/1970/TR88.pdf); [Gonggiatgul, Shobaki, & Muyan-Özçelik 2022](https://www.researchgate.net/scientific-contributions/Pinar-Muyan-Oezcelik-2329934107))**:
   - *Reasoning*: Two different prefix permutations visiting the same vertex subset $S$ and ending at the same vertex $u$ face the exact same remaining subproblem. Our 64-byte cache-line-aligned 4-way set-associative Zobrist table stores *both* the minimum prefix cost `min_prefix` seen at $(S, u)$ *and* the proven lower bound `suffix_lb` on completing the tour from $(S, u)$. Any future arrival at $(S, u)$ is pruned in $O(1)$ if either `cur_cost >= min_prefix` or `cur_cost + suffix_lb >= best_cost`, using lock-free 64-bit atomic compare-and-swap operations with zero heap allocations during search.

6. **Threadripper 2990WX Asymmetric WX NUMA Scheduling & Hierarchical Work Stealing ([Blumofe & Leiserson 1999](https://bibbase.org/network/publication/blumofe-leiserson-schedulingmultithreadedcomputationsbyworkstealing-1999); [TechPowerUp 2970WX/2990WX Architecture](https://www.techpowerup.com/review/amd-ryzen-threadripper-2970wx/3.html))**:
   - *Reasoning*: On the AMD Ryzen Threadripper 2990WX, only 2 of the 4 Zeppelin dies have local DDR4 memory controllers, while the other 2 compute dies must traverse the Infinity Fabric for DRAM access. Our `NumaTopology` scheduler inspects `/sys/devices/system/node/node*/meminfo` to schedule threads onto memory-bearing dies first, binds the 64 MB Zobrist transposition table to local DRAM via `SYS_mbind`, pins worker threads to dedicated physical cores via `pthread_setaffinity_np`, and performs two-tier work stealing (stealing from sibling cores on the same NUMA die before crossing the Infinity Fabric).
