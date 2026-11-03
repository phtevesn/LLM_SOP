#include "validator.hpp"

namespace sop {

bool Validator::validate(const Instance& inst, const std::vector<int>& path, int64_t reported_cost, std::string& err_msg) {
    int n = inst.n;
    if (static_cast<int>(path.size()) != n) {
        err_msg = "Path length (" + std::to_string(path.size()) + ") != N (" + std::to_string(n) + ")";
        return false;
    }
    std::vector<int> pos(n, -1);
    for (int idx = 0; idx < n; ++idx) {
        int u = path[idx];
        if (u < 0 || u >= n) {
            err_msg = "Node index out of bounds: " + std::to_string(u);
            return false;
        }
        if (pos[u] != -1) {
            err_msg = "Duplicate node in path: " + std::to_string(u);
            return false;
        }
        pos[u] = idx;
    }
    // Check all precedence constraints directly from raw_mat:
    // T[i][j] < 0 means node i depends on node j, so j must come before i (pos[j] < pos[i]).
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            if (i != j && inst.raw_mat[i * n + j] < 0) {
                if (pos[j] >= pos[i]) {
                    err_msg = "Precedence violation: node " + std::to_string(j) +
                              " must precede node " + std::to_string(i);
                    return false;
                }
            }
        }
    }
    // Recompute total objective cost from scratch using raw_mat
    int64_t recomputed = 0;
    for (int idx = 0; idx + 1 < n; ++idx) {
        int u = path[idx];
        int v = path[idx + 1];
        int64_t w = inst.raw_mat[u * n + v];
        if (w < 0) {
            err_msg = "Direct negative edge used in path: (" + std::to_string(u) + " -> " + std::to_string(v) + ")";
            return false;
        }
        recomputed += w;
    }
    if (recomputed != reported_cost) {
        err_msg = "Recomputed cost (" + std::to_string(recomputed) +
                  ") != reported cost (" + std::to_string(reported_cost) + ")";
        return false;
    }
    return true;
}

} // namespace sop