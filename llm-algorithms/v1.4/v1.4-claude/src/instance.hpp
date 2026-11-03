// instance.hpp - .sop parsing and the preprocessed "Problem" used by solvers.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sop {

// Raw content of a .sop file: the N x N matrix exactly as given.
struct RawInstance {
    std::string name;        // NAME header (informational)
    int n = 0;               // DIMENSION
    std::vector<int64_t> T;  // row-major n*n matrix
    int64_t at(int i, int j) const { return T[(size_t)i * n + j]; }
};

RawInstance parseSopFile(const std::string& path);

// Compressed sparse row adjacency.
struct Csr {
    std::vector<int32_t> off;  // size n+1
    std::vector<int32_t> adj;
    const int32_t* begin(int v) const { return adj.data() + off[v]; }
    const int32_t* end(int v) const { return adj.data() + off[v + 1]; }
    int degree(int v) const { return off[v + 1] - off[v]; }
};

// Preprocessed problem. Internal nodes are 0..n-1; nodes >= nReal are dummy
// source / sink nodes added when the precedence order has several minimal /
// maximal elements. A feasible solution is a Hamiltonian path start -> end.
struct Problem {
    int n = 0;
    int nReal = 0;
    int start = -1;
    int end = -1;
    bool feasible = true;  // false if the precedence graph has a cycle
    bool addedSource = false;
    bool addedSink = false;
    // cost[i*n+j]: cost of arc i->j, or kForbidden if the arc can never be
    // used in a feasible path.
    std::vector<int32_t> cost;
    // prec[a*n+b] == 1  <=>  a must come before b (transitive closure).
    std::vector<uint8_t> prec;
    Csr hasseSucc;  // covering relation (transitive reduction), successors
    Csr hassePred;  // covering relation, predecessors
    std::vector<int32_t> predCount;  // |hassePred[v]|
    int64_t maxArc = 0;              // largest finite arc cost
    int32_t c(int i, int j) const { return cost[(size_t)i * n + j]; }
    bool before(int a, int b) const { return prec[(size_t)a * n + b] != 0; }
};

Problem buildProblem(const RawInstance& raw);

// Strip dummy nodes from an internal path.
std::vector<int> toRealPath(const Problem& p, const std::vector<int>& internalPath);

}  // namespace sop
