#pragma once
#include "sop/common.hpp"
#include <functional>

namespace sop {
struct Instance {
    int n = 0;
    std::string name;
    std::vector<Cost> matrix;
    Cost at(int i, int j) const { return matrix[static_cast<std::size_t>(i) * n + j]; }
    static Instance read(const std::string& filename);
    Cost validate(const std::vector<int>& path) const;
};

struct Graph {
    int n = 0, m = 0, words = 0;
    bool cyclic = false, unique_order = false, complete = false;
    int symmetry_edges = 0;
    std::vector<Cost> costs;
    std::vector<std::vector<int>> predecessors, successors;
    std::vector<std::uint64_t> pred, succ;
    std::vector<int> sources, sinks, topological;
    std::vector<std::uint64_t> zobrist;
    Cost cost(int i, int j) const { return costs[static_cast<std::size_t>(i) * m + j]; }
    bool before(int i, int j) const {
        return (pred[static_cast<std::size_t>(j) * words + i / 64] >> (i % 64)) & 1U;
    }
    bool available(int v, const std::vector<std::uint64_t>& visited) const;
    static Graph build(const Instance&, Control&,
                       const std::function<void(const std::vector<int>&)>& on_order = {});
};
} // namespace sop
