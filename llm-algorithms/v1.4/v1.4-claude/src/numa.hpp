// numa.hpp - CPU topology discovery, thread pinning and NUMA memory policy.
//
// Everything here is best-effort: on systems without /sys topology information
// (or without permission) the functions silently degrade to "no pinning".
#pragma once

#include <cstddef>
#include <vector>

namespace sop {

struct Topology {
    // CPUs we may run on, ordered so that thread i should be pinned to cpuOrder[i % size]:
    // physical cores first (round-robin over NUMA nodes), SMT siblings afterwards.
    std::vector<int> cpuOrder;
    std::vector<int> nodeOfOrderIdx;  // NUMA node of cpuOrder[i]
    std::vector<int> memNodes;        // NUMA nodes that own memory
    int numNodes = 1;                 // max node id + 1 (>=1)
    bool valid = false;               // discovery succeeded
};

Topology discoverTopology();

// Pins the calling thread to one CPU. Returns false on failure.
bool pinCurrentThread(int cpu);

// Asks the kernel to interleave [ptr, ptr+len) across the given memory nodes and to
// use transparent huge pages. Both are advisory; failures are ignored.
void adviseInterleaved(void* ptr, size_t len, const std::vector<int>& memNodes);

}  // namespace sop
