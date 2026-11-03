#pragma once
#include "sop/instance.hpp"

namespace sop {
enum class BoundStatus { feasible, infeasible, cancelled };

struct AssignmentScratch {
    std::vector<Cost> distance;
    std::vector<int> parent;
    std::vector<unsigned char> seen;
    explicit AssignmentScratch(int size) : distance(size), parent(size), seen(size) {}
};

// Integral minimum-cost perfect matching, with feasible row/column duals.
// Fixing an arc removes one row/column. A warm update needs at most two
// augmentations: one for the deletion and one for new frontier restrictions.
struct Assignment {
    std::vector<int> rows, cols, row_match, col_match;
    std::vector<Cost> u, v;
    Cost value = 0;
    int last = -1;

    BoundStatus initialize(const Instance&, const Bits&, AssignmentScratch&, const Deadline&);
    BoundStatus extend(const Instance&, int next, const Bits&, AssignmentScratch&, const Deadline&);
    Cost reduced_cost(const Instance& p, int next) const { return p.edge(last, next) - u[last] - v[next]; }
    std::vector<int> completion(const Instance&) const;
    Cost connectivity_penalty(const Instance&, const Bits&, const Deadline&) const;
private:
    Cost cost(const Instance&, int row, int col, const Bits&) const;
    BoundStatus repair(const Instance&, const Bits&, AssignmentScratch&, const Deadline&);
    BoundStatus augment(const Instance&, int row, const Bits&, AssignmentScratch&, const Deadline&);
};
} // namespace sop
