// Integer primal-dual assignment relaxation, reversible single-edge updates,
// assignment-component bounds, and extraction of feasible matching completions.
#include "sop/assignment.hpp"

#include <algorithm>
#include <numeric>
#include <stdexcept>

namespace sop {
Assignment::Assignment(const Graph& graph, Control& control) : graph_(graph), control_(control) {
    const int m = graph.m;
    state_.u.resize(m); state_.v.resize(m);
    state_.row.resize(m, -1); state_.col.resize(m, -1);
    state_.allowed.resize(m, 1);
    distance_.resize(m); way_.resize(m); used_.resize(m);
    stamp_.resize(static_cast<std::size_t>(m) * 6, 0);
    tree_rows_.reserve(m); tree_cols_.reserve(m); columns_.reserve(m);
}
void Assignment::set(int kind, int index, Cost value) {
    Cost old = kind == 0 ? state_.u[index] : kind == 1 ? state_.v[index]
        : kind == 2 ? state_.row[index] : kind == 3 ? state_.col[index]
        : kind == 4 ? state_.frontier : state_.allowed[index];
    if (old == value) return;
    auto& stamp = stamp_[static_cast<std::size_t>(kind) * graph_.m + index];
    if (recording_ && stamp != transaction_) {
        trail_.push_back({kind, index, old}); stamp = transaction_;
    }
    if (kind == 0) state_.u[index] = value;
    else if (kind == 1) state_.v[index] = value;
    else if (kind == 2) state_.row[index] = static_cast<int>(value);
    else if (kind == 3) state_.col[index] = static_cast<int>(value);
    else if (kind == 4) state_.frontier = static_cast<int>(value);
    else state_.allowed[index] = static_cast<unsigned char>(value);
}
void Assignment::rollback(Mark mark) {
    recording_ = false;
    while (trail_.size() > mark.trail) {
        auto c = trail_.back(); trail_.pop_back(); set(c.kind, c.index, c.old);
    }
    recording_ = true; state_.bound = mark.bound;
}
void Assignment::load(const AssignmentState& state) { state_ = state; trail_.clear(); }
Cost Assignment::reduced(int row, int col) const {
    if (row == state_.frontier && !state_.allowed[col]) return INF;
    const Cost c = graph_.cost(row, col);
    return c >= INF ? INF : c - state_.u[row] - state_.v[col];
}

bool Assignment::augment(int root) {
    // Hungarian shortest augmenting path with feasible dual potentials.
    std::fill(distance_.begin(), distance_.end(), INF);
    std::fill(used_.begin(), used_.end(), 0);
    tree_rows_.clear(); tree_cols_.clear(); columns_.clear();
    for (int j = 0; j < graph_.m; ++j) if (state_.col[j] != -2) columns_.push_back(j);
    int row = root;
    while (true) {
        if (control_.stopped()) return false;
        tree_rows_.push_back(row);
        Cost delta = INF;
        int next = -1;
        for (int j : columns_) if (!used_[j]) {
            Cost rc = reduced(row, j);
            if (rc < distance_[j]) { distance_[j] = rc; way_[j] = row; }
            if (distance_[j] < delta || (distance_[j] == delta && next >= 0 &&
                state_.col[j] == -1 && state_.col[next] != -1)) { delta = distance_[j]; next = j; }
        }
        if (next < 0 || delta >= INF) return false;
        if (delta != 0) {
            for (int i : tree_rows_) set(0, i, state_.u[i] + delta);
            for (int j : tree_cols_) set(1, j, state_.v[j] - delta);
            for (int j : columns_) if (!used_[j] && distance_[j] < INF) distance_[j] -= delta;
            state_.bound += delta;
        }
        used_[next] = 1; tree_cols_.push_back(next);
        if (state_.col[next] == -1) {
            // Flip the alternating path; all new matched edges have zero reduced cost.
            int j = next;
            while (j != -1) {
                int i = way_[j], previous = state_.row[i];
                set(2, i, j); set(3, j, i); j = previous;
            }
            return true;
        }
        row = state_.col[next];
    }
}

bool Assignment::initialize() {
    recording_ = false;
    state_.bound = 0;
    std::fill(state_.row.begin(), state_.row.end(), -1);
    std::fill(state_.col.begin(), state_.col.end(), -1);
    std::fill(state_.v.begin(), state_.v.end(), 0);
    // Row/column dual reduction and a greedy zero matching seed Hungarian augmentation.
    for (int i = 0; i < graph_.m; ++i) {
        if (control_.stopped()) { recording_ = true; return false; }
        Cost minimum = INF;
        for (int j = 0; j < graph_.m; ++j) minimum = std::min(minimum, graph_.cost(i, j));
        if (minimum >= INF) { recording_ = true; return false; }
        state_.u[i] = minimum; state_.bound += minimum;
    }
    for (int j = 0; j < graph_.m; ++j) {
        Cost minimum = INF;
        for (int i = 0; i < graph_.m; ++i) if (graph_.cost(i, j) < INF)
            minimum = std::min(minimum, graph_.cost(i, j) - state_.u[i]);
        if (minimum >= INF) { recording_ = true; return false; }
        state_.v[j] = minimum; state_.bound += minimum;
    }
    for (int i = 0; i < graph_.m; ++i) for (int j = 0; j < graph_.m; ++j)
        if (state_.col[j] == -1 && reduced(i, j) == 0) {
            state_.row[i] = j; state_.col[j] = i; break;
        }
    for (int i = 0; i < graph_.m; ++i) if (state_.row[i] == -1 && !augment(i)) {
        recording_ = true; return false;
    }
    recording_ = true; trail_.clear(); return true;
}

bool Assignment::force(int row, int col) {
    ++transaction_;
    // Fix row->col and delete its row and column. At most one augmentation is needed.
    const int old_col = state_.row[row], old_row = state_.col[col];
    if (old_col < 0 || old_row < 0 || graph_.cost(row, col) >= INF)
        throw std::logic_error("invalid assignment restriction");
    state_.bound -= state_.u[row] + state_.v[col];
    set(2, row, -2); set(3, col, -2);
    if (old_col == col) return true;
    set(2, old_row, -1); set(3, old_col, -1);
    return augment(old_row);
}

bool Assignment::restrict_frontier(int last, const std::vector<std::uint64_t>& visited) {
    // Only a ready vertex can immediately follow the current prefix. Removing
    // other outgoing edges strengthens the residual assignment without excluding a path.
    set(4, 0, last);
    bool remaining = false;
    for (int j = 0; j < graph_.n; ++j) {
        bool unvisited = !bit(visited, j); remaining = remaining || unvisited;
        set(5, j, unvisited && graph_.available(j, visited));
    }
    set(5, graph_.n, !remaining);
    const int matched = state_.row[last];
    if (matched < 0) throw std::logic_error("frontier has no assignment match");
    if (state_.allowed[matched]) return true;
    set(2, last, -1); set(3, matched, -1);
    return augment(last);
}

bool Assignment::completion(const std::vector<int>& prefix, std::vector<int>& path) const {
    // A single matching path/cycle is an exact completion only if precedence also holds.
    path = prefix;
    std::vector<std::uint64_t> visited(graph_.words, 0);
    for (int v : prefix) set_bit(visited, v);
    int last = prefix.empty() ? graph_.n : prefix.back();
    while (static_cast<int>(path.size()) < graph_.n) {
        int next = state_.row[last];
        if (next < 0 || next == graph_.n || bit(visited, next) || !graph_.available(next, visited)) return false;
        path.push_back(next); set_bit(visited, next); last = next;
    }
    return state_.row[last] == graph_.n;
}

Cost Assignment::component_extra() {
    // Contract assignment cycles and the residual last-to-dummy path.
    const int m = graph_.m;
    std::vector<int> parent(m), rank(m, 0), labels(m, -1), component(m, -1), rows, cols;
    std::iota(parent.begin(), parent.end(), 0);
    auto root = [&](int a) { while (parent[a] != a) { parent[a] = parent[parent[a]]; a = parent[a]; } return a; };
    for (int i = 0; i < m; ++i) if (state_.row[i] >= 0) {
        rows.push_back(i);
        int a = root(i), b = root(state_.row[i]);
        if (a != b) { if (rank[a] < rank[b]) std::swap(a, b); parent[b] = a; if (rank[a] == rank[b]) ++rank[a]; }
    }
    for (int j = 0; j < m; ++j) if (state_.col[j] >= 0) cols.push_back(j);
    int k = 0;
    for (int i = 0; i < m; ++i) if (state_.row[i] >= 0 || state_.col[i] >= 0) {
        int r = root(i);
        if (labels[r] == -1) labels[r] = k++;
        component[i] = labels[r];
    }
    if (k < 2) return 0;
    std::vector<Cost> cross(static_cast<std::size_t>(k) * k, INF), outgoing(k, INF), incoming(k, INF);
    for (int i : rows) {
        if (control_.stopped()) return 0;
        for (int j : cols) {
            int a = component[i], b = component[j];
            if (a == b) continue;
            Cost r = reduced(i, j);
            auto& edge = cross[static_cast<std::size_t>(a) * k + b];
            edge = std::min(edge, r); outgoing[a] = std::min(outgoing[a], r); incoming[b] = std::min(incoming[b], r);
        }
    }
    Cost out_sum = 0, in_sum = 0;
    for (int c = 0; c < k; ++c) { out_sum = plus(out_sum, outgoing[c]); in_sum = plus(in_sum, incoming[c]); }
    // Prim's undirected MST is a separate reduced-cost connectivity bound.
    std::vector<Cost> distance(k, INF);
    std::vector<unsigned char> used(k, 0);
    distance[0] = 0;
    Cost mst = 0;
    for (int step = 0; step < k; ++step) {
        int a = -1;
        for (int b = 0; b < k; ++b) if (!used[b] && (a < 0 || distance[b] < distance[a])) a = b;
        if (a < 0 || distance[a] >= INF) return INF;
        used[a] = 1; mst += distance[a];
        for (int b = 0; b < k; ++b) if (!used[b])
            distance[b] = std::min(distance[b], std::min(cross[static_cast<std::size_t>(a) * k + b],
                                                       cross[static_cast<std::size_t>(b) * k + a]));
    }
    // These three bounds overlap: take their maximum, never their sum.
    return std::max({mst, out_sum, in_sum});
}

void Assignment::check() const {
    Cost primal = 0, dual = 0;
    for (int i = 0; i < graph_.m; ++i) if (state_.row[i] != -2) {
        int j = state_.row[i];
        if (j < 0 || state_.col[j] != i || reduced(i, j) != 0) throw std::logic_error("matching invariant");
        primal += graph_.cost(i, j); dual += state_.u[i];
        for (int c = 0; c < graph_.m; ++c) if (state_.col[c] != -2 && reduced(i, c) < 0)
            throw std::logic_error("assignment dual infeasibility");
    }
    for (int j = 0; j < graph_.m; ++j) if (state_.col[j] != -2) dual += state_.v[j];
    if (primal != dual || primal != state_.bound) throw std::logic_error("assignment primal-dual mismatch");
}
} // namespace sop
