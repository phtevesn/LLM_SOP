/*
 * File: heuristics.hpp
 * Purpose: Declares upper-bounding primal heuristics for the Sequential Ordering
 *          Problem: Multi-Start Randomized Topological Greedy Construction,
 *          Precedence-Feasible 2-Opt Subpath Reversal, Gambardella & Dorigo's
 *          SOP-3-Exchange Local Search, and Iterated Local Search (ILS) Kicks.
 */

#ifndef SOP_HEURISTICS_HPP
#define SOP_HEURISTICS_HPP

#include "types.hpp"
#include <functional>
#include <utility>
#include <vector>

namespace sop {

class Heuristics {
public:
    using IncumbentCallback = std::function<void(int64_t, const std::vector<int>&)>;

    // Runs Multi-Start Randomized Topological Greedy + 2-Opt + SOP-3-Exchange + ILS
    // to compute a tight initial upper bound before Branch-and-Bound begins.
    static std::pair<int64_t, std::vector<int>> run_initial_heuristics(
        const SopInstance& inst,
        int num_restarts,
        const IncumbentCallback& on_new_best);

    // Improves a feasible tour in-place using Precedence-Feasible 2-Opt and SOP-3-Exchange.
    static int64_t local_search_refine(const SopInstance& inst,
                                       std::vector<int>& path,
                                       int64_t current_cost);
};

} // namespace sop

#endif // SOP_HEURISTICS_HPP
