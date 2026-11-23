// Exact short-window interchange dominance: cheaper (or lexicographically earlier
// equal-cost) reorderings of the last three internal prefix vertices dominate a prefix.
#include "sop/dominance.hpp"

#include <algorithm>
#include <array>

namespace sop {
bool window_dominated(const Graph& g, const std::vector<int>& path) {
    const int length = static_cast<int>(path.size());
    if (length < 3) return false;
    const int width = std::min(3, length - 1), begin = length - width - 1;
    const int left = begin == 0 ? g.n : path[begin - 1], right = path.back();
    std::array<int, 3> original{}, candidate{};
    for (int i = 0; i < width; ++i) original[i] = candidate[i] = path[begin + i];
    auto cost = [&](const std::array<int, 3>& order) {
        Cost value = g.cost(left, order[0]);
        for (int i = 1; i < width; ++i) value = plus(value, g.cost(order[i - 1], order[i]));
        return plus(value, g.cost(order[width - 1], right));
    };
    const Cost current = cost(original);
    // The window contains only two or three entries; explicit compare/exchange
    // also avoids speculative small-range std::sort array-bounds diagnostics.
    if (candidate[0] > candidate[1]) std::swap(candidate[0], candidate[1]);
    if (width == 3) {
        if (candidate[1] > candidate[2]) std::swap(candidate[1], candidate[2]);
        if (candidate[0] > candidate[1]) std::swap(candidate[0], candidate[1]);
    }
    do {
        if (candidate == original) continue;
        bool feasible = true;
        for (int a = 0; a < width; ++a) for (int b = a + 1; b < width; ++b)
            if (g.before(candidate[b], candidate[a])) feasible = false;
        if (!feasible) continue;
        Cost alternative = cost(candidate);
        if (alternative < current || (alternative == current && candidate < original)) return true;
    } while (std::next_permutation(candidate.begin(), candidate.begin() + width));
    return false;
}
} // namespace sop
