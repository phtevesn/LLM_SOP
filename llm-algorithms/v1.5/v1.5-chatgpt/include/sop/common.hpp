#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <csignal>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace sop {
using Cost = std::int64_t;
using Clock = std::chrono::steady_clock;
constexpr Cost INF = std::numeric_limits<Cost>::max() / 4;
static_assert(std::atomic<std::sig_atomic_t>::is_always_lock_free,
              "signal cancellation requires lock-free sig_atomic_t atomics");
extern std::atomic<std::sig_atomic_t> interrupted;

enum class Stop { running, exhausted, timeout, interrupted, error };

class Control {
public:
    Control(Clock::time_point start, double seconds);
    bool stopped();
    bool prove();
    void fail();
    double elapsed() const;
    double remaining() const;
    Stop reason() const { return reason_.load(std::memory_order_acquire); }
    Clock::time_point deadline() const { return deadline_; }
private:
    Clock::time_point start_, deadline_;
    std::atomic<Stop> reason_{Stop::running};
};

void log(const std::string& message);
std::uint64_t mix64(std::uint64_t value);
inline bool bit(const std::vector<std::uint64_t>& bits, int v) {
    return (bits[static_cast<std::size_t>(v) / 64] >> (v % 64)) & 1U;
}
inline void set_bit(std::vector<std::uint64_t>& bits, int v) {
    bits[static_cast<std::size_t>(v) / 64] |= std::uint64_t{1} << (v % 64);
}
inline void clear_bit(std::vector<std::uint64_t>& bits, int v) {
    bits[static_cast<std::size_t>(v) / 64] &= ~(std::uint64_t{1} << (v % 64));
}
inline Cost plus(Cost a, Cost b) { return (a >= INF || b >= INF) ? INF : a + b; }

struct Options {
    std::string input, output;
    double time_limit = -1.0;
    int threads = 1;
    std::size_t history_mb = 256;
    bool shared_history = true;
    bool affinity = true;
    bool component_bound = true;
    bool header = true;
    int tail_dp = 12;
    double heuristic_seconds = -1.0;
    std::uint64_t seed = 1;
};

struct Result {
    int n = 0;
    Cost cost = -1;
    std::vector<int> path;
    std::uint64_t enumerated = 0;
    bool optimal = false;
    bool infeasible = false;
    double seconds = 0;
};
} // namespace sop
