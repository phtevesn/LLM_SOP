// Independent exhaustive checks of assignment updates, lower-bound admissibility,
// suffix DP, collision-safe dominance, and feasibility-preserving local search.
#include "sop/assignment.hpp"
#include "sop/heuristics.hpp"
#include "sop/history.hpp"
#include "sop/tail_dp.hpp"

#include <algorithm>
#include <functional>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>

using namespace sop;
namespace {
void require(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
Instance random_instance(int n, int density, std::mt19937_64& rng) {
    Instance in; in.n = n; in.matrix.resize(static_cast<std::size_t>(n) * n);
    std::vector<int> order(n); std::iota(order.begin(), order.end(), 0); std::shuffle(order.begin(), order.end(), rng);
    for (int i = 0; i < n; ++i) for (int j = 0; j < n; ++j)
        in.matrix[static_cast<std::size_t>(i) * n + j] = i == j ? 0 : static_cast<Cost>(rng() % 101);
    for (int a = 0; a < n; ++a) for (int b = a + 1; b < n; ++b) if (static_cast<int>(rng() % 100) < density)
        in.matrix[static_cast<std::size_t>(order[b]) * n + order[a]] = -1 - static_cast<Cost>(rng() % 9);
    return in;
}
Cost brute_suffix(const Instance& in, const std::vector<int>& prefix) {
    std::vector<bool> used(in.n, false);
    for (int v : prefix) used[v] = true;
    std::function<Cost(int, int)> dfs = [&](int last, int length) -> Cost {
        if (length == in.n) return 0;
        Cost best = INF;
        for (int v = 0; v < in.n; ++v) if (!used[v]) {
            bool ready = true;
            for (int p = 0; p < in.n; ++p) if (in.at(v, p) < 0 && !used[p]) { ready = false; break; }
            if (!ready) continue;
            Cost edge = last < 0 ? 0 : in.at(last, v);
            if (edge < 0) continue;
            used[v] = true; best = std::min(best, plus(edge, dfs(v, length + 1))); used[v] = false;
        }
        return best;
    };
    return dfs(prefix.empty() ? -1 : prefix.back(), static_cast<int>(prefix.size()));
}
Cost brute_assignment(const Graph& g, const AssignmentState& state) {
    std::vector<int> rows, cols;
    for (int i = 0; i < g.m; ++i) { if (state.row[i] != -2) rows.push_back(i); if (state.col[i] != -2) cols.push_back(i); }
    Cost best = INF;
    do {
        Cost cost = 0;
        for (std::size_t i = 0; i < rows.size(); ++i)
            cost = plus(cost, rows[i] == state.frontier && !state.allowed[cols[i]] ? INF : g.cost(rows[i], cols[i]));
        best = std::min(best, cost);
    } while (std::next_permutation(cols.begin(), cols.end()));
    return best;
}
void same(const AssignmentState& a, const AssignmentState& b) {
    require(a.bound == b.bound && a.u == b.u && a.v == b.v && a.row == b.row && a.col == b.col &&
            a.frontier == b.frontier && a.allowed == b.allowed, "rollback changed state");
}
}
int main() {
    try {
        std::mt19937_64 rng(0x534f5054455354ULL);
        std::uint64_t checked = 0;
        for (int trial = 0; trial < 160; ++trial) {
            int n = 2 + trial % 7;
            Instance in = random_instance(n, (trial % 5) * 20, rng);
            Control control(Clock::now(), -1);
            int orders = 0;
            Graph g = Graph::build(in, control, [&](const std::vector<int>& order) {
                require(in.validate(order) >= 0, "early topological incumbent is invalid"); ++orders;
            });
            require(orders == 1, "acyclic preprocessing did not publish its feasible order");
            Assignment ap(g, control);
            require(ap.initialize(), "root assignment unexpectedly infeasible");
            std::vector<int> prefix;
            std::vector<std::uint64_t> visited(g.words, 0);
            while (static_cast<int>(prefix.size()) < n) {
                ap.check();
                require(ap.bound() == brute_assignment(g, ap.state()), "assignment differs from permutation oracle");
                Cost exact = brute_suffix(in, prefix);
                require(plus(ap.bound(), ap.component_extra()) <= exact, "component bound exceeds raw-matrix optimum");
                std::uint64_t count = 0;
                int last = prefix.empty() ? n : prefix.back();
                auto tail = solve_tail(g, visited, last, control, count);
                require(tail.complete && tail.cost == exact, "tail DP differs from raw-matrix oracle");
                auto full = prefix; full.insert(full.end(), tail.suffix.begin(), tail.suffix.end());
                require(in.validate(full) == path_cost(g, prefix) + exact, "DP reconstruction mismatch");
                std::vector<int> candidates;
                for (int v = 0; v < n; ++v) if (!bit(visited, v) && g.available(v, visited)) candidates.push_back(v);
                require(!candidates.empty(), "acyclic prefix has no extension");
                for (int v : candidates) {
                    auto state = ap.state(); auto mark = ap.mark();
                    require(ap.force(last, v), "feasible prefix lost by incremental assignment");
                    set_bit(visited, v);
                    require(ap.restrict_frontier(v, visited), "frontier filtering lost a feasible suffix");
                    clear_bit(visited, v);
                    ap.check();
                    require(ap.bound() == brute_assignment(g, ap.state()), "incremental assignment not optimal");
                    ap.rollback(mark); same(state, ap.state()); ++checked;
                }
                int v = candidates[static_cast<std::size_t>(rng() % candidates.size())];
                require(ap.force(last, v), "path extension failed"); prefix.push_back(v); set_bit(visited, v);
                require(ap.restrict_frontier(v, visited), "frontier filtering failed");
            }
            ap.check(); require(ap.bound() == 0, "completed suffix must have zero closing cost");
        }
        // Same hash, distinct full keys: no probabilistic pruning is permitted.
        HistoryTable table(2, 1024);
        std::vector<std::uint64_t> a{1, 0}, b{1, std::uint64_t{1} << 63};
        require(!table.dominated(a, 3, 10, 7), "new state dominated");
        require(!table.dominated(b, 3, 20, 7), "hash collision caused false dominance");
        require(table.dominated(a, 3, 11, 7), "equal state not dominated");
        require(!table.dominated(a, 3, 9, 7), "better state pruned");
        require(!table.dominated(a, 4, 99, 7), "different last node pruned");
        HistoryTable disabled(2, 0);
        require(!disabled.dominated(a, 3, 10, 7) && !disabled.dominated(a, 3, 11, 7), "disabled history prunes");
        for (int trial = 0; trial < 80; ++trial) {
            auto in = random_instance(10 + trial % 11, (trial % 5) * 15, rng);
            Control control(Clock::now(), -1); auto g = Graph::build(in, control);
            auto path = greedy(g, rng, trial % 8, control);
            Cost old = in.validate(path);
            improve(g, path, control, Clock::now() + std::chrono::milliseconds(10));
            require(in.validate(path) <= old, "Or-opt worsened cost or invalidated path");
        }
        std::cout << "core tests passed: 160 random DAGs, " << checked << " incremental restrictions, "
                     "exhaustive assignment/SOP oracles, collision cases, 80 local-search checks\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAILED: " << e.what() << '\n'; return 1; }
}
