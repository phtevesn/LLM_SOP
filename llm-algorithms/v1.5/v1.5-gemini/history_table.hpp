/*
 * File: history_table.hpp
 * Purpose: Declares the 64-bit Zobrist State Hasher and the Lock-Free
 *          Cache-Line-Aligned 4-Way Set-Associative History Domination &
 *          Suffix Lower-Bound Transposition Table.
 */

#ifndef SOP_HISTORY_TABLE_HPP
#define SOP_HISTORY_TABLE_HPP

#include "numa_topology.hpp"
#include "types.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sop {

/*
 * Algorithm: 64-Bit Zobrist Incremental Subset & Endpoint Hasher (Zobrist 1970)
 * Part: Maps any SOP state (visited subset S, current endpoint u) in O(1) incremental
 *       XOR operations to a 64-bit state key. Uses exact bit-packing when N <= 57.
 */
class ZobristHasher {
public:
    explicit ZobristHasher(int n = 0);

    inline uint64_t toggle_visit(uint64_t current_vis_hash, int vertex) const noexcept {
        if (exact_packing_) {
            return current_vis_hash ^ (1ULL << vertex);
        }
        return current_vis_hash ^ vis_keys_[vertex];
    }

    inline uint64_t make_state_key(uint64_t vis_hash, int current_u) const noexcept {
        if (exact_packing_) {
            return ((vis_hash << 6) | static_cast<uint64_t>(current_u)) | (1ULL << 63);
        }
        return (vis_hash ^ last_keys_[current_u]) | (1ULL << 63);
    }

private:
    int n_ = 0;
    bool exact_packing_ = false;
    std::vector<uint64_t> vis_keys_;
    std::vector<uint64_t> last_keys_;
};

/*
 * Structure: HistorySlot & HistoryBucket
 * Purpose: Each 16-byte HistorySlot stores an atomic 64-bit state key and an
 *          atomic 64-bit packed payload: (uint32_t min_prefix_cost << 32) | uint32_t suffix_lb.
 *          Four slots form a 64-byte cache-line-aligned bucket.
 */
struct HistorySlot {
    std::atomic<uint64_t> key{0};
    std::atomic<uint64_t> payload{0};
};

struct alignas(64) HistoryBucket {
    HistorySlot slots[4];
};

class ZobristHistoryTable {
public:
    ZobristHistoryTable(size_t num_buckets_pow2, int memory_numa_node = 0);
    ~ZobristHistoryTable();

    ZobristHistoryTable(const ZobristHistoryTable&) = delete;
    ZobristHistoryTable& operator=(const ZobristHistoryTable&) = delete;

    static inline uint64_t pack_payload(int min_prefix, int suffix_lb) noexcept {
        uint32_t mp = static_cast<uint32_t>(std::max(0, std::min(min_prefix, INT_INF)));
        uint32_t slb = static_cast<uint32_t>(std::max(0, std::min(suffix_lb, INT_INF)));
        return (static_cast<uint64_t>(mp) << 32) | static_cast<uint64_t>(slb);
    }

    static inline int unpack_min_prefix(uint64_t payload) noexcept {
        return static_cast<int>(payload >> 32);
    }

    static inline int unpack_suffix_lb(uint64_t payload) noexcept {
        return static_cast<int>(payload & 0xFFFFFFFFULL);
    }

    // Looks up `state_key`; returns true if found and outputs `min_prefix` and `suffix_lb`.
    inline bool probe(uint64_t state_key, int& min_prefix, int& suffix_lb) const noexcept {
        const size_t b_idx = bucket_index(state_key);
        const HistoryBucket& bucket = buckets_[b_idx];
        for (int i = 0; i < 4; ++i) {
            uint64_t k = bucket.slots[i].key.load(std::memory_order_acquire);
            if (k == state_key) {
                uint64_t p = bucket.slots[i].payload.load(std::memory_order_relaxed);
                min_prefix = unpack_min_prefix(p);
                suffix_lb = unpack_suffix_lb(p);
                return true;
            }
            if (k == 0ULL) break;
        }
        return false;
    }

    // Checks if `state_key` is dominated at `cur_cost` given `best_cost`;
    // if not dominated, atomically records `cur_cost` as the new `min_prefix`.
    // Returns true if the state is dominated (can be pruned immediately), and
    // sets `out_suffix_lb` to the known suffix lower bound.
    inline bool check_or_record_prefix(uint64_t state_key,
                                       int cur_cost,
                                       int64_t best_cost,
                                       int& out_suffix_lb) noexcept {
        const size_t b_idx = bucket_index(state_key);
        HistoryBucket& bucket = buckets_[b_idx];
        int replace_idx = 3;
        int min_pref_seen = -1;

        for (int i = 0; i < 4; ++i) {
            uint64_t k = bucket.slots[i].key.load(std::memory_order_acquire);
            if (k == state_key) {
                uint64_t old_p = bucket.slots[i].payload.load(std::memory_order_relaxed);
                while (true) {
                    int old_mp = unpack_min_prefix(old_p);
                    int old_slb = unpack_suffix_lb(old_p);
                    out_suffix_lb = old_slb;
                    if (cur_cost >= old_mp ||
                        static_cast<int64_t>(cur_cost) + old_slb >= best_cost) {
                        return true; // Pruned by prefix dominance or suffix LB!
                    }
                    uint64_t new_p = pack_payload(cur_cost, old_slb);
                    if (bucket.slots[i].payload.compare_exchange_weak(
                            old_p, new_p, std::memory_order_relaxed)) {
                        return false;
                    }
                }
            }
            if (k == 0ULL) {
                uint64_t expected = 0ULL;
                uint64_t init_p = pack_payload(cur_cost, 0);
                if (bucket.slots[i].key.compare_exchange_strong(
                        expected, state_key, std::memory_order_release, std::memory_order_relaxed)) {
                    bucket.slots[i].payload.store(init_p, std::memory_order_relaxed);
                    out_suffix_lb = 0;
                    return false;
                }
                if (expected == state_key) {
                    --i;
                    continue;
                }
            } else {
                uint64_t p = bucket.slots[i].payload.load(std::memory_order_relaxed);
                int mp = unpack_min_prefix(p);
                if (mp > min_pref_seen) {
                    min_pref_seen = mp;
                    replace_idx = i;
                }
            }
        }

        // All 4 ways occupied: overwrite the least useful slot
        bucket.slots[replace_idx].payload.store(pack_payload(cur_cost, 0), std::memory_order_relaxed);
        bucket.slots[replace_idx].key.store(state_key, std::memory_order_release);
        out_suffix_lb = 0;
        return false;
    }

    // Updates the proven suffix lower bound `new_suffix_lb` for `state_key`.
    inline void update_suffix_lb(uint64_t state_key, int new_suffix_lb) noexcept {
        if (new_suffix_lb <= 0) return;
        const size_t b_idx = bucket_index(state_key);
        HistoryBucket& bucket = buckets_[b_idx];
        for (int i = 0; i < 4; ++i) {
            uint64_t k = bucket.slots[i].key.load(std::memory_order_acquire);
            if (k == state_key) {
                uint64_t old_p = bucket.slots[i].payload.load(std::memory_order_relaxed);
                while (true) {
                    int old_mp = unpack_min_prefix(old_p);
                    int old_slb = unpack_suffix_lb(old_p);
                    if (new_suffix_lb <= old_slb) return;
                    uint64_t new_p = pack_payload(old_mp, new_suffix_lb);
                    if (bucket.slots[i].payload.compare_exchange_weak(
                            old_p, new_p, std::memory_order_relaxed)) {
                        return;
                    }
                }
            }
            if (k == 0ULL) return;
        }
    }

private:
    inline size_t bucket_index(uint64_t key) const noexcept {
        uint64_t h = key * 0x9E3779B97F4A7C15ULL;
        h ^= (h >> 32);
        return static_cast<size_t>(h) & mask_;
    }

    size_t num_buckets_ = 0;
    size_t mask_ = 0;
    size_t alloc_bytes_ = 0;
    HistoryBucket* buckets_ = nullptr;
};

} // namespace sop

#endif // SOP_HISTORY_TABLE_HPP
