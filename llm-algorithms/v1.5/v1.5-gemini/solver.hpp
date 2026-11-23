/*
 * File: solver.hpp
 * Purpose: Declares the parallel NUMA-aware SOP Branch-and-Bound solver
 *          combining Carpaneto-Toth Incremental LAP Subtour/Precedence
 *          Branch-and-Bound with Dual-Potential (LAP + Held-Karp Subgradient)
 *          2-Cycle-Free Prefix/Suffix Branch-and-Bound, Zobrist History
 *          Domination, and Hierarchical NUMA Work Stealing.
 */

#ifndef SOP_SOLVER_HPP
#define SOP_SOLVER_HPP

#include "bounds.hpp"
#include "history_table.hpp"
#include "numa_topology.hpp"
#include "types.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <vector>

namespace sop {

class SopSolver {
public:
    SopSolver(SopInstance& inst, const SolverOptions& opts);

    // Executes the complete SOP optimization pipeline and returns SolverResult.
    SolverResult solve();

private:
    struct SearchTask64 {
        int depth = 1;
        int u = 0;
        uint64_t vis = 0;
        int cur_cost = 0;
        int pot_lap = 0;
        int64_t pot_sub = 0;
        int est_lb = 0;
        std::vector<int> prefix_path;
    };

    struct SearchTaskGen {
        int depth = 1;
        int u = 0;
        BitSet1024 vis;
        uint64_t vis_hash = 0;
        int cur_cost = 0;
        int pot_lap = 0;
        int64_t pot_sub = 0;
        int est_lb = 0;
        std::vector<int> prefix_path;
    };

    struct alignas(64) WorkerStealQueue64 {
        std::mutex mtx;
        std::deque<SearchTask64> deque;
        int numa_node = 0;
    };

    struct alignas(64) WorkerStealQueueGen {
        std::mutex mtx;
        std::deque<SearchTaskGen> deque;
        int numa_node = 0;
    };

    bool check_timeout() const noexcept;
    void update_incumbent(int64_t new_cost, const std::vector<int>& working_path, const char* phase);

    // Engine A: Carpaneto-Toth Subtour & Precedence-Violation Path Elimination B&B
    bool run_subtour_lap_bb(const DualBoundTables& tables, uint64_t max_lap_nodes);

    // Engine B (Fast 64-bit register path for N <= 64):
    // Dual-Potential 2-Cycle-Free Prefix/Suffix B&B with Zobrist History Domination
    void run_prefix_bb_64(const DualBoundTables& tables);
    int dfs_worker_64(int worker_id,
                      SearchTask64& task,
                      const DualBoundTables& tables,
                      ZobristHistoryTable& history,
                      std::vector<std::unique_ptr<WorkerStealQueue64>>& queues,
                      uint64_t& local_nodes);

    // Engine B (General BitSet1024 path for 64 < N <= 1000):
    void run_prefix_bb_general(const DualBoundTables& tables);
    int dfs_worker_general(int worker_id,
                           SearchTaskGen& task,
                           const DualBoundTables& tables,
                           const ZobristHasher& hasher,
                           ZobristHistoryTable& history,
                           std::vector<std::unique_ptr<WorkerStealQueueGen>>& queues,
                           uint64_t& local_nodes);

    SopInstance& inst_;
    SolverOptions opts_;
    NumaTopology numa_;
    std::chrono::steady_clock::time_point start_time_;

    std::atomic<int64_t> best_cost_{INF_COST};
    std::atomic<bool> timed_out_{false};
    std::atomic<bool> abort_search_{false};
    std::atomic<uint64_t> total_enumerated_nodes_{0};

    mutable std::mutex incumbent_mtx_;
    std::vector<int> best_working_path_;
};

} // namespace sop

#endif // SOP_SOLVER_HPP
