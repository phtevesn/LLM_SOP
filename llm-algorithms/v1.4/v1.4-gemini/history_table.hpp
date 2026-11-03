#ifndef SOP_HISTORY_TABLE_HPP
#define SOP_HISTORY_TABLE_HPP

#include "bitset.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace sop {

struct PrefixDominanceEntry {
    std::atomic<uint64_t> key_hash{0};
    int last_node = -1;
    int64_t best_prefix_cost = INF;
    Bitset1024 visited;
};

/**
 * @brief Thread-safe 4,096-shard Prefix Equivalence Dominance Hash Table.
 *        Prunes any Branch-and-Bound state (visited_set, last_node) whose
 *        prefix cost is >= a previously explored prefix on the exact same vertex subset.
 */
class ShardedHistoryTable {
public:
    explicit ShardedHistoryTable(size_t pow2_size = (1 << 20))
        : mask_(pow2_size - 1), table_(pow2_size), locks_(4096) {}

    bool check_and_update(uint64_t h, int last_node, const Bitset1024& vis, int64_t prefix_cost, int nwords) {
        uint64_t combined = h ^ (static_cast<uint64_t>(last_node + 1) * 0x9e3779b97f4a7c15ULL);
        size_t idx = static_cast<size_t>(combined) & mask_;
        size_t lock_idx = (idx >> 2) & 4095;
        std::lock_guard<std::mutex> lk(locks_[lock_idx]);

        for (size_t probe = 0; probe < 4; ++probe) {
            size_t slot = (idx + probe) & mask_;
            auto& e = table_[slot];
            if (e.last_node == -1) {
                e.key_hash.store(combined, std::memory_order_relaxed);
                e.last_node = last_node;
                e.best_prefix_cost = prefix_cost;
                e.visited = vis;
                return true;
            }
            if (e.key_hash.load(std::memory_order_relaxed) == combined &&
                e.last_node == last_node &&
                e.visited.equals(vis, nwords)) {
                if (e.best_prefix_cost <= prefix_cost) return false;
                e.best_prefix_cost = prefix_cost;
                return true;
            }
        }
        auto& e = table_[idx];
        e.key_hash.store(combined, std::memory_order_relaxed);
        e.last_node = last_node;
        e.best_prefix_cost = prefix_cost;
        e.visited = vis;
        return true;
    }

private:
    size_t mask_;
    std::vector<PrefixDominanceEntry> table_;
    std::vector<std::mutex> locks_;
};

} // namespace sop

#endif // SOP_HISTORY_TABLE_HPP