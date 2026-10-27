#ifndef SOP_SOLVER_HPP
#define SOP_SOLVER_HPP

#include "bitset.hpp"
#include "numa_topology.hpp"
#include "shared_state.hpp"
#include "sop_instance.hpp"

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sop {

/**
 * @brief Exact SOP Solver orchestrating:
 *        1. Multi-threaded Portfolio Heuristic + LNS upper-bound initialization
 *        2. Adaptive Parallel Bidirectional Meet-in-the-Middle Bounded Dynamic Programming
 *           with Mask-Factored Held-Karp MST + AP Reduced-Cost + Degree Bounds
 *        3. Parallel Work-Stealing Branch-and-Bound with Sharded Prefix Dominance Table
 *           and concurrent LNS intensification for large N
 */
class ExactSOPSolver {
public:
    struct SolveResult {
        int64_t best_cost = -1;
        std::vector<int> best_path;
        uint64_t enumerated_nodes = 0;
        bool optimal = false;
    };

    ExactSOPSolver(const Instance& inst, SharedSolverState& shared, const NumaTopology& numa, int num_threads);

    SolveResult solve();

private:
    const Instance& inst_;
    SharedSolverState& shared_;
    const NumaTopology& numa_;
    int num_threads_;

    using LayerMap = std::unordered_map<Bitset1024, std::vector<std::pair<int, int64_t>>, BitsetHasher, BitsetEqual>;

    struct BnBTask {
        std::vector<int> prefix;
        Bitset1024 vis;
        uint64_t hash = 0;
        int64_t prefix_cost = 0;
        int64_t rem_min_in = 0;
        int64_t rem_min_out = 0;
    };

    bool reconstruct_subpath_dfs(int u, int target_end, const Bitset1024& allowed_mask, int target_len,
                                 Bitset1024& vis, int64_t cur_cost, int64_t target_cost, std::vector<int>& path,
                                 std::unordered_map<uint64_t, int64_t>& memo);

    bool run_parallel_bidirectional_mitm(std::vector<uint64_t>& thread_counts);
    bool run_parallel_bnb_and_lns(std::vector<uint64_t>& thread_counts);
};

} // namespace sop

#endif // SOP_SOLVER_HPP