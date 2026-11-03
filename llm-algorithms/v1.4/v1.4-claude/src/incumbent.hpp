// incumbent.hpp - thread-safe store for the best feasible solution found so far.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace sop {

class Incumbent {
public:
    // Current best cost (INT64_MAX if none). Cheap, lock-free.
    int64_t cost() const { return best_.load(std::memory_order_relaxed); }
    // Offer a complete internal path (start..end). Returns true if it improved the best.
    // Logs every improvement to stderr.
    bool offer(int64_t cost, const std::vector<int>& path, const char* source);
    // Copy of the best path (empty if none).
    std::vector<int> path() const;
    bool hasSolution() const { return cost() != INT64_MAX; }

private:
    std::atomic<int64_t> best_{INT64_MAX};
    mutable std::mutex m_;
    std::vector<int> path_;
};

}  // namespace sop
