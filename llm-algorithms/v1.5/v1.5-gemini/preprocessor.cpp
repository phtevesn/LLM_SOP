/*
 * File: preprocessor.cpp
 * Purpose: Implements the SOP preprocessing phase:
 *          1. Extracting precedence constraints (T[i][j] < 0 => j precedes i).
 *          2. Dynamically deriving candidate start and end nodes from the
 *             precedence graph (without hardcoding 0 or N-1) and injecting
 *             implied precedences when the start or end node is unique.
 *          3. Computing the bit-parallel Warshall transitive closure and
 *             checking for precedence cycles.
 *          4. Applying Escudero's precedence-based arc elimination rules
 *             (reverse precedences, transitive bypasses, and position windows).
 *          5. Probing forward vs. backward order-ideal expansion and
 *             transposing the instance when backward search has a narrower
 *             poset ideal width (Salii 2019).
 */

#include "preprocessor.hpp"

#include <algorithm>
#include <vector>

namespace sop {

/*
 * Algorithm: Precedence Digraph Extraction
 * Part: Scans raw matrix T; whenever T[i][j] < 0, records that node j must
 *       precede node i (prec[j][i] = 1) and forbids the direct arc i -> j.
 */
void Preprocessor::build_initial_precedences(SopInstance& inst) {
    const int n = inst.n;
    inst.num_words = (n + 63) / 64;
    inst.cost.assign(n, std::vector<int>(n, INT_INF));
    inst.prec.assign(n, std::vector<uint8_t>(n, 0));
    inst.pred_bits.assign(n, BitSet1024());
    inst.succ_bits.assign(n, BitSet1024());

    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            int64_t val = inst.raw_matrix[i][j];
            if (i == j) {
                inst.cost[i][j] = INT_INF;
            } else if (val < 0) {
                inst.prec[j][i] = 1;
                inst.pred_bits[i].set(j);
                inst.succ_bits[j].set(i);
                inst.cost[i][j] = INT_INF;
            } else {
                inst.cost[i][j] = (val >= INT_INF) ? INT_INF : static_cast<int>(val);
            }
        }
    }
}

/*
 * Algorithm: Dynamic Start and End Node Derivation & Implied Precedence Injection
 * Part: Derives valid start nodes (zero predecessors) and valid terminal nodes
 *       (zero successors). If a unique start node s* exists, injects s* < v for
 *       all v != s*; if a unique terminal node t* exists, injects v < t* for
 *       all v != t*.
 */
void Preprocessor::derive_start_and_end_nodes(SopInstance& inst) {
    const int n = inst.n;
    inst.start_nodes.clear();
    inst.end_nodes.clear();

    for (int u = 0; u < n; ++u) {
        int in_deg = inst.pred_bits[u].count(inst.num_words);
        int out_deg = inst.succ_bits[u].count(inst.num_words);
        if (in_deg == 0) inst.start_nodes.push_back(u);
        if (out_deg == 0) inst.end_nodes.push_back(u);
    }

    if (inst.start_nodes.empty() || inst.end_nodes.empty()) {
        inst.is_feasible = false;
        return;
    }

    if (inst.start_nodes.size() == 1) {
        inst.unique_start = inst.start_nodes[0];
        int s = inst.unique_start;
        for (int v = 0; v < n; ++v) {
            if (v != s) {
                inst.prec[s][v] = 1;
                inst.pred_bits[v].set(s);
                inst.succ_bits[s].set(v);
                inst.cost[v][s] = INT_INF;
            }
        }
    } else {
        inst.unique_start = -1;
    }

    if (inst.end_nodes.size() == 1) {
        inst.unique_end = inst.end_nodes[0];
        int t = inst.unique_end;
        for (int u = 0; u < n; ++u) {
            if (u != t) {
                inst.prec[u][t] = 1;
                inst.pred_bits[t].set(u);
                inst.succ_bits[u].set(t);
                inst.cost[t][u] = INT_INF;
            }
        }
    } else {
        inst.unique_end = -1;
    }
}

/*
 * Algorithm: Bit-Parallel Warshall Transitive Closure & DAG Cycle Detection
 * Part: Computes transitive closure of precedence relation using 64-bit word
 *       OR operations in O(N^3 / 64) time and verifies irreflexivity (no cycles).
 */
bool Preprocessor::compute_transitive_closure(SopInstance& inst) {
    const int n = inst.n;
    const int nw = inst.num_words;

    for (int k = 0; k < n; ++k) {
        for (int i = 0; i < n; ++i) {
            if (inst.succ_bits[i].test(k)) {
                inst.succ_bits[i].bitwise_or(inst.succ_bits[k], nw);
            }
        }
    }

    int total_prec = 0;
    for (int i = 0; i < n; ++i) {
        if (inst.succ_bits[i].test(i)) {
            inst.is_feasible = false;
            return false;
        }
        for (int j = 0; j < n; ++j) {
            if (inst.succ_bits[i].test(j)) {
                inst.prec[i][j] = 1;
                inst.pred_bits[j].set(i);
                ++total_prec;
            }
        }
    }

    inst.in_degree.assign(n, 0);
    inst.out_degree.assign(n, 0);
    for (int u = 0; u < n; ++u) {
        inst.in_degree[u] = inst.pred_bits[u].count(nw);
        inst.out_degree[u] = inst.succ_bits[u].count(nw);
    }

    if (n > 1) {
        inst.precedence_density =
            static_cast<double>(total_prec) / static_cast<double>(n * (n - 1));
    }
    return true;
}

/*
 * Algorithm: Cardinality Position-Window Tightening
 * Part: Computes earliest visit index E[u] = |Pred(u)| and latest visit index
 *       L[u] = N - 1 - |Succ(u)| for every node u in any feasible tour.
 */
void Preprocessor::compute_position_windows(SopInstance& inst) {
    const int n = inst.n;
    inst.earliest_pos.resize(n);
    inst.latest_pos.resize(n);
    for (int u = 0; u < n; ++u) {
        inst.earliest_pos[u] = inst.in_degree[u];
        inst.latest_pos[u] = (n - 1) - inst.out_degree[u];
        if (inst.earliest_pos[u] > inst.latest_pos[u]) {
            inst.is_feasible = false;
        }
    }
}

/*
 * Algorithm: Escudero Precedence-Based Arc Elimination (Graph Reduction)
 * Part: Eliminates direct transitions (u, v) that violate precedence constraints:
 *       1. Reverse precedence: v < u implies u cannot precede v.
 *       2. Transitive bypass: if succ_bits[u] intersects pred_bits[v], there
 *          exists w with u < w < v, so w must be visited strictly between u and v.
 *       3. Position-window incompatibility: L[u] + 1 < E[v] or disjointness of
 *          combined before/after sets.
 */
void Preprocessor::eliminate_infeasible_arcs(SopInstance& inst) {
    const int n = inst.n;
    const int nw = inst.num_words;
    int eliminated = 0;

    for (int u = 0; u < n; ++u) {
        for (int v = 0; v < n; ++v) {
            if (u == v) {
                inst.cost[u][v] = INT_INF;
                continue;
            }
            if (inst.cost[u][v] >= INT_INF) continue;

            bool forbid = false;
            if (inst.prec[v][u]) {
                forbid = true;
            } else if (inst.succ_bits[u].intersects(inst.pred_bits[v], nw)) {
                forbid = true;
            } else if (inst.latest_pos[u] + 1 < inst.earliest_pos[v]) {
                forbid = true;
            } else {
                int union_pred_succ = 0;
                for (int w = 0; w < nw; ++w) {
                    uint64_t must_before_u = inst.pred_bits[u].words[w] | inst.pred_bits[v].words[w];
                    uint64_t must_after_v = inst.succ_bits[u].words[w] | inst.succ_bits[v].words[w];
                    if (w == (u >> 6)) must_before_u &= ~(1ULL << (u & 63));
                    if (w == (v >> 6)) must_after_v &= ~(1ULL << (v & 63));
                    if ((must_before_u & must_after_v) != 0ULL) {
                        forbid = true;
                        break;
                    }
                    union_pred_succ += __builtin_popcountll(must_before_u | must_after_v);
                }
                if (union_pred_succ > n - 2) {
                    forbid = true;
                }
            }

            if (forbid) {
                inst.cost[u][v] = INT_INF;
                ++eliminated;
            }
        }
    }
    inst.eliminated_arcs = eliminated;
}

/*
 * Algorithm: Forward vs. Backward Order-Ideal Poset Width Probe (Salii 2019)
 * Part: Expands 6 layers of precedence-feasible order ideals from the start nodes
 *       (forward) and from the end nodes (backward). If backward ideals grow
 *       significantly slower than forward ideals, selects the transposed orientation.
 */
bool Preprocessor::should_transpose_poset(const SopInstance& inst) {
    if (inst.unique_start == -1 || inst.unique_end == -1 || inst.n > 64) {
        if (inst.unique_start != -1 && inst.unique_end != -1) {
            int fwd_ready = 0, bwd_ready = 0;
            for (int u = 0; u < inst.n; ++u) {
                if (u != inst.unique_start && inst.in_degree[u] == 1) ++fwd_ready;
                if (u != inst.unique_end && inst.out_degree[u] == 1) ++bwd_ready;
            }
            return (bwd_ready > 0 && bwd_ready * 2 < fwd_ready);
        }
        return false;
    }

    const int n = inst.n;
    std::vector<uint64_t> fwd_pred(n, 0), bwd_succ(n, 0);
    for (int i = 0; i < n; ++i) {
        fwd_pred[i] = inst.pred_bits[i].words[0];
        bwd_succ[i] = inst.succ_bits[i].words[0];
    }

    std::vector<uint64_t> fwd = {1ULL << inst.unique_start};
    std::vector<uint64_t> bwd = {1ULL << inst.unique_end};
    const uint64_t all_mask = (n == 64) ? ~0ULL : ((1ULL << n) - 1ULL);
    const int probe_steps = std::min(6, n - 1);

    for (int step = 1; step <= probe_steps; ++step) {
        std::vector<uint64_t> next_fwd, next_bwd;
        for (uint64_t vis : fwd) {
            uint64_t unvis = all_mask ^ vis;
            while (unvis) {
                int v = __builtin_ctzll(unvis);
                unvis &= unvis - 1;
                if ((vis & fwd_pred[v]) == fwd_pred[v]) {
                    next_fwd.push_back(vis | (1ULL << v));
                }
            }
            if (next_fwd.size() > 50000) break;
        }
        std::sort(next_fwd.begin(), next_fwd.end());
        next_fwd.erase(std::unique(next_fwd.begin(), next_fwd.end()), next_fwd.end());

        for (uint64_t vis : bwd) {
            uint64_t unvis = all_mask ^ vis;
            while (unvis) {
                int v = __builtin_ctzll(unvis);
                unvis &= unvis - 1;
                if ((vis & bwd_succ[v]) == bwd_succ[v]) {
                    next_bwd.push_back(vis | (1ULL << v));
                }
            }
            if (next_bwd.size() > 50000) break;
        }
        std::sort(next_bwd.begin(), next_bwd.end());
        next_bwd.erase(std::unique(next_bwd.begin(), next_bwd.end()), next_bwd.end());

        fwd = std::move(next_fwd);
        bwd = std::move(next_bwd);
        if (fwd.size() > 40000 || bwd.size() > 40000) break;
    }

    return (!bwd.empty() && bwd.size() * 2 < fwd.size());
}

/*
 * Algorithm: Dual Poset & Cost Matrix Transposition
 * Part: Reverses vertex indices (u' = N - 1 - u), transposes cost matrix
 *       (cost'[u'][v'] = cost[v][u]), and reverses all precedence arcs so
 *       backward search is executed identically to forward search.
 */
void Preprocessor::transpose_instance(SopInstance& inst) {
    const int n = inst.n;
    const int nw = inst.num_words;

    std::vector<std::vector<int>> new_cost(n, std::vector<int>(n, INT_INF));
    std::vector<std::vector<uint8_t>> new_prec(n, std::vector<uint8_t>(n, 0));
    std::vector<BitSet1024> new_pred(n), new_succ(n);

    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            int ri = n - 1 - i;
            int rj = n - 1 - j;
            new_cost[ri][rj] = inst.cost[j][i];
            if (inst.prec[j][i]) {
                new_prec[ri][rj] = 1;
                new_pred[rj].set(ri);
                new_succ[ri].set(rj);
            }
        }
    }

    inst.cost = std::move(new_cost);
    inst.prec = std::move(new_prec);
    inst.pred_bits = std::move(new_pred);
    inst.succ_bits = std::move(new_succ);

    for (int u = 0; u < n; ++u) {
        inst.in_degree[u] = inst.pred_bits[u].count(nw);
        inst.out_degree[u] = inst.succ_bits[u].count(nw);
    }

    std::vector<int> new_starts, new_ends;
    for (int u : inst.end_nodes) new_starts.push_back(n - 1 - u);
    for (int u : inst.start_nodes) new_ends.push_back(n - 1 - u);
    std::sort(new_starts.begin(), new_starts.end());
    std::sort(new_ends.begin(), new_ends.end());
    inst.start_nodes = std::move(new_starts);
    inst.end_nodes = std::move(new_ends);
    inst.unique_start = (inst.start_nodes.size() == 1) ? inst.start_nodes[0] : -1;
    inst.unique_end = (inst.end_nodes.size() == 1) ? inst.end_nodes[0] : -1;
    inst.is_transposed = !inst.is_transposed;

    compute_position_windows(inst);
}

void Preprocessor::run(SopInstance& inst) {
    if (inst.n <= 0) {
        inst.is_feasible = false;
        return;
    }
    if (inst.n == 1) {
        inst.num_words = 1;
        inst.cost.assign(1, std::vector<int>(1, 0));
        inst.prec.assign(1, std::vector<uint8_t>(1, 0));
        inst.pred_bits.assign(1, BitSet1024());
        inst.succ_bits.assign(1, BitSet1024());
        inst.in_degree.assign(1, 0);
        inst.out_degree.assign(1, 0);
        inst.earliest_pos.assign(1, 0);
        inst.latest_pos.assign(1, 0);
        inst.start_nodes = {0};
        inst.end_nodes = {0};
        inst.unique_start = 0;
        inst.unique_end = 0;
        inst.is_feasible = true;
        return;
    }

    build_initial_precedences(inst);
    derive_start_and_end_nodes(inst);
    if (!inst.is_feasible) return;

    if (!compute_transitive_closure(inst)) return;
    compute_position_windows(inst);
    if (!inst.is_feasible) return;

    eliminate_infeasible_arcs(inst);

    if (should_transpose_poset(inst)) {
        transpose_instance(inst);
    }
}

} // namespace sop
