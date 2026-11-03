#include "sop/assignment.hpp"
#include <algorithm>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>

using namespace sop;

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

static Cost allowed(const Instance& p, const Assignment& a, const Bits& visited, int r, int c) {
    if (r == a.last) {
        if (c == p.n && a.rows.size() != 1) return INF;
        if (c != p.n && !subset(p.pred[c], visited, p.words)) return INF;
    }
    return p.edge(r, c);
}

// Independent subset-DP assignment oracle. No Hungarian code or potentials.
static Cost oracle(const Instance& p, const Assignment& a, const Bits& visited) {
    const std::size_t states = std::size_t{1} << a.cols.size();
    std::vector<Cost> dp(states, INF);
    dp[0] = 0;
    for (std::size_t mask = 0; mask < states; ++mask) if (dp[mask] != INF) {
        const auto row = static_cast<std::size_t>(__builtin_popcountll(mask));
        if (row == a.rows.size()) continue;
        for (std::size_t c = 0; c < a.cols.size(); ++c) if (!(mask & (std::size_t{1} << c))) {
            const Cost cost = allowed(p, a, visited, a.rows[row], a.cols[c]);
            if (cost == INF) continue;
            auto& destination = dp[mask | (std::size_t{1} << c)];
            destination = std::min(destination, dp[mask] + cost);
        }
    }
    return dp.back();
}

static Cost completion_oracle(const Instance& p, int last, Bits visited, int remaining) {
    if (!remaining) return p.edge(last, p.n);
    Cost best = INF;
    for (int v = 0; v < p.n; ++v) if (!contains(visited, v) && subset(p.pred[v], visited, p.words)) {
        insert(visited, v);
        best = std::min(best, p.edge(last, v) + completion_oracle(p, v, visited, remaining - 1));
        erase(visited, v);
    }
    return best;
}

int main() {
    std::mt19937_64 rng(481729);
    std::uint64_t checked = 0;
    for (int test = 0; test < 1200; ++test) {
        Instance p;
        p.n = 1 + static_cast<int>(rng() % 9); p.words = 1;
        p.original.resize(std::size_t(p.n) * p.n);
        for (int i = 0; i < p.n; ++i) for (int j = 0; j < p.n; ++j)
            p.original[std::size_t(i) * p.n + j] = i == j ? 0 : static_cast<Cost>(rng() % 97);
        std::vector<int> order(p.n);
        std::iota(order.begin(), order.end(), 0); std::shuffle(order.begin(), order.end(), rng);
        for (int i = 0; i < p.n; ++i) for (int j = i + 1; j < p.n; ++j)
            if (rng() % 4 == 0) p.original[std::size_t(order[j]) * p.n + order[i]] = -1;
        const Deadline deadline(Clock::now(), -1);
        require(preprocess(p, deadline), "Preprocess cancelled unexpectedly");
        Assignment a;
        AssignmentScratch scratch(p.n + 1);
        Bits visited{};
        require(a.initialize(p, visited, scratch, deadline) == BoundStatus::feasible, "Root matching failed");
        for (int depth = 0; depth <= p.n; ++depth) {
            require(a.value == oracle(p, a, visited), "Warm matching differs from independent assignment oracle");
            const Cost penalty = a.connectivity_penalty(p, visited, deadline);
            require(penalty != INF && penalty >= 0, "Invalid connectivity penalty");
            require(a.value + penalty <= completion_oracle(p, a.last, visited, p.n - depth), "Connectivity bound exceeds true completion optimum");
            for (int r : a.rows) {
                require(a.col_match[a.row_match[r]] == r, "Matching is not bijective");
                require(allowed(p, a, visited, r, a.row_match[r]) == a.u[r] + a.v[a.row_match[r]], "Matched edge is not dual tight");
                for (int c : a.cols) {
                    const Cost cost = allowed(p, a, visited, r, c);
                    require(cost == INF || cost >= a.u[r] + a.v[c], "Dual infeasibility");
                }
            }
            ++checked;
            if (depth == p.n) break;
            const int next = order[depth]; insert(visited, next);
            require(a.extend(p, next, visited, scratch, deadline) == BoundStatus::feasible, "Valid prefix has no matching");
        }
    }
    std::cout << "Assignment oracle: " << checked << " residual states passed\n";
}
