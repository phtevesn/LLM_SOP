#ifndef SOP_NUMA_TOPOLOGY_HPP
#define SOP_NUMA_TOPOLOGY_HPP

#include <vector>

namespace sop {

/**
 * @brief NUMA Topology & Thread Affinity Manager tailored for multi-die architectures
 *        such as the AMD Ryzen Threadripper 2990WX (4 NUMA dies, 32 cores).
 */
struct NumaTopology {
    struct NodeInfo {
        int node_id;
        std::vector<int> cpus;
        bool has_local_memory;
    };

    std::vector<NodeInfo> nodes;
    std::vector<int> thread_to_cpu;
    std::vector<int> thread_to_node;

    void detect(int num_threads);
    void pin_current_thread(int thread_idx) const;
};

} // namespace sop

#endif // SOP_NUMA_TOPOLOGY_HPP