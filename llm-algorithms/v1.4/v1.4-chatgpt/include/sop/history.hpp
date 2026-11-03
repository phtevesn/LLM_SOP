#pragma once
#include "sop/types.hpp"
#include <cstdlib>
#include <memory>
#include <mutex>

namespace sop {
// Bounded, four-way associative cache. Every hit compares the ENTIRE state;
// hash collisions or eviction can reduce speed but cannot alter the answer.
class History {
public:
    History(std::size_t megabytes, int words);
    bool dominated(const Bits& visited, int last, Cost prefix);
    std::uint64_t hits() const { return hits_.load(std::memory_order_relaxed); }
private:
    static constexpr std::size_t SHARDS = 256;
    struct alignas(64) Shard {
        std::mutex mutex;
        std::uint64_t* entries = nullptr;
        bool disabled = false;
        std::size_t cursor = 0;
        ~Shard() { std::free(entries); }
    };
    std::unique_ptr<Shard[]> shards_;
    std::size_t buckets_ = 0, stride_ = 0;
    int words_;
    std::atomic<std::uint64_t> hits_{0};
};
} // namespace sop
