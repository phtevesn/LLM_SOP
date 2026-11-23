/*
 * File: bounds.cpp
 * Purpose: Implements lower-bounding relaxations for the SOP:
 *          1. Full O(N^3) and Incremental O(N^2) Shortest Augmenting Path
 *             Linear Assignment Problem (LAP) solvers (Jonker & Volgenant 1987;
 *             Carpaneto, Dell'Amico, & Toth 1995).
 *          2. Held-Karp (1962) Style Subgradient Optimization on 2-Cycle-Free
 *             Directed Dual Potentials (U_sub, V_sub).
 *          3. Precomputation of sorted reduced-cost adjacency lists for fast
 *             parent-cached 2-edge lower bounding during Branch-and-Bound.
 */

#include "bounds.hpp"

#include <algorithm>
#include <vector>

namespace sop {

/*
 * Algorithm: Jonker-Volgenant / Hungarian Shortest Augmenting Path LAP Solver
 * Part: Solves the N x N Linear Assignment Problem to optimality in O(N^3) time
 *       using Dijkstra shortest augmenting paths with dual potentials (u, v).
 */
LapSolution LowerBounds::solve_lap_full(int n, const std::vector<std::vector<int>>& cost_matrix) {
    LapSolution sol;
    sol.n = n;
    sol.row_to_col.assign(n, -1);
    sol.col_to_row.assign(n, -1);
    sol.u_dual.assign(n, 0);
    sol.v_dual.assign(n, 0);
    sol.optimal_cost = 0;
    sol.feasible = true;

    std::vector<int64_t> u(n + 1, 0), v(n + 1, 0);
    std::vector<int> p(n + 1, 0), way(n + 1, 0);
    std::vector<int64_t> minv(n + 1, INF_COST);
    std::vector<uint8_t> used(n + 1, 0);

    for (int i = 1; i <= n; ++i) {
        p[0] = i;
        int j0 = 0;
        std::fill(minv.begin(), minv.end(), INF_COST);
        std::fill(used.begin(), used.end(), 0);

        do {
            used[j0] = 1;
            int i0 = p[j0];
            int j1 = 0;
            int64_t delta = INF_COST;
            const auto& row_costs = cost_matrix[i0 - 1];

            for (int j = 1; j <= n; ++j) {
                if (!used[j]) {
                    int64_t c = row_costs[j - 1];
                    int64_t cur = c - u[i0] - v[j];
                    if (cur < minv[j]) {
                        minv[j] = cur;
                        way[j] = j0;
                    }
                    if (minv[j] < delta) {
                        delta = minv[j];
                        j1 = j;
                    }
                }
            }

            if (delta >= INT_INF / 2 || j1 == 0) {
                sol.feasible = false;
                sol.optimal_cost = INF_COST;
                return sol;
            }

            for (int j = 0; j <= n; ++j) {
                if (used[j]) {
                    u[p[j]] += delta;
                    v[j] -= delta;
                } else {
                    minv[j] -= delta;
                }
            }
            j0 = j1;
        } while (p[j0] != 0);

        do {
            int j1 = way[j0];
            p[j0] = p[j1];
            j0 = j1;
        } while (j0 != 0);
    }

    int64_t total_cost = 0;
    for (int j = 1; j <= n; ++j) {
        int r = p[j] - 1;
        int c = j - 1;
        if (r < 0 || cost_matrix[r][c] >= INT_INF) {
            sol.feasible = false;
            sol.optimal_cost = INF_COST;
            return sol;
        }
        sol.row_to_col[r] = c;
        sol.col_to_row[c] = r;
        total_cost += cost_matrix[r][c];
    }

    for (int i = 0; i < n; ++i) {
        sol.u_dual[i] = static_cast<int>(u[i + 1]);
        sol.v_dual[i] = static_cast<int>(v[i + 1]);
    }
    sol.optimal_cost = total_cost;
    return sol;
}

/*
 * Algorithm: Incremental Single-Row Shortest Augmenting Path LAP Updater
 * Part: Re-optimizes an N x N assignment in O(N^2) time after breaking a single
 *       matched arc (freed_row -> freed_col) by running one Dijkstra augmenting
 *       path search from `freed_row` while preserving dual feasibility.
 */
LapSolution LowerBounds::solve_lap_incremental(
    int n,
    const std::vector<std::vector<int>>& cost_matrix,
    const LapSolution& parent_sol,
    int freed_row,
    int freed_col) {
    LapSolution sol = parent_sol;
    if (!sol.feasible) return sol;

    sol.row_to_col[freed_row] = -1;
    sol.col_to_row[freed_col] = -1;

    std::vector<int64_t> minv(n, INF_COST);
    std::vector<int> way(n, -1);
    std::vector<uint8_t> used_col(n, 0);

    int cur_r = freed_row;
    int cur_c = -1;

    while (true) {
        int64_t delta = INF_COST;
        int next_c = -1;
        const auto& row_costs = cost_matrix[cur_r];
        int64_t u_r = sol.u_dual[cur_r];

        for (int j = 0; j < n; ++j) {
            if (!used_col[j]) {
                int c_val = row_costs[j];
                if (c_val < INT_INF) {
                    int64_t red = static_cast<int64_t>(c_val) - u_r - sol.v_dual[j];
                    if (red < minv[j]) {
                        minv[j] = red;
                        way[j] = cur_c;
                    }
                }
                if (minv[j] < delta) {
                    delta = minv[j];
                    next_c = j;
                }
            }
        }

        if (delta >= INT_INF / 2 || next_c == -1) {
            sol.feasible = false;
            sol.optimal_cost = INF_COST;
            return sol;
        }

        sol.u_dual[freed_row] += static_cast<int>(delta);
        for (int j = 0; j < n; ++j) {
            if (used_col[j]) {
                int mr = sol.col_to_row[j];
                sol.u_dual[mr] += static_cast<int>(delta);
                sol.v_dual[j] -= static_cast<int>(delta);
            } else {
                minv[j] -= delta;
            }
        }

        cur_c = next_c;
        used_col[cur_c] = 1;
        int mr = sol.col_to_row[cur_c];
        if (mr == -1) break;
        cur_r = mr;
    }

    while (cur_c != -1) {
        int prev_c = way[cur_c];
        int r = (prev_c == -1) ? freed_row : sol.col_to_row[prev_c];
        sol.row_to_col[r] = cur_c;
        sol.col_to_row[cur_c] = r;
        cur_c = prev_c;
    }

    int64_t total_cost = 0;
    for (int i = 0; i < n; ++i) {
        int c = sol.row_to_col[i];
        if (c < 0 || cost_matrix[i][c] >= INT_INF) {
            sol.feasible = false;
            sol.optimal_cost = INF_COST;
            return sol;
        }
        total_cost += cost_matrix[i][c];
    }
    sol.optimal_cost = total_cost;
    return sol;
}

/*
 * Algorithm: Held-Karp Style Subgradient Ascent on 2-Cycle-Free Dual Potentials
 * Part: Iteratively adjusts integer-scaled vertex potentials (U_sub, V_sub) along
 *       the degree-violation subgradient (2 - deg_out[i], 2 - deg_in[j]) of the
 *       2-cycle-free 2-edge relaxation to maximize the root lower bound.
 */
std::pair<std::vector<int>, std::vector<int>> LowerBounds::optimize_subgradient_duals(
    const SopInstance& inst,
    int scale) {
    const int n = inst.n;
    std::vector<int> U(n, 0), V(n, 0);
    std::vector<int> bestU(n, 0), bestV(n, 0);
    if (n <= 2) return {bestU, bestV};

    int s_node = (inst.unique_start != -1) ? inst.unique_start : 0;
    int t_node = (inst.unique_end != -1) ? inst.unique_end : (n - 1);

    int64_t best_total = -INF_COST;
    const int num_iters = (n <= 80) ? 360 : 120;

    for (int iter = 0; iter < num_iters; ++iter) {
        int64_t pot_sum = 0;
        for (int i = 0; i < n; ++i) {
            if (i != t_node) pot_sum += U[i];
            if (i != s_node) pot_sum += V[i];
        }

        std::vector<int> deg_out(n, 0), deg_in(n, 0);
        int64_t pair_sum = 0, out_sum = 0, in_sum = 0;

        // Outgoing from s_node to immediately ready vertices (in_degree == 1)
        int best0_w = INT_INF, best0_to = -1;
        for (int j = 0; j < n; ++j) {
            if (j != s_node && inst.in_degree[j] <= 1 && inst.cost[s_node][j] < INT_INF) {
                int w = inst.cost[s_node][j] * scale - U[s_node] - V[j];
                if (w < best0_w) {
                    best0_w = w;
                    best0_to = j;
                }
            }
        }
        if (best0_w >= INT_INF) break;
        pair_sum += best0_w;
        out_sum += best0_w;
        deg_out[s_node]++;
        if (best0_to != -1) deg_in[best0_to]++;

        // Incoming to t_node
        int bestN_w = INT_INF, bestN_from = -1;
        for (int i = 0; i < n; ++i) {
            if (i != t_node && inst.cost[i][t_node] < INT_INF) {
                int w = inst.cost[i][t_node] * scale - U[i] - V[t_node];
                if (w < bestN_w) {
                    bestN_w = w;
                    bestN_from = i;
                }
            }
        }
        if (bestN_w >= INT_INF) break;
        pair_sum += bestN_w;
        in_sum += bestN_w;
        if (bestN_from != -1) deg_out[bestN_from]++;
        deg_in[t_node]++;

        bool valid_iter = true;
        for (int x = 0; x < n; ++x) {
            if (x == s_node || x == t_node) continue;

            int in1 = INT_INF, in2 = INT_INF, in1_u = -1, in2_u = -1;
            for (int i = 0; i < n; ++i) {
                if (i == t_node) continue;
                if (i == s_node && inst.in_degree[x] > 1) continue;
                if (inst.cost[i][x] < INT_INF) {
                    int w = inst.cost[i][x] * scale - U[i] - V[x];
                    if (w < in1) {
                        in2 = in1;
                        in2_u = in1_u;
                        in1 = w;
                        in1_u = i;
                    } else if (w < in2) {
                        in2 = w;
                        in2_u = i;
                    }
                }
            }

            int out1 = INT_INF, out2 = INT_INF, out1_v = -1, out2_v = -1;
            for (int j = 0; j < n; ++j) {
                if (j == s_node) continue;
                if (inst.cost[x][j] < INT_INF) {
                    int w = inst.cost[x][j] * scale - U[x] - V[j];
                    if (w < out1) {
                        out2 = out1;
                        out2_v = out1_v;
                        out1 = w;
                        out1_v = j;
                    } else if (w < out2) {
                        out2 = w;
                        out2_v = j;
                    }
                }
            }

            if (in1 >= INT_INF || out1 >= INT_INF) {
                valid_iter = false;
                break;
            }

            in_sum += in1;
            out_sum += out1;
            int chosen_in_u = in1_u;
            int chosen_out_v = out1_v;
            int bp = in1 + out1;
            if (in1_u == out1_v) {
                int64_t opt1 = static_cast<int64_t>(in1) + out2;
                int64_t opt2 = static_cast<int64_t>(in2) + out1;
                if (opt1 <= opt2) {
                    bp = static_cast<int>(std::min<int64_t>(opt1, INT_INF));
                    chosen_out_v = out2_v;
                } else {
                    bp = static_cast<int>(std::min<int64_t>(opt2, INT_INF));
                    chosen_in_u = in2_u;
                }
            }
            pair_sum += bp;
            if (chosen_in_u != -1) deg_out[chosen_in_u]++;
            deg_in[x]++;
            deg_out[x]++;
            if (chosen_out_v != -1) deg_in[chosen_out_v]++;
        }

        if (!valid_iter) break;

        int64_t total_scaled = pot_sum + std::max({out_sum, in_sum, (pair_sum + 1) / 2});
        if (total_scaled > best_total) {
            best_total = total_scaled;
            bestU = U;
            bestV = V;
        }

        int step = std::max(1, (scale * 8) / (5 + iter / 8));
        for (int i = 0; i < n; ++i) {
            if (i != t_node) U[i] += step * (2 - deg_out[i]);
            if (i != s_node) V[i] += step * (2 - deg_in[i]);
        }
    }

    return {bestU, bestV};
}

/*
 * Algorithm: Dual-Potential Bound Table Precomputation
 * Part: Computes both LAP dual potentials (U_lap, V_lap) and Held-Karp
 *       subgradient dual potentials (U_sub, V_sub) and builds sorted incoming
 *       and outgoing reduced-cost edge lists for every vertex.
 */
DualBoundTables LowerBounds::build_dual_bound_tables(const SopInstance& inst) {
    DualBoundTables tables;
    const int n = inst.n;
    tables.n = n;
    tables.subgrad_scale = 64;

    int s_node = (inst.unique_start != -1) ? inst.unique_start : 0;
    int t_node = (inst.unique_end != -1) ? inst.unique_end : (n - 1);

    // 1. Solve root LAP relaxation with synthetic return edge (t_node -> s_node) = 0
    auto lap_matrix = inst.cost;
    if (inst.unique_start != -1 && inst.unique_end != -1) {
        lap_matrix[t_node][s_node] = 0;
    } else {
        for (int t : inst.end_nodes) {
            for (int s : inst.start_nodes) {
                if (t != s) lap_matrix[t][s] = 0;
            }
        }
    }

    LapSolution lap_sol = solve_lap_full(n, lap_matrix);
    if (lap_sol.feasible && inst.unique_start != -1 && inst.unique_end != -1) {
        tables.u_lap = lap_sol.u_dual;
        tables.v_lap = lap_sol.v_dual;
        tables.root_lap_lb = lap_sol.optimal_cost;

        // Count cycles in root LAP solution
        std::vector<uint8_t> vis(n, 0);
        int cyc = 0;
        for (int i = 0; i < n; ++i) {
            if (!vis[i]) {
                ++cyc;
                int cur = i;
                while (cur >= 0 && !vis[cur]) {
                    vis[cur] = 1;
                    cur = lap_sol.row_to_col[cur];
                }
            }
        }
        tables.root_lap_cycles = cyc;
    } else {
        tables.u_lap.assign(n, 0);
        tables.v_lap.assign(n, 0);
        tables.root_lap_lb = 0;
        tables.root_lap_cycles = n;
    }

    // 2. Optimize Held-Karp subgradient duals (U_sub, V_sub)
    if (inst.unique_start != -1 && inst.unique_end != -1) {
        auto [us, vs] = optimize_subgradient_duals(inst, tables.subgrad_scale);
        tables.u_sub = std::move(us);
        tables.v_sub = std::move(vs);
    } else {
        tables.u_sub.assign(n, 0);
        tables.v_sub.assign(n, 0);
    }

    tables.root_pot_lap = 0;
    tables.root_pot_sub = 0;
    for (int i = 0; i < n; ++i) {
        if (i != t_node) {
            tables.root_pot_lap += tables.u_lap[i];
            tables.root_pot_sub += tables.u_sub[i];
        }
        if (i != s_node) {
            tables.root_pot_lap += tables.v_lap[i];
            tables.root_pot_sub += tables.v_sub[i];
        }
    }

    // 3. Build sorted incoming and outgoing edge lists in both dual spaces
    tables.out_lap.resize(n);
    tables.in_lap.resize(n);
    tables.out_sub.resize(n);
    tables.in_sub.resize(n);
    tables.min_out_raw.assign(n, 0);
    tables.min_in_raw.assign(n, 0);

    for (int u = 0; u < n; ++u) {
        int min_o = INT_INF;
        for (int v = 0; v < n; ++v) {
            int c = inst.cost[u][v];
            if (c < INT_INF) {
                min_o = std::min(min_o, c);
                int r_lap = c - tables.u_lap[u] - tables.v_lap[v];
                int r_sub = c * tables.subgrad_scale - tables.u_sub[u] - tables.v_sub[v];
                tables.out_lap[u].push_back({r_lap, v});
                tables.in_lap[v].push_back({r_lap, u});
                tables.out_sub[u].push_back({r_sub, v});
                tables.in_sub[v].push_back({r_sub, u});
            }
        }
        tables.min_out_raw[u] = (min_o < INT_INF) ? min_o : 0;
    }

    for (int u = 0; u < n; ++u) {
        std::sort(tables.out_lap[u].begin(), tables.out_lap[u].end());
        std::sort(tables.in_lap[u].begin(), tables.in_lap[u].end());
        std::sort(tables.out_sub[u].begin(), tables.out_sub[u].end());
        std::sort(tables.in_sub[u].begin(), tables.in_sub[u].end());

        int min_i = INT_INF;
        for (int prev = 0; prev < n; ++prev) {
            if (inst.cost[prev][u] < INT_INF) {
                min_i = std::min(min_i, inst.cost[prev][u]);
            }
        }
        tables.min_in_raw[u] = (min_i < INT_INF) ? min_i : 0;
    }

    return tables;
}

} // namespace sop
