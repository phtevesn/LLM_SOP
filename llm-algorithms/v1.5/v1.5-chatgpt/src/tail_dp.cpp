// Exact Held-Karp subset dynamic programming on a small remaining suffix, with
// precedence masks and explicit reconstruction of an optimal completion.
#include "sop/tail_dp.hpp"

#include <functional>
#include <stdexcept>

namespace sop {
TailResult solve_tail(const Graph& g, const std::vector<std::uint64_t>& visited,
                      int last, Control& control, std::uint64_t& count) {
    std::vector<int> nodes;
    for (int v = 0; v < g.n; ++v) if (!bit(visited, v)) nodes.push_back(v);
    const int k = static_cast<int>(nodes.size());
    if (k > 20) throw std::logic_error("tail DP exceeds configured maximum");
    const std::uint32_t full = (std::uint32_t{1} << k) - 1;
    std::vector<std::uint32_t> pred(k, 0);
    for (int i = 0; i < k; ++i) for (int j = 0; j < k; ++j)
        if (g.before(nodes[j], nodes[i])) pred[i] |= std::uint32_t{1} << j;
    const std::size_t states = (static_cast<std::size_t>(full) + 1) * (k + 1);
    std::vector<Cost> memo(states, -1);
    std::vector<signed char> choice(states, -1);
    bool cancelled = false;
    std::function<Cost(std::uint32_t, int)> visit = [&](std::uint32_t mask, int end) -> Cost {
        const auto index = static_cast<std::size_t>(mask) * (k + 1) + end;
        if (memo[index] >= 0) return memo[index];
        // The tail root was already counted by B&B; only new non-root DP states count here.
        if (end != k) ++count;
        if ((count & 255U) == 0 && control.stopped()) { cancelled = true; return INF; }
        const int actual = end == k ? last : nodes[end];
        if (mask == full) return memo[index] = g.cost(actual, g.n);
        Cost best = INF;
        for (int v = 0; v < k; ++v) {
            const auto b = std::uint32_t{1} << v;
            if ((mask & b) || (pred[v] & ~mask) || g.cost(actual, nodes[v]) >= INF) continue;
            Cost value = plus(g.cost(actual, nodes[v]), visit(mask | b, v));
            if (cancelled) return INF;
            if (value < best) { best = value; choice[index] = static_cast<signed char>(v); }
        }
        return memo[index] = best;
    };
    if (control.stopped()) return {};
    Cost cost = visit(0, k);
    if (cancelled || control.stopped()) return {};
    TailResult result; result.complete = true; result.cost = cost;
    if (cost >= INF) return result;
    std::uint32_t mask = 0;
    int end = k;
    while (mask != full) {
        int v = choice[static_cast<std::size_t>(mask) * (k + 1) + end];
        if (v < 0) throw std::logic_error("missing tail-DP reconstruction");
        result.suffix.push_back(nodes[v]); mask |= std::uint32_t{1} << v; end = v;
    }
    return result;
}
} // namespace sop
