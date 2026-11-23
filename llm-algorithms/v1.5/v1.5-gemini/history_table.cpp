/*
 * File: history_table.cpp
 * Purpose: Implements the 64-bit SplitMix64 Zobrist table initialization and
 *          NUMA-aligned allocation/deallocation for the lock-free 4-way
 *          set-associative ZobristHistoryTable.
 */

#include "history_table.hpp"

namespace sop {

namespace {

// Algorithm: SplitMix64 Pseudorandom Bit Generator (Steele et al. 2014)
// Part: Generates high-entropy 64-bit keys for Zobrist hashing of vertices.
inline uint64_t splitmix64(uint64_t& state) noexcept {
    uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

} // namespace

ZobristHasher::ZobristHasher(int n)
    : n_(n), exact_packing_(n <= 56), vis_keys_(n, 0), last_keys_(n, 0) {
    uint64_t seed = 0x243F6A8885A308D3ULL;
    for (int i = 0; i < n; ++i) {
        vis_keys_[i] = splitmix64(seed) & ~(1ULL << 63);
        last_keys_[i] = splitmix64(seed) & ~(1ULL << 63);
    }
}

ZobristHistoryTable::ZobristHistoryTable(size_t num_buckets_pow2, int memory_numa_node) {
    size_t cap = 1;
    while (cap < num_buckets_pow2) cap <<= 1;
    num_buckets_ = cap;
    mask_ = cap - 1;
    alloc_bytes_ = num_buckets_ * sizeof(HistoryBucket);
    buckets_ = static_cast<HistoryBucket*>(
        NumaTopology::allocate_numa_aligned(alloc_bytes_, memory_numa_node));
}

ZobristHistoryTable::~ZobristHistoryTable() {
    if (buckets_) {
        NumaTopology::free_numa_aligned(buckets_, alloc_bytes_);
        buckets_ = nullptr;
    }
}

} // namespace sop
