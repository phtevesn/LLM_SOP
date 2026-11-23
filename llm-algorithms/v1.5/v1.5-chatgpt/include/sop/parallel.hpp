#pragma once
#include "sop/assignment.hpp"

#include <condition_variable>
#include <deque>
#include <memory>

namespace sop {
struct Placement { int cpu = -1, node = -1; };
std::vector<Placement> placements(int threads, bool bind);
bool pin(const Placement& placement);

struct Task {
    std::vector<int> prefix;
    Cost cost = 0;
    AssignmentState assignment;
};

class Scheduler {
public:
    Scheduler(int threads, const std::vector<Placement>& placement, Control&, Task root);
    bool arrive();
    Clock::time_point start_time() const { return start_time_; } // Read only after arrive().
    bool take(int worker, Task& task);
    bool wants_work() const;
    void publish(int worker, Task task);
    void finish();
    void wake();
private:
    struct alignas(64) Queue { std::mutex mutex; std::deque<Task> tasks; };
    int threads_;
    Control& control_;
    std::unique_ptr<Queue[]> queues_;
    std::vector<std::vector<int>> victims_;
    std::atomic<std::uint64_t> outstanding_{1};
    std::atomic<int> queued_{1}, idle_{0};
    std::mutex wait_mutex_;
    std::condition_variable condition_;
    int arrived_ = 0;
    Clock::time_point start_time_{};
    bool pop(int owner, bool steal, Task& task);
};
} // namespace sop
