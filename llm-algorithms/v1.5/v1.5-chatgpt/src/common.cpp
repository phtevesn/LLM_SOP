// Shared numeric, timing, cancellation, logging, and deterministic hashing utilities.
#include "sop/common.hpp"

#include <algorithm>
#include <iostream>

namespace sop {
std::atomic<std::sig_atomic_t> interrupted{0};
namespace { std::mutex log_mutex; }

Control::Control(Clock::time_point start, double seconds) : start_(start) {
    deadline_ = seconds < 0 ? Clock::time_point::max()
        : start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
}
bool Control::stopped() {
    if (reason() != Stop::running) return true;
    Stop next = Stop::running;
    if (interrupted.load(std::memory_order_relaxed)) next = Stop::interrupted;
    else if (Clock::now() >= deadline_) next = Stop::timeout;
    if (next != Stop::running) {
        Stop expected = Stop::running;
        reason_.compare_exchange_strong(expected, next, std::memory_order_acq_rel);
    }
    return reason() != Stop::running;
}
bool Control::prove() {
    if (stopped()) return false;
    Stop expected = Stop::running;
    // The timestamp check is the proof-completion event, before publishing it.
    if (Clock::now() >= deadline_) { stopped(); return false; }
    return reason_.compare_exchange_strong(expected, Stop::exhausted, std::memory_order_acq_rel);
}
void Control::fail() { reason_.store(Stop::error, std::memory_order_release); }
double Control::elapsed() const { return std::chrono::duration<double>(Clock::now() - start_).count(); }
double Control::remaining() const {
    if (deadline_ == Clock::time_point::max()) return std::numeric_limits<double>::infinity();
    return std::max(0.0, std::chrono::duration<double>(deadline_ - Clock::now()).count());
}
void log(const std::string& message) {
    std::lock_guard<std::mutex> guard(log_mutex);
    std::cerr << message << '\n';
}
std::uint64_t mix64(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}
} // namespace sop
