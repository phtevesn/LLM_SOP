// bnb.hpp - parallel branch-and-bound with an incremental assignment-problem
// lower bound and history-based dominance pruning.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "incumbent.hpp"
#include "instance.hpp"
#include "numa.hpp"

namespace sop {

struct BnbConfig {
    int workers = 1;        // number of branch-and-bound threads
    int cpuOffset = 0;      // first index into Topology::cpuOrder used for pinning
    bool pin = true;        // pin threads to CPUs / replicate data per NUMA node
    double deadline = -1;   // absolute time (nowSeconds() scale); < 0: none
    size_t memoBytes = 0;   // size of the dominance table (0 disables it)
};

struct BnbResult {
    bool completed = false;     // search space exhausted before the deadline => incumbent optimal
    uint64_t nodes = 0;         // enumerated nodes, summed over all workers
    int64_t rootLowerBound = 0; // AP bound at the root
    bool memoUsed = false;
};

// Searches for a path cheaper than the incumbent. If it completes, the final incumbent
// (possibly unchanged) is proven optimal. `stop` may be raised externally; it is also raised
// internally when the deadline passes.
BnbResult runBranchAndBound(const Problem& problem, Incumbent& incumbent, const Topology& topo,
                            const BnbConfig& cfg, std::atomic<bool>& stop);

}  // namespace sop
