#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace sop {
using Cost = std::int64_t;
constexpr int MAX_N = 1000;
constexpr int BIT_WORDS = (MAX_N + 63) / 64;
constexpr Cost INF = std::numeric_limits<Cost>::max() / 4;
using Bits = std::array<std::uint64_t, BIT_WORDS>;
using Clock = std::chrono::steady_clock;
inline std::atomic<bool> interrupt_requested{false};
static_assert(std::atomic<bool>::is_always_lock_free, "Signal cancellation requires a lock-free atomic bool");

inline bool contains(const Bits& b, int i) { return (b[i / 64] >> (i % 64)) & 1U; }
inline void insert(Bits& b, int i) { b[i / 64] |= std::uint64_t{1} << (i % 64); }
inline void erase(Bits& b, int i) { b[i / 64] &= ~(std::uint64_t{1} << (i % 64)); }
inline bool subset(const Bits& a, const Bits& b, int words) {
    for (int w = 0; w < words; ++w) if (a[w] & ~b[w]) return false;
    return true;
}

struct Options {
    std::string input, output;
    double time_limit = -1.0;
    unsigned threads = 1;
    std::size_t history_mb = 256;
    std::uint64_t seed = 1;
    double heuristic_seconds = 0.5;
    bool header = true;
    bool pin = true;
    // 0: disabled; 1: root and every eighth depth; 2: every state.
    int connectivity = 1;
};

// All phases share the same absolute wall-clock deadline. Cancellation is
// cooperative; callers must never translate a cancelled bound into a proof.
class Deadline {
public:
    Deadline(Clock::time_point start, double seconds) : start_(start), limit_(seconds) {}
    bool expired() const {
        return interrupt_requested.load(std::memory_order_relaxed) || (limit_ >= 0 && elapsed() >= limit_);
    }
    double elapsed() const { return std::chrono::duration<double>(Clock::now() - start_).count(); }
    double remaining() const { return limit_ < 0 ? 1e100 : limit_ - elapsed(); }
private:
    Clock::time_point start_;
    double limit_;
};

struct Result {
    Cost cost = -1;
    std::vector<int> path;
    std::uint64_t nodes = 0;
    bool optimal = false;
    bool infeasible = false;
    double proof_seconds = -1;
};
} // namespace sop
