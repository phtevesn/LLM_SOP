// solver.hpp - orchestration: heuristic phase, parallel branch-and-bound, result assembly.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "instance.hpp"

namespace sop {

struct SolverOptions {
    double timeLimit = -1;    // seconds since program start; < 0: none
    int threads = 1;
    uint64_t seed = 12345;
    size_t memoBytes = 0;     // 0: choose automatically
    bool pin = true;
    bool noHeuristic = false;
};

struct SolveResult {
    bool feasibleFound = false;
    std::vector<int> path;     // real node ids (dummies stripped)
    int64_t cost = -1;
    bool optimal = false;
    uint64_t nodes = 0;
    bool infeasibleInstance = false;
};

SolveResult solve(const Problem& problem, const SolverOptions& opt);

}  // namespace sop
