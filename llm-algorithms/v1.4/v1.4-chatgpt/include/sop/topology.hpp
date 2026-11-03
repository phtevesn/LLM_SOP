#pragma once
#include <vector>

namespace sop {
struct Placement { int cpu = -1, node = -1; };
std::vector<Placement> discover_placement(unsigned workers);
bool bind_worker(int cpu);
} // namespace sop
