#ifndef SOP_SHARED_STATE_HPP
#define SOP_SHARED_STATE_HPP

#include "sop_instance.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <vector>

namespace sop {

/**
 * @brief Thread-safe global incumbent manager and wall-clock timer.
 *        Logs every incumbent improvement immediately to stderr.
 */
struct SharedSolverState {
    const Instance& inst;
    std::chrono::steady_clock::time_point start_time;
    double time_limit_sec = 0.0; // <= 0 means no time limit
    bool has_time_limit = false;

    std::atomic<int64_t> best_cost{INF};
    std::atomic<bool> abort_flag{false};
    std::mutex incumbent_mtx;
    std::vector<int> best_path;

    explicit SharedSolverState(const Instance& ins, double tlim)
        : inst(ins),
          start_time(std::chrono::steady_clock::now()),
          time_limit_sec(tlim),
          has_time_limit(tlim > 0.0) {}

    inline double elapsed_sec() const noexcept {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
    }

    inline bool is_timed_out() const noexcept {
        if (abort_flag.load(std::memory_order_relaxed)) return true;
        if (has_time_limit && elapsed_sec() >= time_limit_sec) {
            return true;
        }
        return false;
    }

    bool update_incumbent(int64_t cand_cost, const std::vector<int>& cand_path) {
        int64_t cur = best_cost.load(std::memory_order_relaxed);
        if (cand_cost >= cur) return false;
        std::lock_guard<std::mutex> lk(incumbent_mtx);
        if (cand_cost < best_cost.load(std::memory_order_relaxed)) {
            best_cost.store(cand_cost, std::memory_order_relaxed);
            best_path = cand_path;
            std::cerr << "[New Best Cost] " << cand_cost
                      << " (elapsed=" << std::fixed << std::setprecision(4) << elapsed_sec() << "s)\n";
            return true;
        }
        return false;
    }
};

} // namespace sop

#endif // SOP_SHARED_STATE_HPP