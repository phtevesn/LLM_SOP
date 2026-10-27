#include "sop/history.hpp"
#include <algorithm>
#include <cstring>

namespace sop {
namespace {
std::uint64_t mix(std::uint64_t x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}
} // namespace
History::History(std::size_t megabytes, int words)
    : shards_(new Shard[SHARDS]), stride_(static_cast<std::size_t>(words) + 3), words_(words) {
    const std::size_t available = megabytes * 1024 * 1024 / (SHARDS * 4 * stride_ * sizeof(std::uint64_t));
    if (available) {
        buckets_ = 1;
        while (buckets_ <= available / 2) buckets_ *= 2;
    }
}

bool History::dominated(const Bits& visited, int last, Cost prefix) {
    if (!buckets_) return false;
    std::uint64_t hash = mix(static_cast<std::uint64_t>(last) + 1);
    for (int w = 0; w < words_; ++w) hash = mix(hash ^ visited[w]);
    hash |= 1; // zero is the empty-slot marker
    // Do not use the forced low bit for shard selection.
    Shard& shard = shards_[(hash >> 1) & (SHARDS - 1)];
    std::lock_guard<std::mutex> lock(shard.mutex);
    if (shard.disabled) return false;
    if (!shard.entries) {
        shard.entries = static_cast<std::uint64_t*>(std::calloc(buckets_ * 4 * stride_, sizeof(std::uint64_t)));
        if (!shard.entries) { shard.disabled = true; return false; }
    }
    const std::size_t bucket = (hash >> 9) & (buckets_ - 1);
    auto* base = shard.entries + bucket * 4 * stride_;
    std::uint64_t* slot = nullptr;
    for (std::size_t i = 0; i < 4; ++i) {
        auto* e = base + i * stride_;
        if (e[0] == hash && e[1] == static_cast<std::uint64_t>(last) &&
            std::memcmp(e + 3, visited.data(), std::size_t(words_) * sizeof(std::uint64_t)) == 0) {
            if (e[2] <= static_cast<std::uint64_t>(prefix)) {
                hits_.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            e[2] = static_cast<std::uint64_t>(prefix);
            return false;
        }
        if (!e[0]) slot = e;
    }
    if (!slot) slot = base + ((shard.cursor++) & 3) * stride_;
    slot[0] = hash; slot[1] = static_cast<std::uint64_t>(last); slot[2] = static_cast<std::uint64_t>(prefix);
    std::memcpy(slot + 3, visited.data(), std::size_t(words_) * sizeof(std::uint64_t));
    return false;
}
} // namespace sop
