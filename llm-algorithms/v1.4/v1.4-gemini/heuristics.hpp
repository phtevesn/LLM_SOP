#ifndef SOP_HEURISTICS_HPP
#define SOP_HEURISTICS_HPP

#include "shared_state.hpp"
#include "sop_instance.hpp"

#include <cstdint>
#include <random>
#include <vector>

namespace sop {

/**
 * @brief Parallel Portfolio Upper-Bound Heuristic Engine combining:
 *        1. Randomized GRASP topological construction
 *        2. Single-node and short-block (L=1..4) forward/backward Or-Opt
 *        3. Precedence-constrained SOP-2-Opt segment reversal
 *        4. Don't-Look-Bits SOP-3-Exchange (adjacent block swap of arbitrary lengths)
 *        5. Large Neighborhood Search (LNS) Ruin-and-Recreate intensification
 */
class HeuristicEngine {
public:
    HeuristicEngine(const Instance& inst, SharedSolverState& shared, uint64_t seed);

    std::vector<int> construct_randomized(double alpha);
    void local_search(std::vector<int>& path, int64_t& best_c, uint64_t& local_ops);
    void ruin_and_recreate(std::vector<int>& path, int64_t& c);
    uint64_t run_portfolio(int starts, int lns_iters, double max_heur_sec);

private:
    const Instance& inst_;
    SharedSolverState& shared_;
    std::mt19937_64 rng_;
};

} // namespace sop

#endif // SOP_HEURISTICS_HPP