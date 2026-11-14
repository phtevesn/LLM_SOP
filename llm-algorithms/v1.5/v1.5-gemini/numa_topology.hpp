/*
 * File: numa_topology.hpp
 * Purpose: Declares NUMA topology discovery, asymmetric die scheduling for the
 *          AMD Ryzen Threadripper 2990WX (4 NUMA nodes, 2 memory-controller dies
 *          and 2 compute-only dies), thread-to-core affinity pinning, and
 *          NUMA-local page allocation helpers.
 */

#ifndef SOP_NUMA_TOPOLOGY_HPP
#define SOP_NUMA_TOPOLOGY_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace sop {

struct NumaNodeDescriptor {
    int node_id = 0;
    uint64_t memory_bytes = 0;
    bool has_local_memory = true;
    int nearest_memory_node = 0;
    std::vector<int> cpu_ids;
};

struct WorkerAssignment {
    int worker_id = 0;
    int cpu_id = -1;
    int numa_node = 0;
    int memory_numa_node = 0;
};

class NumaTopology {
public:
    NumaTopology();

    // Discovers system NUMA nodes and CPUs from /sys/devices/system/node
    void discover();

    // Computes optimal worker-to-core assignments for `num_threads` threads,
    // prioritizing memory-bearing NUMA dies (e.g., Nodes 0 and 2 on Threadripper 2990WX)
    // before compute-only dies (Nodes 1 and 3), and grouping threads by CCX/die.
    std::vector<WorkerAssignment> plan_worker_assignments(int num_threads) const;

    // Pins the calling thread to the assigned logical CPU core.
    static bool pin_calling_thread_to_cpu(int cpu_id);

    // Binds a virtual memory region to a specific NUMA node with local memory
    // using the Linux mbind syscall (gracefully no-ops if unsupported).
    static void* allocate_numa_aligned(size_t size_bytes, int memory_numa_node);
    static void free_numa_aligned(void* ptr, size_t size_bytes);

    int num_numa_nodes() const noexcept { return static_cast<int>(nodes_.size()); }
    const std::vector<NumaNodeDescriptor>& nodes() const noexcept { return nodes_; }

private:
    std::vector<NumaNodeDescriptor> nodes_;
    static std::vector<int> parse_cpu_list(const std::string& str);
};

} // namespace sop

#endif // SOP_NUMA_TOPOLOGY_HPP
