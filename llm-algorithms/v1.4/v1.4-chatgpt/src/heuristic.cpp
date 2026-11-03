#include "sop/heuristic.hpp"
#include <algorithm>
#include <cmath>

namespace sop {
namespace {
Cost path_cost(const Instance& p, const std::vector<int>& path) {
    Cost value = 0;
    for (int i = 1; i < p.n; ++i) value += p.edge(path[i - 1], path[i]);
    return value;
}

// Relocate [start,start+length) after position `after` in the original path.
// after == -1 means the front. Internal order is preserved.
void relocate(std::vector<int>& path, int start, int length, int after) {
    if (after < start)
        std::rotate(path.begin() + after + 1, path.begin() + start, path.begin() + start + length);
    else
        std::rotate(path.begin() + start, path.begin() + start + length, path.begin() + after + 1);
}

bool can_cross(const Instance& p, const std::vector<int>& path, int start, int length, int crossed, bool right) {
    for (int k = 0; k < length; ++k) {
        const int b = path[start + k], x = path[crossed];
        if (right ? contains(p.pred[x], b) : contains(p.pred[b], x)) return false;
    }
    return true;
}

Cost relocation_delta(const Instance& p, const std::vector<int>& path, int start, int length, int after) {
    const int a = start == 0 ? p.n : path[start - 1];
    const int b = start + length == p.n ? p.n : path[start + length];
    const int x = path[start], y = path[start + length - 1];
    const int c = after == -1 ? p.n : path[after];
    const int d = after + 1 == p.n ? p.n : path[after + 1];
    const Cost ab = p.edge(a, b), cx = p.edge(c, x), yd = p.edge(y, d);
    if (ab == INF || cx == INF || yd == INF) return INF;
    return ab + cx + yd - p.edge(a, x) - p.edge(y, b) - p.edge(c, d);
}
} // namespace

std::vector<int> greedy_path(const Instance& p, std::mt19937_64& rng, int variation, const Stop& stop) {
    auto degree = p.indegree;
    std::vector<int> path;
    Bits visited{};
    int last = p.n;
    for (int step = 0; step < p.n; ++step) {
        if (stop()) return {};
        std::vector<std::pair<double, int>> candidates;
        for (int i = 0; i < p.n; ++i) if (!degree[i] && !contains(visited, i)) {
            const Cost edge = p.edge(last, i);
            if (edge == INF) continue;
            double score = static_cast<double>(edge);
            if (variation % 3 == 1) {
                Cost outgoing = INF;
                for (int j = 0; j <= p.n; ++j)
                    if (j == p.n || (!contains(visited, j) && j != i)) outgoing = std::min(outgoing, p.edge(i, j));
                if (outgoing != INF) score += 0.35 * static_cast<double>(outgoing);
            }
            if (variation % 3 == 2) score -= 0.01 * static_cast<double>(p.successors[i].size());
            candidates.emplace_back(score, i);
        }
        if (candidates.empty()) return {};
        const int restricted = variation == 0 ? 1 : std::min<int>(3, static_cast<int>(candidates.size()));
        std::partial_sort(candidates.begin(), candidates.begin() + restricted, candidates.end());
        int pick = 0;
        if (variation != 0 && rng() % 4 == 0) pick = static_cast<int>(rng() % restricted);
        last = candidates[pick].second;
        path.push_back(last); insert(visited, last);
        for (int s : p.successors[last]) --degree[s];
    }
    return path;
}

void improve_path(const Instance& p, std::vector<int>& path, const Stop& stop) {
    if (p.n < 2) return;
    // Precedence-preserving Or-opt with several larger block neighborhoods.
    // Stop at the first forbidden crossing, so legality costs O(length) per move.
    bool changed = true;
    while (changed && !stop()) {
        changed = false;
        for (int length : {1, 2, 3, 4, 8, 16}) {
            if (changed || length >= p.n) break;
            for (int i = 0; i + length <= p.n && !changed; ++i) {
                if (stop()) return;
                for (int j = i + length; j < p.n; ++j) {
                    if (!can_cross(p, path, i, length, j, true)) break;
                    if (relocation_delta(p, path, i, length, j) < 0) {
                        relocate(path, i, length, j); changed = true; break;
                    }
                }
                if (changed) break;
                for (int j = i - 1; j >= 0; --j) {
                    if (!can_cross(p, path, i, length, j, false)) break;
                    if (relocation_delta(p, path, i, length, j - 1) < 0) {
                        relocate(path, i, length, j - 1); changed = true; break;
                    }
                }
            }
        }
    }
}

void heuristic_search(const Instance& p, std::uint64_t seed, const std::vector<int>& initial,
                      const Stop& stop, const Publish& publish, const Incumbent& incumbent) {
    std::mt19937_64 rng(seed);
    std::vector<int> best = initial;
    Cost best_cost = best.empty() ? INF : path_cost(p, best);
    std::uint64_t iteration = 0;
    while (!stop()) {
        if (iteration % 16 == 0) {
            auto global = incumbent();
            if (!global.empty()) {
                const Cost c = path_cost(p, global);
                if (c < best_cost) { best_cost = c; best = std::move(global); }
            }
        }
        std::vector<int> path;
        if (best.empty() || iteration % 8 == 0)
            path = greedy_path(p, rng, static_cast<int>(iteration % 3) + 1, stop);
        else {
            path = best;
            const int kicks = 2 + static_cast<int>(iteration % 7);
            for (int k = 0; k < kicks && !stop(); ++k) {
                const int i = static_cast<int>(rng() % p.n), j = static_cast<int>(rng() % p.n);
                const int length = 1 + static_cast<int>(rng() % std::min(6, p.n - i));
                if (j >= i && j < i + length) continue;
                bool valid = true;
                const int begin = j > i ? i + length : j;
                const int end = j > i ? j : i - 1;
                for (int x = begin; x <= end; ++x) {
                    if (!can_cross(p, path, i, length, x, j > i)) { valid = false; break; }
                }
                if (valid) relocate(path, i, length, j > i ? j : j - 1);
            }
        }
        if (path.empty()) break;
        improve_path(p, path, stop);
        if (stop()) { publish(path); break; }
        const Cost c = path_cost(p, path);
        if (c < best_cost) { best_cost = c; best = path; publish(path); }
        ++iteration;
    }
}
} // namespace sop
