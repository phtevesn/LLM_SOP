#pragma once
#include "sop/common.hpp"

#include <memory>

namespace sop {
class HistoryTable {
public:
    HistoryTable(int words, std::size_t bytes);
    bool dominated(const std::vector<std::uint64_t>& visited, int last, Cost cost, std::uint64_t hash);
    std::size_t bytes() const;
private:
    struct Entry { std::uint64_t hash = 0; Cost cost = INF; int last = -1; std::uint32_t age = 0; };
    int words_;
    std::size_t buckets_ = 0;
    std::uint32_t clock_ = 0;
    std::vector<Entry> entries_;
    std::vector<std::uint64_t> keys_;
};

class SharedHistory {
public:
    SharedHistory(int workers, int words, std::size_t bytes);
    void initialize(int worker); // Called on that worker after CPU binding (first touch).
    bool dominated(const std::vector<std::uint64_t>& visited, int last, Cost cost, std::uint64_t hash);
private:
    struct alignas(64) Shard { std::mutex mutex; std::unique_ptr<HistoryTable> table; };
    int workers_, words_;
    std::size_t bytes_;
    std::unique_ptr<Shard[]> shards_;
};
} // namespace sop
