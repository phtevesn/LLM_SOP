#pragma once
#include "sop/instance.hpp"

namespace sop {
struct AssignmentState {
    std::vector<Cost> u, v;
    std::vector<int> row, col; // -1: unmatched; -2: deleted from residual problem.
    int frontier = -1;
    std::vector<unsigned char> allowed;
    Cost bound = 0;
};

class Assignment {
public:
    struct Mark { std::size_t trail; Cost bound; };
    Assignment(const Graph& graph, Control& control);
    bool initialize();
    bool force(int row, int col);
    bool restrict_frontier(int last, const std::vector<std::uint64_t>& visited);
    Mark mark() const { return {trail_.size(), state_.bound}; }
    void rollback(Mark mark);
    void load(const AssignmentState& state);
    const AssignmentState& state() const { return state_; }
    Cost bound() const { return state_.bound; }
    Cost reduced(int row, int col) const;
    Cost component_extra();
    bool completion(const std::vector<int>& prefix, std::vector<int>& path) const;
    void check() const; // Expensive invariant checker used by the tests.
private:
    struct Change { int kind, index; Cost old; };
    const Graph& graph_;
    Control& control_;
    AssignmentState state_;
    std::vector<Change> trail_;
    std::vector<Cost> distance_;
    std::vector<int> way_, tree_rows_, tree_cols_, columns_;
    std::vector<unsigned char> used_;
    std::vector<std::uint64_t> stamp_;
    std::uint64_t transaction_ = 0;
    bool recording_ = true;
    void set(int kind, int index, Cost value);
    bool augment(int root);
};
} // namespace sop
