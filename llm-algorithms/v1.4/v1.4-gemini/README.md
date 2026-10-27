# Parallel Exact Sequential Ordering Problem (SOP) Solver

A production-quality, modular, NUMA-aware C++17 solver for the **Sequential Ordering Problem (SOP)** on TSPLIB-formatted `.sop` instances. The solver finds a minimum-cost Hamiltonian path satisfying all precedence constraints and proves exact optimality using a hybrid architecture combining **Portfolio Metaheuristics**, **Hungarian Assignment Problem (AP) Dual Potentials**, **Directed Held-Karp Minimum Spanning Tree (HK-MST) Lagrangian Relaxation**, **Parallel Bidirectional Meet-in-the-Middle Bounded Dynamic Programming**, and **Parallel Work-Stealing Branch-and-Bound with Prefix Equivalence Dominance Pruning**.

## Project Architecture & Module Layout

The project is organized into clean, single-responsibility C++17 headers (`.hpp`) and implementation units (`.cpp`):

| File | Responsibility |
| :--- | :--- |
| `bitset.hpp` | 64-byte cache-line-aligned 1024-bit fixed-capacity bitset (`Bitset1024`), Zobrist-style hash functor (`BitsetHasher`), and equality comparator (`BitsetEqual`) for $O(N/64)$ precedence closure and state operations up to $N \le 1024$. |
| `numa_topology.hpp` / `numa_topology.cpp` | Linux `/sys/devices/system/node` NUMA topology discovery and `pthread_setaffinity_np` hardware thread pinning tailored for multi-die architectures such as the **AMD Ryzen Threadripper 2990WX** (4 NUMA dies, 32 cores). |
| `sop_instance.hpp` / `sop_instance.cpp` | Robust TSPLIB `.sop` parser, bitset Floyd-Warshall transitive closure, automatic start/end candidate derivation, transitive reduction arc pruning, $O(N^3)$ Hungarian AP relaxation, and Directed Held-Karp MST subgradient ascent. |
| `validator.hpp` / `validator.cpp` | Independent from-scratch solution verifier checking permutation completeness, all raw matrix precedence constraints ($T[i][j] < 0$), and exact objective cost sum. |
| `shared_state.hpp` | Thread-safe global incumbent manager (`SharedSolverState`), wall-clock timer, and `stderr` best-cost update logger. |
| `heuristics.hpp` / `heuristics.cpp` | Multi-start randomized GRASP topological constructor, $O(N^2)$ forward/backward Or-Opt ($L \in \{1,2,3,4\}$), precedence-constrained SOP-2-Opt segment reversal, Don't-Look-Bits SOP-3-Exchange, and Large Neighborhood Search (LNS) Ruin-and-Recreate. |
| `history_table.hpp` | 4,096-shard lock-striped Zobrist hash table (`ShardedHistoryTable`) for prefix equivalence dominance pruning during parallel Branch-and-Bound. |
| `solver.hpp` / `solver.cpp` | `ExactSOPSolver` coordinating multi-threaded heuristic initialization, parallel Bidirectional Meet-in-the-Middle Bounded DP, and parallel Work-Stealing Branch-and-Bound with concurrent LNS intensification. |
| `cli.hpp` | Command-line argument parser (`CLIOptions`) supporting positional `.sop` filepath, `--time-limit`, `--threads`, `--output`, and `--no-header`. |
| `main.cpp` | Application entry point, orchestration, validation check, and stdout/CSV file output formatting. |
| `Makefile` | Build configuration supporting parallel compilation (`make -j`) and automatic header dependency tracking (`-MMD -MP`). |

## Section 1: Requirements and Installations Required

The program is written in standard **C++17** using only the C++ Standard Library and POSIX threads (`pthread`) / Linux system headers (`sched.h`, `dirent.h`) for hardware thread affinity and `/sys/devices/system/node` NUMA topology introspection. No external third-party libraries are required.

### System Requirements
* **Operating System**: Ubuntu / Debian Linux (or any POSIX-compliant Linux distribution)
* **Compiler**: `g++` (GCC 8.0 or newer with `-std=c++17` support; GCC 11+ recommended) and `make`
* **Target Hardware**: Scales from single-core machines up to 32+ core NUMA servers such as the **AMD Ryzen Threadripper 2990WX** (32 cores / 64 threads across 4 NUMA dies with 122 GiB RAM)

### Installation of Build Tools on Ubuntu
```bash
sudo apt-get update
sudo apt-get install -y build-essential g++ make
```

## Section 2: Exact Commands to Build and Run

### Building the Solver
Build using the included `Makefile` (or compile all `.cpp` translation units directly with `g++`):

```bash
# Option A: Build via Makefile
make -j

# Option B: Direct g++ compilation of all modular translation units
g++ -O3 -march=native -std=c++17 -pthread -Wall -Wextra \
    main.cpp sop_instance.cpp numa_topology.cpp validator.cpp heuristics.cpp solver.cpp \
    -o sop_solver
```

### Command-Line Interface Syntax
```bash
./sop_solver [filepath] [--time-limit SECONDS] [--threads N] [--output out.csv] [--no-header]
```

* `filepath`: Path to the input `.sop` instance file (required).
* `--time-limit SECONDS` (or `-t SECONDS`): Optional wall-clock time limit in seconds. Default: no time limit (runs until optimality is proven).
* `--threads N` (or `-j N`): Optional number of worker threads to use. Default: `1` (single-threaded). On an AMD Ryzen Threadripper 2990WX, pass `--threads 32` (or `--threads 64` for SMT).
* `--output out.csv` (or `-o out.csv`): Optional path to an output CSV file. When specified, the result is written to both `stdout` and `out.csv`. If `out.csv` already exists, the row is appended; the CSV header row is written to `out.csv` only if the file is new or empty (unless `--no-header` is passed).
* `--no-header`: Optional flag to omit the CSV header line on `stdout` (and on `out.csv` if creating a new file).

### Example Runs

**Example 1: Prove exact optimality on `prob.42.sop` and `rbg048a.sop` using 32 threads and append results to `results.csv`**
```bash
./sop_solver prob.42.sop --threads 32 --output results.csv
./sop_solver rbg048a.sop --threads 32 --output results.csv
```

**Example `stderr` Progress Log Output:**
```text
[Info] Loaded instance 'prob.42.sop' (N=42, starts=1, ends=1, AP_LB=174, HK_LB=226, threads=32, NUMA_nodes=4)
[New Best Cost] 514 (elapsed=0.0002s)
[New Best Cost] 319 (elapsed=0.0003s)
[New Best Cost] 260 (elapsed=0.0025s)
[New Best Cost] 243 (elapsed=0.0080s)
[Validator] Final path verified valid (permutation, all precedences, exact cost=243, optimal=yes).
```

**Example `stdout` CSV Output:**
```csv
Instance,Instance Size,Time,Enumerated Nodes,Best Cost,Best Path,Optimal
prob.42.sop,42,1.833516,8105216,243,0 11 4 13 7 27 34 6 22 19 33 16 25 24 39 1 36 32 18 38 5 2 8 23 3 35 15 9 14 10 20 12 30 31 26 17 28 21 40 29 37 41,yes
```

**Example 2: Run on a large instance (`R.200.1000.1.sop`) with a 60-second time limit, 32 threads, and `--no-header`**
```bash
./sop_solver R.200.1000.1.sop --time-limit 60 --threads 32 --output results.csv --no-header
```

## Section 3: Input and Output Format Descriptions

### Input Format (`.sop` TSPLIB Format)
The solver accepts TSPLIB-formatted Sequential Ordering Problem files:
1. **Metadata Header**: Keyword-value lines (`NAME`, `TYPE`, `COMMENT`, `DIMENSION`, `EDGE_WEIGHT_TYPE`, `EDGE_WEIGHT_FORMAT`). Keywords may appear in any order, with or without a colon separator.
2. **Matrix Section**: Introduced by `EDGE_WEIGHT_SECTION`, followed by the repeated dimension $N$ ($N \le 1000$), and then $N \times N$ whitespace-separated integers (`FULL_MATRIX`) that may wrap across arbitrary lines. An `EOF` token is optional and not required.
3. **Matrix Semantics ($T$)**:
   * Nodes are 0-indexed ($0 \dots N-1$) according to matrix row/column order.
   * $T[i][j] \ge 0$: Direct transition cost from node $i$ to node $j$.
   * $T[i][j] < 0$: Precedence constraint indicating node $i$ depends on node $j$ ($j$ must appear before $i$ in the tour).
   * $T[i][i] = 0$: Diagonal entries are ignored.
   * Start and end candidates are derived automatically from the precedence graph (nodes with zero predecessors and zero successors, respectively).

### Output Format (CSV)
All diagnostic and incumbent-update messages are written exclusively to `stderr`. `stdout` (and the optional `--output` CSV file) receives clean CSV output with the following columns:

| Column | Description |
| :--- | :--- |
| `Instance` | Input file basename (e.g., `prob.42.sop`) |
| `Instance Size` | Number of vertices $N$ |
| `Time` | Total wall-clock runtime in seconds (including preprocessing, heuristic search, optimality proof, and validation) |
| `Enumerated Nodes` | Total search states / nodes processed summed across all worker threads (see below for unit definition) |
| `Best Cost` | Total cost of the best valid Hamiltonian path found (`-1` if no feasible solution was found before timeout) |
| `Best Path` | Space-separated 0-based vertex indices of the tour (`empty` if no feasible solution was found) |
| `Optimal` | `yes` if and only if optimality was mathematically proven before any time limit expired; `no` otherwise |

**Definition of `Enumerated Nodes` Counted Unit**:
Because the solver employs a multi-phase exact algorithm, `Enumerated Nodes` is the exact sum across all threads of:
1. Local search neighbourhood passes executed during the initial portfolio heuristic / LNS phase, plus
2. Prefix/suffix state expansions $(S, u)$ evaluated during the Bidirectional Meet-in-the-Middle Bounded Dynamic Programming phase, plus
3. Search tree nodes visited during the Parallel Work-Stealing Branch-and-Bound phase.

## Section 4: Algorithmic Choices and Architecture Reasoning

### 1. Bitset Transitive Closure & Graph Reduction (`bitset.hpp`, `sop_instance.cpp`)
For $N \le 1000$, representing vertex subsets and precedence sets as `alignas(64) Bitset1024` (16 $\times$ 64-bit words) allows set union, intersection, and subset-readiness checks (`preds[v].is_subset_of(visited)`) to execute in a handful of register instructions. During preprocessing:
* We compute the full transitive closure of the precedence DAG via bitset Floyd-Warshall in $O(N^3 / 64)$ operations.
* **Transitive Reduction Pruning**: Any arc $(u, v)$ for which there exists an intermediate node $w$ with $u \prec w \prec v$ (`succs[u].intersects(preds[v])`) can never be traversed directly in a valid tour and is permanently eliminated ($T[u][v] = \infty$).

### 2. Dual Lower-Bound Engine: Assignment Problem (AP) + Directed Held-Karp MST (`sop_instance.cpp`)
A major challenge in SOP instances varies by instance structure:
* **Dense-precedence / asymmetric instances (`rbg048a.sop`)**: The Assignment Problem (AP) relaxation solved via the $O(N^3)$ Hungarian Shortest Augmenting Path algorithm provides a tight lower bound (`AP_LB = 337` vs. optimal `351`) and yields exact dual potentials $(u_i, v_j)$ with non-negative reduced costs $\bar{c}_{ij} = c_{ij} - u_i - v_j \ge 0$.
* **2-Cycle-heavy instances (`prob.42.sop`)**: Pure AP relaxation suffers from cheap 2-cycles ($u \leftrightarrow v$), yielding a loose bound (`AP_LB = 174` vs. optimal `243`). To overcome 2-cycles, we also solve the **Directed Held-Karp Minimum Spanning Tree (HK-MST) Lagrangian Relaxation** via subgradient ascent on vertex in-degree and out-degree multipliers $(\pi^{\text{out}}, \pi^{\text{in}})$, raising the root bound from `174` to `226`.

### 3. Mask-Factored Bidirectional Meet-in-the-Middle Bounded DP (`solver.cpp`)
In SOP, many different partial permutations visit the exact same subset of vertices $S$ and end at the same vertex $u$. By grouping states by their **order ideal** (visited bitmask $S$):
* Both the **Induced Held-Karp Minimum Spanning Tree** $\text{MST}_{\text{HK}}(V \setminus S)$ and the **Dynamic In/Out-Degree Bounds** on the unvisited complement $V \setminus S$ depend **only on the bitmask $S$**, not on the endpoint $u \in S$.
* Consequently, we compute Prim's MST and the unvisited degree bounds **once per bitmask $S$** and reuse them across all endpoints $u \in S$ and all ready transitions $v \in V \setminus S$.
* Furthermore, the precedence poset is often asymmetric between the start and end of the tour. For example, on `rbg048a.sop`, forward expansion has thousands of masks at depth 10, whereas backward expansion from the sink node collapses to a **single feasible suffix mask** at depth 11. Our adaptive bidirectional engine always expands whichever direction (forward prefix layer vs. backward suffix layer) has the smaller state frontier, meeting in the middle when $d_{\text{fwd}} + d_{\text{bwd}} = N$.

### 4. Parallel Work-Stealing Branch-and-Bound with Prefix Dominance Table (`history_table.hpp`, `solver.cpp`)
When $N$ is large (e.g., $N = 200 \dots 1000$) and the exact DP frontier exceeds memory thresholds, the solver seamlessly transitions into a **Parallel Work-Stealing Branch-and-Bound** engine:
* **Sharded Prefix Equivalence History Table**: Stores Zobrist-hashed `(visited_bitset, last_node) -> best_prefix_cost` entries across 4,096 fine-grained lock shards, immediately pruning any branch that reaches an already-seen prefix subset with a higher or equal cost (Gouveia & Ruthmair / Shobaki & Jamal prefix dominance).
* **Concurrent LNS Upper-Bound Intensification**: On multi-core runs ($\ge 4$ threads), 75% of threads execute exact work-stealing B&B while 25% of threads run continuous Large Neighborhood Search (Ruin-and-Recreate + Don't-Look-Bits SOP-3-Exchange + SOP-2-Opt + Or-Opt), broadcasting tighter upper bounds via `std::atomic<int64_t>` to accelerate B&B pruning in real time.

### 5. AMD Ryzen Threadripper 2990WX NUMA Awareness (`numa_topology.cpp`)
The AMD Ryzen Threadripper 2990WX features a 4-die "WX" Dynamic Local Mode topology where **Nodes 0 and 2** have direct DDR4 memory controllers while **Nodes 1 and 3** are compute dies connected via Infinity Fabric. Our `NumaTopology` manager inspects `/sys/devices/system/node/node*` at startup:
* It prioritizes memory-controller-owning NUMA nodes first and distributes threads round-robin across dies using `pthread_setaffinity_np` to maximize aggregate L3 cache capacity and memory channel utilization.
* During parallel layer expansion and B&B, each worker thread writes exclusively to its own thread-local hash table / state buffer (`local_next[tid]`) with 64-byte cache-line-aligned bitsets (`alignas(64) Bitset1024`), eliminating cross-die cache-line ping-pong and false sharing.
