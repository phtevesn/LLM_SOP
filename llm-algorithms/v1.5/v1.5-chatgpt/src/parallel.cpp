// Linux topology-aware CPU placement and bounded work-stealing deques. Task
// accounting includes both queued and executing work, so emptiness alone is no proof.
#include "sop/parallel.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <tuple>
#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace sop {
namespace {
int read_integer(const std::string& path, int fallback) {
    std::ifstream stream(path); int value = fallback;
    if (!(stream >> value)) return fallback;
    return value;
}
}
std::vector<Placement> placements(int threads, bool bind) {
    std::vector<Placement> answer(static_cast<std::size_t>(threads));
    if (!bind) return answer;
#ifdef __linux__
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) return answer;
    std::map<int, std::vector<int>> primary, siblings;
    std::set<std::tuple<int, int, int>> cores;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) if (CPU_ISSET(cpu, &allowed)) {
        const std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(cpu);
        int package = read_integer(base + "/topology/physical_package_id", 0);
        int die = read_integer(base + "/topology/die_id", 0);
        int core = read_integer(base + "/topology/core_id", cpu);
        int node = -1;
        std::error_code ec;
        std::filesystem::directory_iterator it(base, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            auto name = it->path().filename().string();
            if (name.size() > 4 && name.compare(0, 4, "node") == 0 &&
                name.find_first_not_of("0123456789", 4) == std::string::npos)
                node = std::stoi(name.substr(4));
        }
        auto& group = cores.emplace(package, die, core).second ? primary : siblings;
        group[node].push_back(cpu);
    }
    std::vector<Placement> order;
    auto interleave = [&](const std::map<int, std::vector<int>>& groups) {
        for (std::size_t index = 0;; ++index) {
            bool added = false;
            for (const auto& [node, cpus] : groups) if (index < cpus.size()) {
                order.push_back({cpus[index], node}); added = true;
            }
            if (!added) break;
        }
    };
    // Spread physical cores across NUMA nodes before assigning SMT siblings.
    interleave(primary); interleave(siblings);
    if (!order.empty()) {
        if (static_cast<std::size_t>(threads) > order.size()) log("warning: more workers than CPUs in the process affinity mask");
        for (int i = 0; i < threads; ++i) answer[i] = order[static_cast<std::size_t>(i) % order.size()];
    }
#endif
    return answer;
}
bool pin(const Placement& placement) {
    if (placement.cpu < 0) return true;
#ifdef __linux__
    cpu_set_t mask; CPU_ZERO(&mask); CPU_SET(placement.cpu, &mask);
    return pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask) == 0;
#else
    return false;
#endif
}

Scheduler::Scheduler(int threads, const std::vector<Placement>& placement, Control& control, Task root)
    : threads_(threads), control_(control), queues_(new Queue[threads]), victims_(threads) {
    queues_[0].tasks.push_back(std::move(root));
    for (int i = 0; i < threads; ++i) {
        for (int offset = 1; offset < threads; ++offset) victims_[i].push_back((i + offset) % threads);
        // Prefer stealing within the same NUMA node; remote work remains reachable.
        std::stable_sort(victims_[i].begin(), victims_[i].end(), [&](int a, int b) {
            return (placement[a].node == placement[i].node) > (placement[b].node == placement[i].node);
        });
    }
}
bool Scheduler::arrive() {
    std::unique_lock<std::mutex> guard(wait_mutex_);
    ++arrived_;
    if (arrived_ == threads_) start_time_ = Clock::now();
    condition_.notify_all();
    while (arrived_ != threads_ && !control_.stopped()) condition_.wait_for(guard, std::chrono::milliseconds(2));
    return !control_.stopped();
}
bool Scheduler::pop(int owner, bool steal, Task& task) {
    auto& q = queues_[owner];
    std::unique_lock<std::mutex> guard(q.mutex, std::try_to_lock);
    if (!guard.owns_lock() || q.tasks.empty()) return false;
    if (steal) { task = std::move(q.tasks.front()); q.tasks.pop_front(); }
    else { task = std::move(q.tasks.back()); q.tasks.pop_back(); }
    queued_.fetch_sub(1, std::memory_order_relaxed); return true;
}
bool Scheduler::take(int worker, Task& task) {
    while (!control_.stopped()) {
        if (pop(worker, false, task)) return true;
        for (int victim : victims_[worker]) if (pop(victim, true, task)) return true;
        idle_.fetch_add(1, std::memory_order_relaxed);
        {
            std::unique_lock<std::mutex> guard(wait_mutex_);
            // A short timed wait also polls deadlines/signals if no publisher wakes us.
            condition_.wait_for(guard, std::chrono::milliseconds(2));
        }
        idle_.fetch_sub(1, std::memory_order_relaxed);
    }
    return false;
}
bool Scheduler::wants_work() const {
    return threads_ > 1 && queued_.load(std::memory_order_relaxed) < threads_ &&
        (idle_.load(std::memory_order_relaxed) > 0 || outstanding_.load(std::memory_order_relaxed) < static_cast<unsigned>(threads_));
}
void Scheduler::publish(int worker, Task task) {
    // Increment before making the task visible; its parent still owns an outstanding unit.
    outstanding_.fetch_add(1, std::memory_order_relaxed);
    {
        auto& q = queues_[worker]; std::lock_guard<std::mutex> guard(q.mutex);
        q.tasks.push_back(std::move(task)); queued_.fetch_add(1, std::memory_order_relaxed);
    }
    condition_.notify_all();
}
void Scheduler::finish() {
    if (outstanding_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        control_.prove(); condition_.notify_all();
    }
}
void Scheduler::wake() { condition_.notify_all(); }
} // namespace sop
