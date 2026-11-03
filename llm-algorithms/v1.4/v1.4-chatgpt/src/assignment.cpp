#include "sop/assignment.hpp"
#include <algorithm>
#include <numeric>
#include <stdexcept>

namespace sop {
Cost Assignment::cost(const Instance& p, int row, int col, const Bits& visited) const {
    const Cost c = p.edge(row, col);
    if (c == INF || row != last) return c;
    if (col == p.n) return rows.size() == 1 ? c : INF;
    return subset(p.pred[col], visited, p.words) ? c : INF;
}

BoundStatus Assignment::initialize(const Instance& p, const Bits& visited, AssignmentScratch& scratch, const Deadline& deadline) {
    const int size = p.n + 1;
    rows.resize(size); cols.resize(size);
    std::iota(rows.begin(), rows.end(), 0); std::iota(cols.begin(), cols.end(), 0);
    row_match.assign(size, -1); col_match.assign(size, -1);
    u.assign(size, 0); v.assign(size, 0);
    last = p.n;
    for (int r : rows) {
        if (deadline.expired()) return BoundStatus::cancelled;
        Cost minimum = INF;
        for (int c : cols) minimum = std::min(minimum, cost(p, r, c, visited));
        if (minimum == INF) return BoundStatus::infeasible;
        u[r] = minimum;
    }
    for (int c : cols) {
        Cost minimum = INF;
        for (int r : rows) {
            const Cost a = cost(p, r, c, visited);
            if (a != INF) minimum = std::min(minimum, a - u[r]);
        }
        if (minimum == INF) return BoundStatus::infeasible;
        v[c] = minimum;
    }
    for (int r : rows) for (int c : cols)
        if (col_match[c] < 0 && cost(p, r, c, visited) != INF && cost(p, r, c, visited) == u[r] + v[c]) {
            row_match[r] = c; col_match[c] = r; break;
        }
    return repair(p, visited, scratch, deadline);
}

BoundStatus Assignment::extend(const Instance& p, int next, const Bits& visited, AssignmentScratch& scratch, const Deadline& deadline) {
    const int old_last = last;
    const int old_col = row_match[old_last], old_row = col_match[next];
    if (old_col < 0 || old_row < 0) throw std::logic_error("Incomplete parent matching");
    row_match[old_last] = -1; col_match[old_col] = -1;
    col_match[next] = -1;
    if (old_row != old_last) row_match[old_row] = -1;
    rows.erase(std::find(rows.begin(), rows.end(), old_last));
    cols.erase(std::find(cols.begin(), cols.end(), next));
    last = next;
    const int current_match = row_match[last];
    if (current_match >= 0 && cost(p, last, current_match, visited) == INF) {
        row_match[last] = -1; col_match[current_match] = -1;
    }
    return repair(p, visited, scratch, deadline);
}

BoundStatus Assignment::repair(const Instance& p, const Bits& visited, AssignmentScratch& s, const Deadline& deadline) {
    for (int r : rows) if (row_match[r] < 0) {
        const auto status = augment(p, r, visited, s, deadline);
        if (status != BoundStatus::feasible) return status;
    }
    value = 0;
    for (int r : rows) value += p.edge(r, row_match[r]);
    return deadline.expired() ? BoundStatus::cancelled : BoundStatus::feasible;
}

BoundStatus Assignment::augment(const Instance& p, int root, const Bits& visited, AssignmentScratch& s, const Deadline& deadline) {
    std::fill(s.distance.begin(), s.distance.end(), INF);
    std::fill(s.seen.begin(), s.seen.end(), 0);
    // Shortest augmenting path in the graph of nonnegative reduced costs.
    int row = root, terminal = -1;
    Cost distance = 0;
    while (true) {
        if (deadline.expired()) return BoundStatus::cancelled;
        Cost next_distance = INF;
        int next_col = -1;
        for (int c : cols) if (!s.seen[c]) {
            const Cost a = cost(p, row, c, visited);
            if (a != INF) {
                const Cost candidate = distance + a - u[row] - v[c];
                if (candidate < s.distance[c]) { s.distance[c] = candidate; s.parent[c] = row; }
            }
            if (s.distance[c] < next_distance) { next_distance = s.distance[c]; next_col = c; }
        }
        if (next_col < 0) return BoundStatus::infeasible;
        distance = next_distance;
        s.seen[next_col] = 1;
        if (col_match[next_col] < 0) { terminal = next_col; break; }
        row = col_match[next_col];
    }
    u[root] += distance;
    for (int c : cols) if (s.seen[c]) {
        const Cost delta = distance - s.distance[c];
        v[c] -= delta;
        if (col_match[c] >= 0) u[col_match[c]] += delta;
    }
    int c = terminal;
    while (true) {
        const int r = s.parent[c];
        const int previous = row_match[r];
        row_match[r] = c; col_match[c] = r;
        if (r == root) break;
        c = previous;
    }
    return BoundStatus::feasible;
}

std::vector<int> Assignment::completion(const Instance& p) const {
    std::vector<int> suffix;
    Bits seen{};
    int at = last;
    for (std::size_t k = 0; k + 1 < rows.size(); ++k) {
        at = row_match[at];
        if (at < 0 || at == p.n || contains(seen, at)) return {};
        insert(seen, at); suffix.push_back(at);
    }
    if (row_match[at] != p.n) return {};
    return suffix;
}

Cost Assignment::connectivity_penalty(const Instance& p, const Bits& visited, const Deadline& deadline) const {
    // The matching is one last-to-dummy path plus disjoint directed cycles.
    // Any Hamiltonian completion must connect these components. Crossing arcs
    // consume nonnegative reduced cost, in addition to the assignment optimum.
    std::vector<int> component(p.n + 1, -1);
    component[p.n] = 0; component[last] = 0;
    int at = row_match[last];
    while (at != p.n) { component[at] = 0; at = row_match[at]; }
    int count = 1;
    for (int r : rows) if (component[r] < 0) {
        at = r;
        do { component[at] = count; at = row_match[at]; } while (at != r);
        ++count;
    }
    if (count == 1) return 0;
    std::vector<Cost> weight(std::size_t(count) * count, INF);
    for (int r : rows) {
        if (deadline.expired()) return 0; // Caller checks cancellation before proof.
        for (int c : cols) if (component[r] != component[c]) {
            const Cost edge = cost(p, r, c, visited);
            if (edge == INF) continue;
            auto& w = weight[std::size_t(component[r]) * count + component[c]];
            w = std::min(w, edge - u[r] - v[c]);
        }
    }
    Cost outgoing = 0, incoming = 0;
    for (int k = 1; k < count; ++k) {
        Cost out = INF, in = INF;
        for (int j = 0; j < count; ++j) if (j != k) {
            out = std::min(out, weight[std::size_t(k) * count + j]);
            in = std::min(in, weight[std::size_t(j) * count + k]);
        }
        if (out == INF || in == INF) return INF;
        outgoing += out; incoming += in;
    }
    // The underlying undirected crossing graph must be connected. Its MST
    // is an additional alternative bound; take a maximum, never add bounds
    // that may charge the same reduced-cost arc more than once.
    std::vector<Cost> key(count, INF);
    std::vector<unsigned char> used(count, 0);
    key[0] = 0;
    Cost tree = 0;
    for (int step = 0; step < count; ++step) {
        int k = -1;
        for (int j = 0; j < count; ++j) if (!used[j] && (k < 0 || key[j] < key[k])) k = j;
        if (k < 0 || key[k] == INF) return INF;
        tree += key[k]; used[k] = 1;
        for (int j = 0; j < count; ++j) if (!used[j])
            key[j] = std::min(key[j], std::min(weight[std::size_t(k) * count + j], weight[std::size_t(j) * count + k]));
    }
    return std::max(tree, std::max(outgoing, incoming));
}
} // namespace sop
