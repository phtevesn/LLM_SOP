// Bounded exact-key history dominance: hashes select buckets but never establish
// equality. Evictions lose pruning opportunities without affecting correctness.
#include "sop/history.hpp"

#include <algorithm>

namespace sop {
HistoryTable::HistoryTable(int words, std::size_t bytes) : words_(words) {
    const std::size_t bucket_bytes = 4 * (sizeof(Entry) + static_cast<std::size_t>(words) * sizeof(std::uint64_t));
    std::size_t maximum = bytes / bucket_bytes;
    if (!maximum) return;
    buckets_ = 1;
    while (buckets_ <= maximum / 2) buckets_ *= 2;
    entries_.resize(buckets_ * 4);
    keys_.resize(buckets_ * 4 * static_cast<std::size_t>(words_));
}
std::size_t HistoryTable::bytes() const { return entries_.size() * sizeof(Entry) + keys_.size() * sizeof(std::uint64_t); }
bool HistoryTable::dominated(const std::vector<std::uint64_t>& visited, int last, Cost cost, std::uint64_t hash) {
    if (!buckets_) return false;
    std::size_t begin = (hash & (buckets_ - 1)) * 4, replace = begin;
    ++clock_;
    for (std::size_t i = begin; i < begin + 4; ++i) {
        auto& e = entries_[i];
        const auto* key = &keys_[i * static_cast<std::size_t>(words_)];
        if (e.last == last && e.hash == hash && std::equal(visited.begin(), visited.end(), key)) {
            e.age = clock_;
            if (e.cost <= cost) return true;
            e.cost = cost; return false;
        }
        if (e.last < 0 || static_cast<std::uint32_t>(clock_ - e.age) >
            static_cast<std::uint32_t>(clock_ - entries_[replace].age)) replace = i;
    }
    entries_[replace] = {hash, cost, last, clock_};
    std::copy(visited.begin(), visited.end(), keys_.begin() + static_cast<std::ptrdiff_t>(replace * words_));
    return false;
}
SharedHistory::SharedHistory(int workers, int words, std::size_t bytes)
    : workers_(workers), words_(words), bytes_(bytes / static_cast<std::size_t>(workers)), shards_(new Shard[workers]) {}
void SharedHistory::initialize(int worker) {
    shards_[worker].table = std::make_unique<HistoryTable>(words_, bytes_);
}
bool SharedHistory::dominated(const std::vector<std::uint64_t>& visited, int last, Cost cost, std::uint64_t hash) {
    auto& shard = shards_[mix64(hash) % static_cast<std::uint64_t>(workers_)];
    std::lock_guard<std::mutex> guard(shard.mutex);
    return shard.table->dominated(visited, last, cost, hash);
}
} // namespace sop
