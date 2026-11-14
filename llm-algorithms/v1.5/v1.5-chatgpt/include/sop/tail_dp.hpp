#pragma once
#include "sop/instance.hpp"

namespace sop {
struct TailResult { bool complete = false; Cost cost = INF; std::vector<int> suffix; };
TailResult solve_tail(const Graph&, const std::vector<std::uint64_t>& visited,
                      int last, Control&, std::uint64_t& count);
} // namespace sop
