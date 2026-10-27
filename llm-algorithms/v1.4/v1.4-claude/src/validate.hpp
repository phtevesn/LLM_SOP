// validate.hpp - independent verification of a final path against the raw matrix.
#pragma once

#include <string>
#include <vector>

#include "instance.hpp"

namespace sop {

struct Validation {
    bool ok = false;
    int64_t cost = -1;  // recomputed from scratch from the raw matrix
    std::string error;
};

// Checks: path is a permutation of 0..n-1, every precedence T[i][j] < 0 has j before i,
// and recomputes the cost as the sum of T[a][b] over consecutive nodes.
Validation validatePath(const RawInstance& raw, const std::vector<int>& path);

}  // namespace sop
