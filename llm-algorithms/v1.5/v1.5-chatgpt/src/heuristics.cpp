// Feasible topological construction, precedence-safe Or-opt local search, and
// parallel randomized restarts; every published incumbent is independently validated.
#include "sop/heuristics.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>

namespace sop {
bool Incumbent::offer(const std::vector<int>& path, Cost claimed, const std::string& source) {
    if (claimed >= cost() || control_.stopped()) return false;
    // The validator reads the original input, not the reduced graph or duals.
    Cost checked = input_.validate(path);
    if (checked != claimed) throw std::logic_error("incumbent cost mismatch");
    std::lock_guard<std::mutex> guard(mutex_);
    if (checked >= cost() || control_.stopped()) return false;
    path_ = path;
    cost_.store(checked, std::memory_order_release);
    std::ostringstream message;
    message << "best cost = " << checked << " (" << source << ", " << control_.elapsed() << " s)";
    log(message.str());
    return true;
}
std::vector<int> Incumbent::path() const { std::lock_guard<std::mutex> guard(mutex_); return path_; }
Cost path_cost(const Graph& graph, const std::vector<int>& path) {
    Cost total = 0;
    for (std::size_t i = 1; i < path.size(); ++i) total = plus(total, graph.cost(path[i - 1], path[i]));
    return total;
}

std::vector<int> greedy(const Graph& g, std::mt19937_64& rng, int trial,
                        Control& control, const AssignmentState* dual, const std::vector<int>& prefix) {
    std::vector<int> degree(g.n), ready, path = prefix;
    std::vector<unsigned char> visited(g.n, 0);
    for (int v : prefix) visited[v] = 1;
    for (int v = 0; v < g.n; ++v) if (!visited[v]) {
        for (int p : g.predecessors[v]) if (!visited[p]) ++degree[v];
        if (!degree[v]) ready.push_back(v);
    }
    int last = path.empty() ? g.n : path.back();
    while (!ready.empty()) {
        if (control.stopped()) return {};
        struct Choice { long double score; int index; };
        std::vector<Choice> choices;
        choices.reserve(ready.size());
        for (std::size_t r = 0; r < ready.size(); ++r) {
            int v = ready[r];
            Cost c = g.cost(last, v);
            if (c >= INF) continue;
            // Alternate ordinary costs and assignment reduced costs; unlock counts break ties.
            Cost adjusted = dual && trial % 2 ? c - dual->u[last] - dual->v[v] : c;
            int unlock = 0;
            for (int w : g.successors[v]) if (!visited[w] && degree[w] == 1) ++unlock;
            long double score = static_cast<long double>(adjusted) -
                static_cast<long double>(unlock) / (g.n + 1);
            choices.push_back({score, static_cast<int>(r)});
        }
        if (choices.empty()) return {};
        const std::size_t width = trial < 2 ? 1 : std::min<std::size_t>(choices.size(), 2 + (trial % 4));
        std::partial_sort(choices.begin(), choices.begin() + static_cast<std::ptrdiff_t>(width), choices.end(),
                          [](const Choice& a, const Choice& b) { return a.score < b.score ||
                              (a.score == b.score && a.index < b.index); });
        std::size_t rank = 0;
        if (trial >= 2 && rng() % 4 == 0) rank = static_cast<std::size_t>(rng() % width);
        int index = choices[rank].index, v = ready[index];
        ready[index] = ready.back(); ready.pop_back();
        path.push_back(v); visited[v] = 1; last = v;
        for (int w : g.successors[v]) if (!visited[w] && --degree[w] == 0) ready.push_back(w);
    }
    return static_cast<int>(path.size()) == g.n ? path : std::vector<int>{};
}

void improve(const Graph& g, std::vector<int>& path, Control& control, Clock::time_point until) {
    const int n = g.n;
    if (n < 3 || static_cast<int>(path.size()) != n) return;
    std::vector<int> position(n);
    // Or-opt: relocate an order-preserving block of one, two, or three nodes.
    for (int pass = 0; pass < 1000; ++pass) {
        if (control.stopped() || Clock::now() >= until) return;
        for (int i = 0; i < n; ++i) position[path[i]] = i;
        Cost best_delta = 0;
        int best_a = -1, best_b = -1, best_length = 0;
        if (n <= 128) {
            // SOP-3-exchange-style adjacent-block swap: ABCD -> ACBD, keeping
            // both block interiors in order. A direct precedence crossing B->C
            // forbids the swap; no symmetric-distance assumption is made.
            std::vector<int> earlier_pred(n);
            for (int middle = 1; middle < n; ++middle) {
                if (control.stopped() || Clock::now() >= until) return;
                std::fill(earlier_pred.begin(), earlier_pred.end(), -1);
                for (int v = 0; v < n; ++v) for (int p : g.predecessors[v])
                    if (position[p] < middle) earlier_pred[v] = std::max(earlier_pred[v], position[p]);
                for (int a = 0; a < middle; ++a) {
                    int left = a == 0 ? n : path[a - 1];
                    for (int end = middle + 1; end <= n; ++end) {
                        if (earlier_pred[path[end - 1]] >= a) break;
                        int right = end == n ? n : path[end];
                        Cost x = g.cost(left, path[middle]);
                        Cost y = g.cost(path[end - 1], path[a]);
                        Cost z = g.cost(path[middle - 1], right);
                        if (x >= INF || y >= INF || z >= INF) continue;
                        Cost delta = x + y + z - g.cost(left, path[a]) -
                            g.cost(path[middle - 1], path[middle]) - g.cost(path[end - 1], right);
                        if (delta < best_delta) { best_delta = delta; best_a = a; best_b = end; best_length = middle - a; }
                    }
                }
            }
        }
        // For larger instances, bounded block lengths keep each pass quadratic.
        if (n > 128)
        for (int length = 1; length <= 3 && length < n; ++length) for (int a = 0; a + length <= n; ++a) {
            if (control.stopped() || Clock::now() >= until) return;
            int maximum_pred = -1, minimum_succ = n;
            for (int z = a; z < a + length; ++z) {
                for (int p : g.predecessors[path[z]]) if (position[p] < a || position[p] >= a + length)
                    maximum_pred = std::max(maximum_pred, position[p]);
                for (int s : g.successors[path[z]]) if (position[s] < a || position[s] >= a + length)
                    minimum_succ = std::min(minimum_succ, position[s]);
            }
            int left = a == 0 ? n : path[a - 1], right = a + length == n ? n : path[a + length];
            int first = path[a], last = path[a + length - 1];
            Cost bridge = g.cost(left, right);
            if (bridge >= INF) continue;
            for (int b = maximum_pred + 1; b <= minimum_succ; ++b) {
                if (b >= a && b <= a + length) continue;
                int before = b == 0 ? n : path[b - 1], after = b == n ? n : path[b];
                Cost x = g.cost(before, first), y = g.cost(last, after);
                if (x >= INF || y >= INF) continue;
                Cost delta = bridge + x + y - g.cost(left, first) - g.cost(last, right) - g.cost(before, after);
                if (delta < best_delta) { best_delta = delta; best_a = a; best_b = b; best_length = length; }
            }
        }
        if (best_a < 0) return;
        if (best_b < best_a)
            std::rotate(path.begin() + best_b, path.begin() + best_a, path.begin() + best_a + best_length);
        else std::rotate(path.begin() + best_a, path.begin() + best_a + best_length, path.begin() + best_b);
    }
}

void heuristic_portfolio(const Graph& g, Incumbent& incumbent, Control& control, std::uint64_t seed,
                         Clock::time_point until, const AssignmentState& dual) {
    std::mt19937_64 rng(seed);
    for (int trial = 0; trial < 100000 && !control.stopped() && Clock::now() < until; ++trial) {
        std::vector<int> path;
        if (trial > 2 && trial % 3 == 0) {
            path = incumbent.path();
            // Iterated local search: adjacent incomparable vertices may be swapped
            // without changing any precedence relation; perturb then improve.
            for (int attempt = 0; attempt < g.n * 2 && path.size() > 1; ++attempt) {
                auto a = static_cast<std::size_t>(rng() % (path.size() - 1));
                if (!g.before(path[a], path[a + 1])) std::swap(path[a], path[a + 1]);
            }
        } else path = greedy(g, rng, trial, control, &dual);
        if (path.empty()) return;
        incumbent.offer(path, path_cost(g, path), "greedy restart");
        // Limit any one restart so workers explore several constructions.
        auto slice = std::min(until, Clock::now() + std::chrono::milliseconds(20));
        improve(g, path, control, slice);
        incumbent.offer(path, path_cost(g, path), "precedence-safe local search");
    }
}
} // namespace sop
