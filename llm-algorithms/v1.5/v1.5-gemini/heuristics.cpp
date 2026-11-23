/*
 * File: heuristics.cpp
 * Purpose: Implements primal upper-bounding heuristics for the SOP:
 *          1. Multi-Start Randomized Topological Nearest-Neighbor Construction.
 *          2. Precedence-Constrained 2-Opt Subpath Reversal Local Search.
 *          3. Gambardella & Dorigo's (2000) Path-Preserving SOP-3-Exchange
 *             Local Search with Bit-Parallel Forward/Backward Labeling.
 *          4. Iterated Local Search (ILS) with Precedence-Valid Adjacent Swap
 *             Perturbations.
 */

#include "heuristics.hpp"

#include <algorithm>
#include <random>
#include <vector>

namespace sop {

/*
 * Algorithm: Precedence-Constrained 2-Opt & SOP-3-Exchange Local Search
 * Part: Iteratively applies subpath reversals (when no internal precedence arc
 *       is inverted) and Gambardella & Dorigo's path-preserving 3-exchange
 *       (swapping two adjacent segments [i..m] and [m+1..j] without reversing
 *       either segment when no vertex in [m+1..j] depends on [i..m]).
 */
int64_t Heuristics::local_search_refine(const SopInstance& inst,
                                        std::vector<int>& path,
                                        int64_t current_cost) {
    const int n = inst.n;
    const int nw = inst.num_words;
    if (n <= 3 || static_cast<int>(path.size()) != n) return current_cost;

    bool improved = true;
    int pass = 0;
    const int max_passes = (n <= 100) ? 40 : 12;

    while (improved && pass < max_passes) {
        improved = false;
        ++pass;

        // Part 1: Precedence-Constrained 2-Opt Subpath Reversal
        // Reverses segment path[i..j] if no vertex in path[i..j] depends on an
        // earlier vertex in path[i..j] and all reversed edges are feasible.
        for (int i = 1; i < n - 2 && !improved; ++i) {
            BitSet1024 seg_bits;
            seg_bits.set(path[i]);
            int64_t forward_internal = 0;
            int64_t reverse_internal = 0;
            bool rev_edges_ok = true;

            for (int j = i + 1; j < n - 1; ++j) {
                int vj = path[j];
                // If vj depends on any vertex already in path[i..j-1], neither vj
                // nor any extension to the right can be reversed across path[i].
                if (inst.pred_bits[vj].intersects(seg_bits, nw)) {
                    break;
                }
                seg_bits.set(vj);

                int prev_v = path[j - 1];
                int f_w = inst.cost[prev_v][vj];
                int r_w = inst.cost[vj][prev_v];
                forward_internal += f_w;
                if (r_w >= INT_INF) {
                    rev_edges_ok = false;
                    break;
                }
                reverse_internal += r_w;

                if (rev_edges_ok) {
                    int u_before = path[i - 1];
                    int u_after = path[j + 1];
                    int w_enter = inst.cost[u_before][vj];
                    int w_exit = inst.cost[path[i]][u_after];
                    if (w_enter < INT_INF && w_exit < INT_INF) {
                        int64_t old_sub = inst.cost[u_before][path[i]] +
                                          forward_internal +
                                          inst.cost[vj][u_after];
                        int64_t new_sub = w_enter + reverse_internal + w_exit;
                        if (new_sub < old_sub) {
                            std::reverse(path.begin() + i, path.begin() + j + 1);
                            current_cost += (new_sub - old_sub);
                            improved = true;
                            break;
                        }
                    }
                }
            }
        }

        // Part 2: Gambardella & Dorigo (2000) Path-Preserving SOP-3-Exchange
        // Swaps adjacent subpaths [i..m] and [m+1..j] into [m+1..j] followed by [i..m].
        // Maintains the union of successors of [i..m] in `left_succ` to prune j in O(N/64).
        const int max_block = (n <= 80) ? (n - 2) : 24;
        for (int i = 1; i < n - 2 && !improved; ++i) {
            BitSet1024 left_succ;
            int m_limit = std::min(n - 2, i + max_block);
            for (int m = i; m < m_limit && !improved; ++m) {
                left_succ.bitwise_or(inst.succ_bits[path[m]], nw);
                int u_before = path[i - 1];
                int u_first_left = path[i];
                int u_last_left = path[m];
                int u_first_right = path[m + 1];

                int w_bridge1 = inst.cost[u_before][u_first_right];
                int j_limit = std::min(n - 1, m + 1 + max_block);

                for (int j = m + 1; j < j_limit; ++j) {
                    int vj = path[j];
                    // Feasibility check: if vj is a successor of any node in [i..m],
                    // then [m+1..j] cannot be moved before [i..m] for this or any larger j.
                    if (left_succ.test(vj)) {
                        break;
                    }
                    if (w_bridge1 >= INT_INF) continue;

                    int u_after = path[j + 1];
                    int w_bridge2 = inst.cost[vj][u_first_left];
                    int w_bridge3 = inst.cost[u_last_left][u_after];
                    if (w_bridge2 < INT_INF && w_bridge3 < INT_INF) {
                        int64_t old_edges = static_cast<int64_t>(inst.cost[u_before][u_first_left]) +
                                            inst.cost[u_last_left][u_first_right] +
                                            inst.cost[vj][u_after];
                        int64_t new_edges = static_cast<int64_t>(w_bridge1) + w_bridge2 + w_bridge3;
                        if (new_edges < old_edges) {
                            std::rotate(path.begin() + i, path.begin() + m + 1, path.begin() + j + 1);
                            current_cost += (new_edges - old_edges);
                            improved = true;
                            break;
                        }
                    }
                }
            }
        }
    }

    return current_cost;
}

/*
 * Algorithm: Multi-Start Randomized Topological Greedy + Iterated Local Search (ILS)
 * Part: Constructs diverse precedence-feasible tours using randomized restricted
 *       candidate lists (RCL) over ready vertices, refines each tour with 2-Opt
 *       and SOP-3-Exchange, and applies precedence-valid adjacent swap kicks
 *       to escape local minima.
 */
std::pair<int64_t, std::vector<int>> Heuristics::run_initial_heuristics(
    const SopInstance& inst,
    int num_restarts,
    const IncumbentCallback& on_new_best) {
    const int n = inst.n;
    const int nw = inst.num_words;
    if (!inst.is_feasible || n <= 0) return {-1, {}};
    if (n == 1) {
        std::vector<int> p = {0};
        if (on_new_best) on_new_best(0, p);
        return {0, p};
    }

    int64_t best_cost = INF_COST;
    std::vector<int> best_path;
    std::mt19937 rng(0x5050C0FFEEULL);

    // Track remaining unvisited predecessor counts in O(N) per construction step
    for (int it = 0; it < num_restarts; ++it) {
        std::vector<int> rem_preds = inst.in_degree;
        std::vector<uint8_t> visited(n, 0);
        std::vector<int> ready;
        ready.reserve(n);

        for (int u : inst.start_nodes) {
            ready.push_back(u);
        }
        if (ready.empty()) break;

        std::vector<int> path;
        path.reserve(n);

        // Choose starting node
        int start_idx = (it == 0) ? 0 : static_cast<int>(rng() % ready.size());
        int curr = ready[start_idx];
        ready[start_idx] = ready.back();
        ready.pop_back();
        visited[curr] = 1;
        path.push_back(curr);

        for (int v = 0; v < n; ++v) {
            if (!visited[v] && inst.prec[curr][v]) {
                if (--rem_preds[v] == 0) {
                    ready.push_back(v);
                }
            }
        }

        int64_t tour_cost = 0;
        bool valid_tour = true;

        for (int step = 1; step < n; ++step) {
            if (ready.empty()) {
                valid_tour = false;
                break;
            }

            // Evaluate transition costs from curr to all ready vertices
            struct ReadyCand {
                int cost;
                int idx_in_ready;
            };
            std::vector<ReadyCand> cands;
            cands.reserve(ready.size());
            for (size_t r = 0; r < ready.size(); ++r) {
                int v = ready[r];
                // Do not pick a terminal node before step == n - 1 unless forced
                if (step < n - 1 && inst.out_degree[v] == 0 && ready.size() > 1) {
                    continue;
                }
                int w = inst.cost[curr][v];
                if (w < INT_INF) {
                    cands.push_back({w, static_cast<int>(r)});
                }
            }

            if (cands.empty()) {
                // Fallback: allow any ready node even if high cost
                for (size_t r = 0; r < ready.size(); ++r) {
                    int v = ready[r];
                    if (step < n - 1 && inst.out_degree[v] == 0 && ready.size() > 1) continue;
                    int64_t raw_w = inst.raw_matrix[inst.to_original_node(curr)][inst.to_original_node(v)];
                    if (inst.is_transposed) {
                        raw_w = inst.raw_matrix[inst.to_original_node(v)][inst.to_original_node(curr)];
                    }
                    if (raw_w >= 0) {
                        cands.push_back({static_cast<int>( std::min<int64_t>(raw_w, INT_INF - 1) ),
                                         static_cast<int>(r)});
                    }
                }
            }

            if (cands.empty()) {
                valid_tour = false;
                break;
            }

            std::sort(cands.begin(), cands.end(),
                      [](const ReadyCand& a, const ReadyCand& b) { return a.cost < b.cost; });

            int pick = 0;
            if (it > 0 && cands.size() > 1) {
                int rcl_size = std::min(static_cast<int>(cands.size()), (it % 3 == 0) ? 4 : 2);
                pick = static_cast<int>(rng() % rcl_size);
            }

            int chosen_ready_idx = cands[pick].idx_in_ready;
            int next_v = ready[chosen_ready_idx];
            tour_cost += cands[pick].cost;

            ready[chosen_ready_idx] = ready.back();
            ready.pop_back();
            visited[next_v] = 1;
            path.push_back(next_v);
            curr = next_v;

            for (int v = 0; v < n; ++v) {
                if (!visited[v] && inst.prec[curr][v]) {
                    if (--rem_preds[v] == 0) {
                        ready.push_back(v);
                    }
                }
            }
        }

        if (!valid_tour || static_cast<int>(path.size()) != n) continue;

        tour_cost = local_search_refine(inst, path, tour_cost);
        if (tour_cost < best_cost) {
            best_cost = tour_cost;
            best_path = path;
            if (on_new_best) on_new_best(best_cost, best_path);
        }
    }

    // Iterated Local Search (ILS) phase: perturb best_path with precedence-valid
    // adjacent swaps and re-apply 2-Opt + SOP-3-Exchange
    if (!best_path.empty() && n >= 5) {
        int ils_iters = (n <= 80) ? 150 : 30;
        for (int iter = 0; iter < ils_iters; ++iter) {
            std::vector<int> cand_path = best_path;
            int swaps_done = 0;
            int target_swaps = 2 + (iter % 4);
            for (int tries = 0; tries < 40 && swaps_done < target_swaps; ++tries) {
                int pos = 1 + static_cast<int>(rng() % (n - 3));
                int a = cand_path[pos];
                int b = cand_path[pos + 1];
                if (!inst.prec[a][b] && !inst.prec[b][a]) {
                    std::swap(cand_path[pos], cand_path[pos + 1]);
                    ++swaps_done;
                }
            }
            if (swaps_done == 0) continue;

            // Recompute cost of perturbed path
            int64_t c_cost = 0;
            bool ok = true;
            for (int i = 0; i < n - 1; ++i) {
                int w = inst.cost[cand_path[i]][cand_path[i + 1]];
                if (w >= INT_INF) {
                    ok = false;
                    break;
                }
                c_cost += w;
            }
            if (!ok) continue;

            c_cost = local_search_refine(inst, cand_path, c_cost);
            if (c_cost < best_cost) {
                best_cost = c_cost;
                best_path = std::move(cand_path);
                if (on_new_best) on_new_best(best_cost, best_path);
            }
        }
    }

    (void)nw;
    return {best_cost, best_path};
}

} // namespace sop
