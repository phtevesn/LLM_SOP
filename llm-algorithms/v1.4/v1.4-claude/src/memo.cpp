#include "memo.hpp"

#include <sys/mman.h>

#include <climits>

#include "numa.hpp"

namespace sop {

MemoTable::MemoTable(size_t bytes, const std::vector<int>& memNodes) {
    size_t b = 4096;
    while (b * 2 <= bytes) b *= 2;
    for (; b >= 4096; b /= 2) {
        void* p = mmap(nullptr, b, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (p != MAP_FAILED) {
            tab_ = static_cast<Entry*>(p);
            mapBytes_ = b;
            nBuckets_ = b / (4 * sizeof(Entry));
            mask_ = nBuckets_ - 1;
            adviseInterleaved(p, b, memNodes);
            return;
        }
        if (b == 4096) break;
    }
}

MemoTable::~MemoTable() {
    if (tab_) munmap(tab_, mapBytes_);
}

bool MemoTable::checkAndInsert(uint64_t key1, uint64_t key2, int depth, int64_t cost) {
    if (!tab_) return false;
    const uint64_t tag = key2 >> 50;  // 14 bits
    Entry* bucket = tab_ + (key1 & mask_) * 4;
    const uint64_t newd = (uint64_t)cost | ((uint64_t)(depth & 1023) << 40) | (tag << 50);
    int victim = 0;
    int victimScore = -1;
    for (int s = 0; s < 4; ++s) {
        uint64_t d = bucket[s].d.load(std::memory_order_relaxed);
        uint64_t a = bucket[s].a.load(std::memory_order_relaxed);
        if (d == 0 && a == 0) {  // empty slot is the best victim
            if (victimScore < 5000) { victim = s; victimScore = 5000; }
            continue;
        }
        if ((a ^ d) == key1 && (d >> 50) == tag) {
            if ((int64_t)(d & kMaxCost) <= cost) return true;  // dominated
            bucket[s].d.store(newd, std::memory_order_relaxed);
            bucket[s].a.store(key1 ^ newd, std::memory_order_relaxed);
            return false;
        }
        int dep = (int)((d >> 40) & 1023);  // deeper states save less work: evict them first
        if (dep > victimScore) { victim = s; victimScore = dep; }
    }
    bucket[victim].d.store(newd, std::memory_order_relaxed);
    bucket[victim].a.store(key1 ^ newd, std::memory_order_relaxed);
    return false;
}

}  // namespace sop
