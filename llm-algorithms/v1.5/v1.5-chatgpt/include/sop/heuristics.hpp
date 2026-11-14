#pragma once
#include "sop/assignment.hpp"

#include <random>

namespace sop {
class Incumbent {
public:
    Incumbent(const Instance& input, Control& control) : input_(input), control_(control) {}
    Cost cost() const { return cost_.load(std::memory_order_acquire); }
    bool offer(const std::vector<int>& path, Cost claimed, const std::string& source);
    std::vector<int> path() const;
private:
    const Instance& input_;
    Control& control_;
    std::atomic<Cost> cost_{INF};
    mutable std::mutex mutex_;
    std::vector<int> path_;
};

Cost path_cost(const Graph&, const std::vector<int>&);
std::vector<int> greedy(const Graph&, std::mt19937_64&, int trial,
                        Control&, const AssignmentState* dual = nullptr,
                        const std::vector<int>& prefix = {});
void improve(const Graph&, std::vector<int>&, Control&, Clock::time_point until);
void heuristic_portfolio(const Graph&, Incumbent&, Control&, std::uint64_t seed,
                         Clock::time_point until, const AssignmentState& dual);
} // namespace sop
