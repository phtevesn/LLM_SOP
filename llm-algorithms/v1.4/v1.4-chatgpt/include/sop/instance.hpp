#pragma once
#include "sop/types.hpp"

namespace sop {
struct Instance {
    int n = 0, words = 0;
    std::string name;
    std::vector<Cost> original;
    std::vector<Bits> pred, succ;
    std::vector<std::vector<int>> successors;
    std::vector<int> indegree, sources, sinks;
    // (n+1)^2 adjacency costs, including a zero-cost dummy vertex n.
    // Impossible adjacency arcs are removed using the precedence closure.
    std::vector<Cost> arcs;
    bool acyclic = true;
    Cost edge(int a, int b) const { return arcs[std::size_t(a) * (n + 1) + b]; }
    Cost raw(int a, int b) const { return original[std::size_t(a) * n + b]; }
};

Instance read_instance(const std::string& path);
bool preprocess(Instance& instance, const Deadline& deadline);
Cost validate_path(const Instance& instance, const std::vector<int>& path);
} // namespace sop
