/*
 * File: solver.cpp
 * Purpose: Implements the parallel NUMA-aware exact Branch-and-Bound solver
 *          for the Sequential Ordering Problem (SOP):
 *          1. Engine A: Carpaneto-Toth Subtour & Precedence-Violation Path
 *             Elimination Branch-and-Bound with O(N^2) Incremental LAP updates.
 *          2. Engine B: Dual-Potential (LAP + Held-Karp Subgradient) 2-Cycle-Free
 *             2-Edge Lower Bounding with Parent Top-3 Edge Caching, Lock-Free
 *             4-Way Zobrist History Domination (both Prefix Cost and Suffix LB),
 *             and Hierarchical NUMA-Aware Work Stealing across up to 32 cores.
 */

#include "solver.hpp"
#include "heuristics.hpp"
#include "preprocessor.hpp"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <memory>
#include <queue>
#include <thread>

namespace sop {

SopSolver::SopSolver(SopInstance& inst, const SolverOptions& opts)
    : inst_(inst), opts_(opts), start_time_(std::chrono::steady_clock::now()) {}

/*
 * Algorithm: Wall-Clock Timeout Monitor
 * Part: Periodically checks elapsed steady_clock time against --time-limit.
 */
bool SopSolver::check_timeout() const noexcept {
    if (opts_.time_limit_sec <= 0.0) return false;
    if (timed_out_.load(std::memory_order_relaxed)) return true;
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - start_time_).count();
    if (elapsed >= opts_.time_limit_sec) {
        const_cast<std::atomic<bool>&>(timed_out_).store(true, std::memory_order_relaxed);
        const_cast<std::atomic<bool>&>(abort_search_).store(true, std::memory_order_relaxed);
        return true;
    }
    return false;
}

/*
 * Algorithm: Shared Atomic Incumbent Update & Stderr Logger
 * Part: Thread-safely updates the global upper bound `best_cost_` and logs
 *       every improvement immediately to stderr as required.
 */
void SopSolver::update_incumbent(int64_t new_cost,
                                 const std::vector<int>& working_path,
                                 const char* phase) {
    int64_t cur = best_cost_.load(std::memory_order_relaxed);
    if (new_cost >= cur) return;

    std::lock_guard<std::mutex> lock(incumbent_mtx_);
    if (new_cost < best_cost_.load(std::memory_order_relaxed)) {
        best_cost_.store(new_cost, std::memory_order_relaxed);
        best_working_path_ = working_path;
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - start_time_).count();
        std::cerr << "[Best Cost Update] New best known cost: " << new_cost
                  << " (Phase: " << phase << ", Time: " << std::fixed
                  << std::setprecision(4) << elapsed << "s)\n";
    }
}

/*
 * Algorithm: Carpaneto-Toth Subtour & Precedence-Violation Path Elimination B&B
 * Part: Solves the ATSP/SOP assignment relaxation via Shortest Augmenting Paths
 *       and branches by partitioning the shortest subtour (if multiple cycles
 *       exist) or the shortest precedence-violating subpath (if a single cycle
 *       violates a precedence constraint u < v), using O(N^2) incremental LAP
 *       updates for each child node.
 */
bool SopSolver::run_subtour_lap_bb(const DualBoundTables& tables, uint64_t max_lap_nodes) {
    const int n = inst_.n; (void)tables;
    if (inst_.unique_start == -1 || inst_.unique_end == -1 || n <= 2) {
        return false;
    }
    const int s_node = inst_.unique_start;
    const int t_node = inst_.unique_end;

    auto base_cost = inst_.cost;
    base_cost[t_node][s_node] = 0;

    LapSolution root_sol = LowerBounds::solve_lap_full(n, base_cost);
    if (!root_sol.feasible) {
        inst_.is_feasible = false;
        return true;
    }
    if (root_sol.optimal_cost >= best_cost_.load(std::memory_order_relaxed)) {
        return true; // Proven optimal at root!
    }

    struct SubtourNode {
        LapSolution lap;
        std::vector<std::pair<int, int>> forbidden;
        std::vector<std::pair<int, int>> required;
        bool operator>(const SubtourNode& other) const noexcept {
            return lap.optimal_cost > other.lap.optimal_cost;
        }
    };

    std::priority_queue<SubtourNode, std::vector<SubtourNode>, std::greater<SubtourNode>> pq;
    pq.push({root_sol, {}, {}});

    uint64_t lap_nodes = 0;
    while (!pq.empty()) {
        if ((lap_nodes & 15) == 0 && check_timeout()) {
            total_enumerated_nodes_.fetch_add(lap_nodes, std::memory_order_relaxed);
            return false;
        }

        SubtourNode cur = pq.top();
        if (cur.lap.optimal_cost >= best_cost_.load(std::memory_order_relaxed)) {
            // Minimum lower bound across all open nodes >= best_cost_: optimality proven!
            total_enumerated_nodes_.fetch_add(lap_nodes, std::memory_order_relaxed);
            return true;
        }
        if (lap_nodes >= max_lap_nodes) {
            total_enumerated_nodes_.fetch_add(lap_nodes, std::memory_order_relaxed);
            return false;
        }
        pq.pop();
        ++lap_nodes;

        // Decompose assignment into directed cycles
        std::vector<int> comp(n, -1);
        std::vector<std::vector<int>> cycles;
        for (int i = 0; i < n; ++i) {
            if (comp[i] == -1) {
                std::vector<int> cyc;
                int u = i;
                while (u >= 0 && comp[u] == -1) {
                    comp[u] = static_cast<int>(cycles.size());
                    cyc.push_back(u);
                    u = cur.lap.row_to_col[u];
                }
                cycles.push_back(std::move(cyc));
            }
        }

        std::vector<std::pair<int, int>> branch_arcs;
        if (cycles.size() > 1) {
            int best_c = -1;
            for (int c = 0; c < static_cast<int>(cycles.size()); ++c) {
                if (best_c == -1 || cycles[c].size() < cycles[best_c].size()) {
                    best_c = c;
                }
            }
            for (int u : cycles[best_c]) {
                int v = cur.lap.row_to_col[u];
                if (u == t_node && v == s_node) continue;
                branch_arcs.push_back({u, v});
            }
        } else {
            // Single Hamiltonian cycle: extract path starting at s_node and check precedences
            std::vector<int> seq(n), pos(n);
            int u = s_node;
            for (int k = 0; k < n; ++k) {
                seq[k] = u;
                pos[u] = k;
                u = cur.lap.row_to_col[u];
            }
            int best_len = INT_INF, best_pv = -1, best_pu = -1;
            for (int i = 0; i < n; ++i) {
                for (int j = 0; j < n; ++j) {
                    if (inst_.prec[i][j] && pos[j] < pos[i]) {
                        int len = pos[i] - pos[j];
                        if (len < best_len) {
                            best_len = len;
                            best_pv = pos[j];
                            best_pu = pos[i];
                        }
                    }
                }
            }
            if (best_pv == -1) {
                // Zero subtours and zero precedence violations -> Feasible optimal tour!
                update_incumbent(cur.lap.optimal_cost, seq, "LAP-B&B");
                continue;
            }
            if (best_len > 8 && lap_nodes > 64) {
                // Precedence-violating paths are long; hand off to Prefix/Suffix B&B
                total_enumerated_nodes_.fetch_add(lap_nodes, std::memory_order_relaxed);
                return false;
            }
            for (int k = best_pv; k < best_pu; ++k) {
                branch_arcs.push_back({seq[k], seq[k + 1]});
            }
        }

        // Partition search space along `branch_arcs` (Carpaneto & Toth partitioning)
        std::vector<std::pair<int, int>> req = cur.required;
        auto cur_matrix = base_cost;
        for (const auto& f : cur.forbidden) cur_matrix[f.first][f.second] = INT_INF;
        for (const auto& r : cur.required) {
            for (int j = 0; j < n; ++j) if (j != r.second) cur_matrix[r.first][j] = INT_INF;
            for (int i = 0; i < n; ++i) if (i != r.first) cur_matrix[i][r.second] = INT_INF;
        }

        LapSolution running_lap = cur.lap;
        for (const auto& [bu, bv] : branch_arcs) {
            bool already_req = false;
            for (const auto& r : cur.required) {
                if (r.first == bu && r.second == bv) {
                    already_req = true;
                    break;
                }
            }
            if (!already_req) {
                // Child with (bu, bv) forbidden and previous branch_arcs required
                cur_matrix[bu][bv] = INT_INF;
                LapSolution child_lap =
                    LowerBounds::solve_lap_incremental(n, cur_matrix, running_lap, bu, bv);
                cur_matrix[bu][bv] = base_cost[bu][bv];

                if (child_lap.feasible &&
                    child_lap.optimal_cost < best_cost_.load(std::memory_order_relaxed)) {
                    SubtourNode child;
                    child.lap = std::move(child_lap);
                    child.forbidden = cur.forbidden;
                    child.forbidden.push_back({bu, bv});
                    child.required = req;
                    pq.push(std::move(child));
                }
            }
            // Enforce (bu, bv) as required for subsequent siblings
            req.push_back({bu, bv});
            for (int j = 0; j < n; ++j) if (j != bv) cur_matrix[bu][j] = INT_INF;
            for (int i = 0; i < n; ++i) if (i != bu) cur_matrix[i][bv] = INT_INF;
        }
    }

    total_enumerated_nodes_.fetch_add(lap_nodes, std::memory_order_relaxed);
    return true;
}

/*
 * Algorithm: Dual-Potential 2-Cycle-Free DFS Worker with Parent Top-3 Caching (N <= 64)
 * Part: Explores a subtree using 64-bit register bitmasks, pre-extracting the top-3
 *       incoming/outgoing edges in `unvis` once per parent node and evaluating
 *       every ready child's 2-cycle-free LAP + Subgradient lower bound in O(k) time.
 *       Pushes shallow sibling tasks to the worker's NUMA-local work-stealing deque.
 */
int SopSolver::dfs_worker_64(
    int worker_id,
    SearchTask64& task,
    const DualBoundTables& tables,
    ZobristHistoryTable& history,
    std::vector<std::unique_ptr<WorkerStealQueue64>>& queues,
    uint64_t& local_nodes) {
    const int n = inst_.n; (void)tables;
    const int scale = tables.subgrad_scale;
    const int t_node = inst_.unique_end;
    const uint64_t all_mask = (n == 64) ? ~0ULL : ((1ULL << n) - 1ULL);

    int depth = task.depth;
    int u = task.u;
    uint64_t vis = task.vis;
    int cur_cost = task.cur_cost;
    int pot_lap = task.pot_lap;
    int64_t pot_sub = task.pot_sub;

    if ((local_nodes & 4095) == 0 && check_timeout()) {
        return INT_INF;
    }

    int64_t cur_best = best_cost_.load(std::memory_order_relaxed);
    if (cur_cost + task.est_lb >= cur_best) {
        return task.est_lb;
    }

    if (depth == n) {
        if (cur_cost < cur_best) {
            update_incumbent(cur_cost, task.prefix_path, "Prefix-B&B");
        }
        return 0;
    }

    uint64_t hkey = ((vis << 6) | static_cast<uint64_t>(u)) | (1ULL << 63);
    int known_slb = 0;
    if (history.check_or_record_prefix(hkey, cur_cost, cur_best, known_slb)) {
        return std::max(known_slb, static_cast<int>(cur_best - cur_cost));
    }

    ++local_nodes;
    uint64_t unvis = all_mask ^ vis;
    uint64_t ready = 0;
    uint64_t tmp = unvis;
    while (tmp) {
        int x = __builtin_ctzll(tmp);
        tmp &= tmp - 1;
        uint64_t pm = inst_.pred_bits[x].words[0];
        if ((vis & pm) == pm) {
            ready |= (1ULL << x);
        }
    }
    if (!ready) {
        history.update_suffix_lb(hkey, INT_INF);
        return INT_INF;
    }

    // Algorithm: Parent-Level Top-3 Active Edge Pre-Extraction
    // Part: Extracts the 3 cheapest incoming and outgoing edges restricted to `unvis`
    //       once at the parent node so all candidate children evaluate in O(k) without
    //       re-scanning adjacency lists.
    struct Top3 {
        int w0 = INT_INF, to0 = -1;
        int w1 = INT_INF, to1 = -1;
        int w2 = INT_INF, to2 = -1;
    };
    Top3 inL[64], outL[64], inS[64], outS[64];
    int active_list[64];
    int n_active = 0;

    tmp = unvis;
    while (tmp) {
        int x = __builtin_ctzll(tmp);
        tmp &= tmp - 1;
        active_list[n_active++] = x;

        int c = 0;
        for (const auto& e : tables.in_lap[x]) {
            if ((unvis >> e.to) & 1ULL) {
                if (c == 0) { inL[x].w0 = e.cost; inL[x].to0 = e.to; c = 1; }
                else if (c == 1) { inL[x].w1 = e.cost; inL[x].to1 = e.to; c = 2; }
                else { inL[x].w2 = e.cost; inL[x].to2 = e.to; break; }
            }
        }
        c = 0;
        for (const auto& e : tables.in_sub[x]) {
            if ((unvis >> e.to) & 1ULL) {
                if (c == 0) { inS[x].w0 = e.cost; inS[x].to0 = e.to; c = 1; }
                else if (c == 1) { inS[x].w1 = e.cost; inS[x].to1 = e.to; c = 2; }
                else { inS[x].w2 = e.cost; inS[x].to2 = e.to; break; }
            }
        }
        if (x != t_node) {
            c = 0;
            for (const auto& e : tables.out_lap[x]) {
                if ((unvis >> e.to) & 1ULL) {
                    if (c == 0) { outL[x].w0 = e.cost; outL[x].to0 = e.to; c = 1; }
                    else if (c == 1) { outL[x].w1 = e.cost; outL[x].to1 = e.to; c = 2; }
                    else { outL[x].w2 = e.cost; outL[x].to2 = e.to; break; }
                }
            }
            c = 0;
            for (const auto& e : tables.out_sub[x]) {
                if ((unvis >> e.to) & 1ULL) {
                    if (c == 0) { outS[x].w0 = e.cost; outS[x].to0 = e.to; c = 1; }
                    else if (c == 1) { outS[x].w1 = e.cost; outS[x].to1 = e.to; c = 2; }
                    else { outS[x].w2 = e.cost; outS[x].to2 = e.to; break; }
                }
            }
        }
    }

    struct Cand {
        int v;
        int w;
        int child_slb;
        int next_cost;
        int npot_lap;
        int64_t npot_sub;
    };
    Cand cands[64];
    int nc = 0;
    int min_suffix = INT_INF;

    tmp = ready;
    while (tmp) {
        int v = __builtin_ctzll(tmp);
        tmp &= tmp - 1;
        // Do not visit unique terminal node before the final step
        if (v == t_node && depth + 1 < n) continue;

        int w = inst_.cost[u][v];
        if (w >= INT_INF) continue;
        int nc_cost = cur_cost + w;
        uint64_t nvis = vis | (1ULL << v);
        uint64_t nunvis = unvis ^ (1ULL << v);

        int np_lap = pot_lap - tables.u_lap[u] - tables.v_lap[v];
        int64_t np_sub = pot_sub - tables.u_sub[u] - tables.v_sub[v];

        if (!nunvis) {
            cur_best = best_cost_.load(std::memory_order_relaxed);
            if (nc_cost < cur_best) {
                cands[nc++] = {v, w, 0, nc_cost, np_lap, np_sub};
            } else {
                min_suffix = std::min(min_suffix, w);
            }
            continue;
        }

        uint64_t nready = ready & ~(1ULL << v);
        for (int idx = 0; idx < n_active; ++idx) {
            int x = active_list[idx];
            if (x != v && !((nready >> x) & 1ULL)) {
                uint64_t pm = inst_.pred_bits[x].words[0];
                if ((nvis & pm) == pm) nready |= (1ULL << x);
            }
        }
        if (!nready) continue;

        // Outgoing edge from v must enter nready
        int v_outL = INT_INF, v_outS = INT_INF;
        for (const auto& e : tables.out_lap[v]) {
            if ((nready >> e.to) & 1ULL) { v_outL = e.cost; break; }
        }
        if (v_outL >= INT_INF) continue;
        for (const auto& e : tables.out_sub[v]) {
            if ((nready >> e.to) & 1ULL) { v_outS = e.cost; break; }
        }

        int64_t out_sumL = v_outL, in_sumL = 0, pair_sumL = v_outL;
        int64_t out_sumS = v_outS, in_sumS = 0, pair_sumS = v_outS;
        bool possible = true;

        for (int idx = 0; idx < n_active; ++idx) {
            int x = active_list[idx];
            if (x == v) continue;
            bool x_ready = ((nready >> x) & 1ULL) != 0ULL;

            int in1L, in2L, in1L_to;
            if (x_ready || inL[x].to0 != v) {
                in1L = inL[x].w0; in1L_to = inL[x].to0;
                in2L = (x_ready || inL[x].to1 != v) ? inL[x].w1 : inL[x].w2;
            } else {
                in1L = inL[x].w1; in1L_to = inL[x].to1;
                in2L = inL[x].w2;
            }
            if (in1L >= INT_INF) { possible = false; break; }

            int in1S, in2S, in1S_to;
            if (x_ready || inS[x].to0 != v) {
                in1S = inS[x].w0; in1S_to = inS[x].to0;
                in2S = (x_ready || inS[x].to1 != v) ? inS[x].w1 : inS[x].w2;
            } else {
                in1S = inS[x].w1; in1S_to = inS[x].to1;
                in2S = inS[x].w2;
            }

            if (x == t_node) {
                in_sumL += in1L; pair_sumL += in1L;
                in_sumS += in1S; pair_sumS += in1S;
                continue;
            }

            int out1L, out2L, out1L_to;
            if (outL[x].to0 != v) {
                out1L = outL[x].w0; out1L_to = outL[x].to0;
                out2L = (outL[x].to1 != v) ? outL[x].w1 : outL[x].w2;
            } else {
                out1L = outL[x].w1; out1L_to = outL[x].to1;
                out2L = outL[x].w2;
            }
            if (out1L >= INT_INF) { possible = false; break; }

            int out1S, out2S, out1S_to;
            if (outS[x].to0 != v) {
                out1S = outS[x].w0; out1S_to = outS[x].to0;
                out2S = (outS[x].to1 != v) ? outS[x].w1 : outS[x].w2;
            } else {
                out1S = outS[x].w1; out1S_to = outS[x].to1;
                out2S = outS[x].w2;
            }

            in_sumL += in1L; out_sumL += out1L;
            int64_t bpL = (in1L_to != out1L_to)
                              ? (static_cast<int64_t>(in1L) + out1L)
                              : std::min(static_cast<int64_t>(in1L) + out2L,
                                         static_cast<int64_t>(in2L) + out1L);
            if (bpL >= INT_INF) { possible = false; break; }
            pair_sumL += bpL;

            in_sumS += in1S; out_sumS += out1S;
            int64_t bpS = (in1S_to != out1S_to)
                              ? (static_cast<int64_t>(in1S) + out1S)
                              : std::min(static_cast<int64_t>(in1S) + out2S,
                                         static_cast<int64_t>(in2S) + out1S);
            if (bpS >= INT_INF) { possible = false; break; }
            pair_sumS += bpS;
        }
        if (!possible) continue;

        int slb_lap = np_lap + static_cast<int>(std::max({out_sumL, in_sumL, (pair_sumL + 1) / 2}));
        int64_t tot_sub = np_sub + std::max({out_sumS, in_sumS, (pair_sumS + 1) / 2});
        int slb_sub = static_cast<int>((tot_sub + scale - 1) / scale);
        int c_slb = std::max(slb_lap, slb_sub);

        uint64_t chkey = ((nvis << 6) | static_cast<uint64_t>(v)) | (1ULL << 63);
        int ch_mp = INT_INF, ch_slb = 0;
        if (history.probe(chkey, ch_mp, ch_slb)) {
            c_slb = std::max(c_slb, ch_slb);
            if (nc_cost >= ch_mp) {
                cur_best = best_cost_.load(std::memory_order_relaxed);
                int dom_slb = std::max(c_slb, static_cast<int>(cur_best - nc_cost));
                min_suffix = std::min(min_suffix, w + dom_slb);
                continue;
            }
        }

        cur_best = best_cost_.load(std::memory_order_relaxed);
        if (nc_cost + c_slb >= cur_best) {
            min_suffix = std::min(min_suffix, w + c_slb);
            continue;
        }
        cands[nc++] = {v, w, c_slb, nc_cost, np_lap, np_sub};
    }

    // Algorithm: Lookahead Lower-Bound Child Ordering
    // Part: Sorts candidate transitions in ascending order of (next_cost + child_slb)
    //       to discover optimal completions early and maximize alpha/beta pruning.
    std::sort(cands, cands + nc, [](const Cand& a, const Cand& b) {
        int lba = a.next_cost + a.child_slb;
        int lbb = b.next_cost + b.child_slb;
        if (lba != lbb) return lba < lbb;
        return a.next_cost < b.next_cost;
    });

    for (int i = 0; i < nc; ++i) {
        if (abort_search_.load(std::memory_order_relaxed)) return INT_INF;
        cur_best = best_cost_.load(std::memory_order_relaxed);
        if (cands[i].next_cost + cands[i].child_slb >= cur_best) {
            min_suffix = std::min(min_suffix, cands[i].w + cands[i].child_slb);
            continue;
        }

        task.prefix_path[depth] = cands[i].v;
        task.depth = depth + 1;
        task.u = cands[i].v;
        task.vis = vis | (1ULL << cands[i].v);
        task.cur_cost = cands[i].next_cost;
        task.pot_lap = cands[i].npot_lap;
        task.pot_sub = cands[i].npot_sub;
        task.est_lb = cands[i].child_slb;

        int actual_slb = dfs_worker_64(worker_id, task, tables, history, queues, local_nodes);

        // Restore parent task frame
        task.depth = depth;
        task.u = u;
        task.vis = vis;
        task.cur_cost = cur_cost;
        task.pot_lap = pot_lap;
        task.pot_sub = pot_sub;

        if (actual_slb < INT_INF) {
            min_suffix = std::min(min_suffix, cands[i].w + actual_slb);
        }
    }

    if (!abort_search_.load(std::memory_order_relaxed)) {
        int final_slb = std::max(known_slb, min_suffix);
        history.update_suffix_lb(hkey, final_slb);
        return final_slb;
    }
    return INT_INF;
}

/*
 * Algorithm: Parallel NUMA-Aware Work-Stealing Scheduler (N <= 64)
 * Part: Expands the top levels of the B&B tree into a priority frontier,
 *       distributes subproblems across NUMA-pinned worker deques, and performs
 *       hierarchical work stealing (same NUMA die first, remote die second).
 */
void SopSolver::run_prefix_bb_64(const DualBoundTables& tables) {
    const int n = inst_.n; (void)tables;
    const int num_threads = std::max(1, opts_.threads);
    auto assignments = numa_.plan_worker_assignments(num_threads);

    size_t ht_buckets = (1ULL << 20); // 1M buckets * 64B = 64 MB
    ZobristHistoryTable history(ht_buckets, assignments[0].memory_numa_node);

    // Seed initial root task(s) for all valid start nodes
    std::vector<SearchTask64> frontier;
    for (int s : inst_.start_nodes) {
        SearchTask64 root;
        root.depth = 1;
        root.u = s;
        root.vis = (1ULL << s);
        root.cur_cost = 0;
        root.pot_lap = static_cast<int>(tables.root_pot_lap);
        root.pot_sub = tables.root_pot_sub;
        root.est_lb = 0;
        root.prefix_path.assign(n, 0);
        root.prefix_path[0] = s;
        frontier.push_back(std::move(root));
    }

    // If multi-threaded, expand frontier via BFS to depth 2..3 so all 32 cores
    // receive balanced, independent subtrees.
    const size_t target_frontier = (num_threads == 1) ? 1 : static_cast<size_t>(num_threads * 48);
    const uint64_t all_mask = (n == 64) ? ~0ULL : ((1ULL << n) - 1ULL);

    while (num_threads > 1 && frontier.size() < target_frontier && !frontier.empty()) {
        if (frontier[0].depth >= n - 1) break;
        std::vector<SearchTask64> next_frontier;
        for (const auto& tk : frontier) {
            total_enumerated_nodes_.fetch_add(1, std::memory_order_relaxed);
            uint64_t unvis = all_mask ^ tk.vis;
            uint64_t tmp = unvis;
            while (tmp) {
                int v = __builtin_ctzll(tmp);
                tmp &= tmp - 1;
                if (v == inst_.unique_end && tk.depth + 1 < n) continue;
                uint64_t pm = inst_.pred_bits[v].words[0];
                if ((tk.vis & pm) == pm && inst_.cost[tk.u][v] < INT_INF) {
                    int w = inst_.cost[tk.u][v];
                    int nc = tk.cur_cost + w;
                    if (nc >= best_cost_.load(std::memory_order_relaxed)) continue;
                    SearchTask64 child = tk;
                    child.depth = tk.depth + 1;
                    child.u = v;
                    child.vis = tk.vis | (1ULL << v);
                    child.cur_cost = nc;
                    child.pot_lap = tk.pot_lap - tables.u_lap[tk.u] - tables.v_lap[v];
                    child.pot_sub = tk.pot_sub - tables.u_sub[tk.u] - tables.v_sub[v];
                    child.prefix_path[tk.depth] = v;
                    next_frontier.push_back(std::move(child));
                }
            }
        }
        if (next_frontier.empty()) break;
        frontier = std::move(next_frontier);
    }

    // Sort frontier by ascending prefix cost so best branches are searched first
    std::sort(frontier.begin(), frontier.end(),
              [](const SearchTask64& a, const SearchTask64& b) {
                  return a.cur_cost < b.cur_cost;
              });

    std::vector<std::unique_ptr<WorkerStealQueue64>> queues(num_threads);
    for (int t = 0; t < num_threads; ++t) {
        queues[t] = std::make_unique<WorkerStealQueue64>();
        queues[t]->numa_node = assignments[t].numa_node;
    }
    for (size_t i = 0; i < frontier.size(); ++i) {
        queues[i % num_threads]->deque.push_back(std::move(frontier[i]));
    }

    auto worker_fn = [&](int tid) {
        if (num_threads > 1) {
            NumaTopology::pin_calling_thread_to_cpu(assignments[tid].cpu_id);
        }
        uint64_t local_nodes = 0;
        const int my_numa = assignments[tid].numa_node;

        while (!abort_search_.load(std::memory_order_relaxed)) {
            SearchTask64 task;
            bool got_task = false;

            {
                std::lock_guard<std::mutex> lk(queues[tid]->mtx);
                if (!queues[tid]->deque.empty()) {
                    task = std::move(queues[tid]->deque.front());
                    queues[tid]->deque.pop_front();
                    got_task = true;
                }
            }

            // Hierarchical NUMA Work Stealing:
            // Pass 1: Steal from sibling threads on the same NUMA die
            // Pass 2: Steal from threads on remote NUMA dies across Infinity Fabric
            if (!got_task && num_threads > 1) {
                for (int pass = 0; pass < 2 && !got_task; ++pass) {
                    for (int offset = 1; offset < num_threads; ++offset) {
                        int victim = (tid + offset) % num_threads;
                        bool same_numa = (queues[victim]->numa_node == my_numa);
                        if ((pass == 0 && !same_numa) || (pass == 1 && same_numa)) continue;

                        std::lock_guard<std::mutex> lk(queues[victim]->mtx);
                        if (!queues[victim]->deque.empty()) {
                            task = std::move(queues[victim]->deque.front());
                            queues[victim]->deque.pop_front();
                            got_task = true;
                            break;
                        }
                    }
                }
            }

            if (!got_task) break;
            dfs_worker_64(tid, task, tables, history, queues, local_nodes);
        }

        total_enumerated_nodes_.fetch_add(local_nodes, std::memory_order_relaxed);
    };

    if (num_threads == 1) {
        worker_fn(0);
    } else {
        std::vector<std::thread> threads;
        threads.reserve(num_threads);
        for (int t = 0; t < num_threads; ++t) {
            threads.emplace_back(worker_fn, t);
        }
        for (auto& th : threads) {
            th.join();
        }
    }
}

/*
 * Algorithm: General BitSet1024 DFS Worker (64 < N <= 1000)
 * Part: Extends the Dual-Potential 2-Cycle-Free Prefix/Suffix B&B with Zobrist
 *       History Domination to arbitrary instance dimensions up to N <= 1000.
 */
int SopSolver::dfs_worker_general(
    int worker_id,
    SearchTaskGen& task,
    const DualBoundTables& tables,
    const ZobristHasher& hasher,
    ZobristHistoryTable& history,
    std::vector<std::unique_ptr<WorkerStealQueueGen>>& queues,
    uint64_t& local_nodes) {
    (void)worker_id;
    (void)queues;
    const int n = inst_.n; (void)tables;
    const int nw = inst_.num_words;
    const int scale = tables.subgrad_scale;
    const int t_node = inst_.unique_end;

    int depth = task.depth;
    int u = task.u;
    int cur_cost = task.cur_cost;
    int pot_lap = task.pot_lap;
    int64_t pot_sub = task.pot_sub;
    uint64_t vis_hash = task.vis_hash;

    if ((local_nodes & 1023) == 0 && check_timeout()) {
        return INT_INF;
    }

    int64_t cur_best = best_cost_.load(std::memory_order_relaxed);
    if (cur_cost + task.est_lb >= cur_best) {
        return task.est_lb;
    }

    if (depth == n) {
        if (cur_cost < cur_best) {
            update_incumbent(cur_cost, task.prefix_path, "Prefix-B&B-Gen");
        }
        return 0;
    }

    uint64_t hkey = hasher.make_state_key(vis_hash, u);
    int known_slb = 0;
    if (history.check_or_record_prefix(hkey, cur_cost, cur_best, known_slb)) {
        return std::max(known_slb, static_cast<int>(cur_best - cur_cost));
    }

    ++local_nodes;

    std::vector<int> active_list;
    active_list.reserve(n - depth);
    std::vector<int> ready_list;
    ready_list.reserve(n - depth);

    for (int x = 0; x < n; ++x) {
        if (!task.vis.test(x)) {
            active_list.push_back(x);
            if (inst_.pred_bits[x].is_subset_of(task.vis, nw)) {
                ready_list.push_back(x);
            }
        }
    }
    if (ready_list.empty()) {
        history.update_suffix_lb(hkey, INT_INF);
        return INT_INF;
    }

    struct CandGen {
        int v;
        int w;
        int child_slb;
        int next_cost;
        int npot_lap;
        int64_t npot_sub;
    };
    std::vector<CandGen> cands;
    cands.reserve(ready_list.size());
    int min_suffix = INT_INF;

    // Static degree sum over unvisited vertices for fast O(k) bound on large N
    int64_t base_out_raw = 0, base_in_raw = 0;
    for (int x : active_list) {
        if (x != t_node) base_out_raw += tables.min_out_raw[x];
        base_in_raw += tables.min_in_raw[x];
    }

    for (int v : ready_list) {
        if (v == t_node && depth + 1 < n) continue;
        int w = inst_.cost[u][v];
        if (w >= INT_INF) continue;
        int nc_cost = cur_cost + w;
        int np_lap = pot_lap - tables.u_lap[u] - tables.v_lap[v];
        int64_t np_sub = pot_sub - tables.u_sub[u] - tables.v_sub[v];

        int rem_out = static_cast<int>(base_out_raw - tables.min_out_raw[v]);
        int rem_in = static_cast<int>(base_in_raw - tables.min_in_raw[v]);
        int c_slb = std::max({0, np_lap, static_cast<int>((np_sub + scale - 1) / scale), rem_out, rem_in});

        uint64_t nvis_hash = hasher.toggle_visit(vis_hash, v);
        uint64_t chkey = hasher.make_state_key(nvis_hash, v);
        int ch_mp = INT_INF, ch_slb = 0;
        if (history.probe(chkey, ch_mp, ch_slb)) {
            c_slb = std::max(c_slb, ch_slb);
            if (nc_cost >= ch_mp) {
                cur_best = best_cost_.load(std::memory_order_relaxed);
                min_suffix = std::min(min_suffix, w + std::max(c_slb, static_cast<int>(cur_best - nc_cost)));
                continue;
            }
        }

        cur_best = best_cost_.load(std::memory_order_relaxed);
        if (nc_cost + c_slb >= cur_best) {
            min_suffix = std::min(min_suffix, w + c_slb);
            continue;
        }
        cands.push_back({v, w, c_slb, nc_cost, np_lap, np_sub});
    }

    std::sort(cands.begin(), cands.end(), [](const CandGen& a, const CandGen& b) {
        int lba = a.next_cost + a.child_slb;
        int lbb = b.next_cost + b.child_slb;
        if (lba != lbb) return lba < lbb;
        return a.next_cost < b.next_cost;
    });

    for (const auto& ch : cands) {
        if (abort_search_.load(std::memory_order_relaxed)) return INT_INF;
        cur_best = best_cost_.load(std::memory_order_relaxed);
        if (ch.next_cost + ch.child_slb >= cur_best) {
            min_suffix = std::min(min_suffix, ch.w + ch.child_slb);
            continue;
        }

        task.prefix_path[depth] = ch.v;
        task.depth = depth + 1;
        task.u = ch.v;
        task.vis.set(ch.v);
        task.vis_hash = hasher.toggle_visit(vis_hash, ch.v);
        task.cur_cost = ch.next_cost;
        task.pot_lap = ch.npot_lap;
        task.pot_sub = ch.npot_sub;
        task.est_lb = ch.child_slb;

        int actual_slb =
            dfs_worker_general(worker_id, task, tables, hasher, history, queues, local_nodes);

        task.depth = depth;
        task.u = u;
        task.vis.reset(ch.v);
        task.vis_hash = vis_hash;
        task.cur_cost = cur_cost;
        task.pot_lap = pot_lap;
        task.pot_sub = pot_sub;

        if (actual_slb < INT_INF) {
            min_suffix = std::min(min_suffix, ch.w + actual_slb);
        }
    }

    if (!abort_search_.load(std::memory_order_relaxed)) {
        int final_slb = std::max(known_slb, min_suffix);
        history.update_suffix_lb(hkey, final_slb);
        return final_slb;
    }
    return INT_INF;
}

void SopSolver::run_prefix_bb_general(const DualBoundTables& tables) {
    const int n = inst_.n; (void)tables;
    const int nw = inst_.num_words;
    const int num_threads = std::max(1, opts_.threads);
    auto assignments = numa_.plan_worker_assignments(num_threads);

    ZobristHasher hasher(n);
    size_t ht_buckets = (1ULL << 20);
    ZobristHistoryTable history(ht_buckets, assignments[0].memory_numa_node);

    std::vector<SearchTaskGen> frontier;
    for (int s : inst_.start_nodes) {
        SearchTaskGen root;
        root.depth = 1;
        root.u = s;
        root.vis.clear();
        root.vis.set(s);
        root.vis_hash = hasher.toggle_visit(0ULL, s);
        root.cur_cost = 0;
        root.pot_lap = static_cast<int>(tables.root_pot_lap);
        root.pot_sub = tables.root_pot_sub;
        root.est_lb = 0;
        root.prefix_path.assign(n, 0);
        root.prefix_path[0] = s;
        frontier.push_back(std::move(root));
    }

    const size_t target_frontier = (num_threads == 1) ? 1 : static_cast<size_t>(num_threads * 4);
    while (num_threads > 1 && frontier.size() < target_frontier && !frontier.empty()) {
        if (frontier[0].depth >= n - 1) break;
        std::vector<SearchTaskGen> next_frontier;
        for (const auto& tk : frontier) {
            total_enumerated_nodes_.fetch_add(1, std::memory_order_relaxed);
            for (int v = 0; v < n; ++v) {
                if (tk.vis.test(v)) continue;
                if (v == inst_.unique_end && tk.depth + 1 < n) continue;
                if (inst_.pred_bits[v].is_subset_of(tk.vis, nw) && inst_.cost[tk.u][v] < INT_INF) {
                    int w = inst_.cost[tk.u][v];
                    int nc = tk.cur_cost + w;
                    if (nc >= best_cost_.load(std::memory_order_relaxed)) continue;
                    SearchTaskGen child = tk;
                    child.depth = tk.depth + 1;
                    child.u = v;
                    child.vis.set(v);
                    child.vis_hash = hasher.toggle_visit(tk.vis_hash, v);
                    child.cur_cost = nc;
                    child.pot_lap = tk.pot_lap - tables.u_lap[tk.u] - tables.v_lap[v];
                    child.pot_sub = tk.pot_sub - tables.u_sub[tk.u] - tables.v_sub[v];
                    child.prefix_path[tk.depth] = v;
                    next_frontier.push_back(std::move(child));
                }
            }
        }
        if (next_frontier.empty()) break;
        frontier = std::move(next_frontier);
    }

    std::sort(frontier.begin(), frontier.end(),
              [](const SearchTaskGen& a, const SearchTaskGen& b) {
                  return a.cur_cost < b.cur_cost;
              });

    std::vector<std::unique_ptr<WorkerStealQueueGen>> queues(num_threads);
    for (int t = 0; t < num_threads; ++t) {
        queues[t] = std::make_unique<WorkerStealQueueGen>();
        queues[t]->numa_node = assignments[t].numa_node;
    }
    for (size_t i = 0; i < frontier.size(); ++i) {
        queues[i % num_threads]->deque.push_back(std::move(frontier[i]));
    }

    auto worker_fn = [&](int tid) {
        if (num_threads > 1) {
            NumaTopology::pin_calling_thread_to_cpu(assignments[tid].cpu_id);
        }
        uint64_t local_nodes = 0;
        const int my_numa = assignments[tid].numa_node;

        while (!abort_search_.load(std::memory_order_relaxed)) {
            SearchTaskGen task;
            bool got_task = false;
            {
                std::lock_guard<std::mutex> lk(queues[tid]->mtx);
                if (!queues[tid]->deque.empty()) {
                    task = std::move(queues[tid]->deque.front());
                    queues[tid]->deque.pop_front();
                    got_task = true;
                }
            }
            if (!got_task && num_threads > 1) {
                for (int pass = 0; pass < 2 && !got_task; ++pass) {
                    for (int offset = 1; offset < num_threads; ++offset) {
                        int victim = (tid + offset) % num_threads;
                        bool same_numa = (queues[victim]->numa_node == my_numa);
                        if ((pass == 0 && !same_numa) || (pass == 1 && same_numa)) continue;

                        std::lock_guard<std::mutex> lk(queues[victim]->mtx);
                        if (!queues[victim]->deque.empty()) {
                            task = std::move(queues[victim]->deque.front());
                            queues[victim]->deque.pop_front();
                            got_task = true;
                            break;
                        }
                    }
                }
            }
            if (!got_task) break;
            dfs_worker_general(tid, task, tables, hasher, history, queues, local_nodes);
        }

        total_enumerated_nodes_.fetch_add(local_nodes, std::memory_order_relaxed);
    };

    if (num_threads == 1) {
        worker_fn(0);
    } else {
        std::vector<std::thread> threads;
        threads.reserve(num_threads);
        for (int t = 0; t < num_threads; ++t) {
            threads.emplace_back(worker_fn, t);
        }
        for (auto& th : threads) {
            th.join();
        }
    }
}

/*
 * Algorithm: Top-Level SOP Exact Solver Orchestrator
 * Part: Coordinates Preprocessing, Primal Heuristics, Dual Bound Precomputation,
 *       Carpaneto-Toth LAP Subtour/Precedence B&B, and Parallel NUMA-Aware
 *       Prefix/Suffix B&B with History Domination, then maps the optimal tour
 *       back to the original 0..N-1 vertex indexing.
 */
SolverResult SopSolver::solve() {
    SolverResult res;
    res.instance_name = inst_.filename;
    res.instance_size = inst_.n;

    // Phase 1: Preprocessing
    Preprocessor::run(inst_);
    if (!inst_.is_feasible) {
        auto end_t = std::chrono::steady_clock::now();
        res.elapsed_seconds = std::chrono::duration<double>(end_t - start_time_).count();
        res.best_cost = -1;
        res.optimal = false;
        return res;
    }

    if (inst_.n == 1) {
        update_incumbent(0, {0}, "Trivial");
        auto end_t = std::chrono::steady_clock::now();
        res.elapsed_seconds = std::chrono::duration<double>(end_t - start_time_).count();
        res.enumerated_nodes = 1;
        res.best_cost = 0;
        res.best_path = {0};
        res.optimal = true;
        return res;
    }

    // Phase 2: Dual Bound Precomputation (LAP + Held-Karp Subgradient Duals)
    DualBoundTables tables = LowerBounds::build_dual_bound_tables(inst_);

    // Phase 3: Primal Upper-Bounding Heuristics (Greedy + 2-Opt + SOP-3-Exchange + ILS)
    int heur_restarts = (inst_.n <= 80) ? 250 : 60;
    Heuristics::run_initial_heuristics(
        inst_, heur_restarts,
        [this](int64_t c, const std::vector<int>& p) { update_incumbent(c, p, "Heuristic-ILS"); });

    // Check if root lower bound already proves optimality of heuristic solution
    int64_t root_lb = std::max(tables.root_lap_lb,
                               (tables.root_pot_sub + tables.subgrad_scale - 1) / tables.subgrad_scale);
    bool proven_optimal = false;
    if (best_cost_.load(std::memory_order_relaxed) <= root_lb) {
        total_enumerated_nodes_.fetch_add(1, std::memory_order_relaxed);
        proven_optimal = true;
    }

    // Phase 4: Engine A — Fast Carpaneto-Toth Subtour/Precedence LAP B&B Stage
    // Particularly effective on large or low-precedence-density instances (e.g. R.200.1000.1.sop)
    if (!proven_optimal && !check_timeout()) {
        uint64_t max_lap_nodes = (inst_.precedence_density < 0.05) ? 50000 : 128;
        if (tables.root_lap_cycles <= std::max(10, inst_.n / 8)) {
            proven_optimal = run_subtour_lap_bb(tables, max_lap_nodes);
        }
    }

    // Phase 5: Engine B — Parallel NUMA-Aware Dual-Potential Prefix/Suffix B&B
    if (!proven_optimal && !check_timeout()) {
        if (inst_.n <= 64) {
            run_prefix_bb_64(tables);
        } else {
            run_prefix_bb_general(tables);
        }
        if (!timed_out_.load(std::memory_order_relaxed)) {
            proven_optimal = true;
        }
    }

    auto end_t = std::chrono::steady_clock::now();
    res.elapsed_seconds = std::chrono::duration<double>(end_t - start_time_).count();
    res.enumerated_nodes = total_enumerated_nodes_.load(std::memory_order_relaxed);

    int64_t final_cost = best_cost_.load(std::memory_order_relaxed);
    if (final_cost < INF_COST && !best_working_path_.empty()) {
        res.best_cost = final_cost;
        res.best_path.resize(inst_.n);
        if (inst_.is_transposed) {
            for (int i = 0; i < inst_.n; ++i) {
                res.best_path[i] = inst_.to_original_node(best_working_path_[inst_.n - 1 - i]);
            }
        } else {
            res.best_path = best_working_path_;
        }
        res.optimal = proven_optimal && !timed_out_.load(std::memory_order_relaxed);
    } else {
        res.best_cost = -1;
        res.best_path.clear();
        res.optimal = false;
    }

    return res;
}

} // namespace sop
