// memo.hpp - lock-free "history" table used for dominance pruning.
//
// A search state is (set of visited nodes, last node). If a state was already reached
// with an equal or smaller path cost, every completion of the new arrival is dominated.
// The table stores 96+ bits of identification per state so a false match (which could
// wrongly prune the optimum) is astronomically unlikely, and uses the "XOR trick" so
// torn concurrent writes are detected and simply read as misses.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sop {

class MemoTable {
public:
    // bytes: requested table size (rounded down to a power of two, >= 4 KiB).
    MemoTable(size_t bytes, const std::vector<int>& memNodes);
    ~MemoTable();
    MemoTable(const MemoTable&) = delete;
    MemoTable& operator=(const MemoTable&) = delete;

    bool enabled() const { return tab_ != nullptr; }
    size_t bytes() const { return nBuckets_ * 4 * sizeof(Entry); }
    static constexpr int64_t kMaxCost = (1LL << 40) - 1;

    // Returns true if the state is dominated (an arrival with cost <= `cost` is on record).
    // Otherwise records / improves the entry and returns false.
    bool checkAndInsert(uint64_t key1, uint64_t key2, int depth, int64_t cost);

private:
    struct Entry {
        std::atomic<uint64_t> a;  // key1 ^ d
        std::atomic<uint64_t> d;  // cost(40) | depth(10)<<40 | tag(14)<<50
    };
    Entry* tab_ = nullptr;
    size_t nBuckets_ = 0;  // buckets of 4 entries (one 64-byte cache line)
    size_t mask_ = 0;
    size_t mapBytes_ = 0;
};

}  // namespace sop
