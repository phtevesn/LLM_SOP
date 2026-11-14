/*
 * File: bounds.hpp
 * Purpose: Declares exact lower-bounding relaxations and dual potential
 *          optimizers for the Sequential Ordering Problem:
 *          1. Jonker-Volgenant / Hungarian Shortest Augmenting Path Linear
 *             Assignment Problem (LAP) solver (full O(N^3) and incremental
 *             single-row O(N^2) augmentation).
 *          2. Held-Karp Style Subgradient Ascent on 2-Cycle-Free Directed
 *             Dual Potentials (U_sub, V_sub).
 *          3. Static sorted adjacency structures for O(1) parent-cached
 *             2-cycle-free 2-edge bounding.
 */

#ifndef SOP_BOUNDS_HPP
#define SOP_BOUNDS_HPP

#include "types.hpp"
#include <cstdint>
#include <utility>
#include <vector>

namespace sop {

struct LapSolution {
    int n = 0;
    int64_t optimal_cost = 0;
    std::vector<int> row_to_col; // row_to_col[i] = assigned column j
    std::vector<int> col_to_row; // col_to_row[j] = assigned row i
    std::vector<int> u_dual;     // row dual potentials
    std::vector<int> v_dual;     // col dual potentials
    bool feasible = true;
};

struct DualBoundTables {
    int n = 0;
    int subgrad_scale = 64;

    // LAP dual potentials (U_lap, V_lap) and Subgradient dual potentials (U_sub, V_sub)
    std::vector<int> u_lap;
    std::vector<int> v_lap;
    std::vector<int> u_sub;
    std::vector<int> v_sub;

    int64_t root_pot_lap = 0;
    int64_t root_pot_sub = 0;
    int64_t root_lap_lb = 0;
    int root_lap_cycles = 0;

    // Sorted incoming and outgoing edge lists in LAP-reduced and Subgradient-reduced costs
    std::vector<std::vector<WeightedEdge>> out_lap;
    std::vector<std::vector<WeightedEdge>> in_lap;
    std::vector<std::vector<WeightedEdge>> out_sub;
    std::vector<std::vector<WeightedEdge>> in_sub;

    // Static minimum outgoing/incoming raw edge weights per vertex
    std::vector<int> min_out_raw;
    std::vector<int> min_in_raw;
};

class LowerBounds {
public:
    // Solves the full N x N Linear Assignment Problem via Shortest Augmenting Paths.
    static LapSolution solve_lap_full(int n, const std::vector<std::vector<int>>& cost_matrix);

    // Incrementally re-optimizes an existing LapSolution in O(N^2) time when a single
    // arc (forbid_u, forbid_v) is forbidden (Carpaneto, Dell'Amico, & Toth 1995).
    static LapSolution solve_lap_incremental(int n,
                                             const std::vector<std::vector<int>>& cost_matrix,
                                             const LapSolution& parent_sol,
                                             int freed_row,
                                             int freed_col);

    // Precomputes both LAP dual potentials and Held-Karp subgradient-tightened
    // 2-cycle-free dual potentials, plus sorted reduced-cost adjacency lists.
    static DualBoundTables build_dual_bound_tables(const SopInstance& inst);

private:
    static std::pair<std::vector<int>, std::vector<int>> optimize_subgradient_duals(
        const SopInstance& inst,
        int scale);
};

} // namespace sop

#endif // SOP_BOUNDS_HPP
