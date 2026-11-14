/*
 * File: numa_topology.cpp
 * Purpose: Implements NUMA topology discovery and thread/memory placement
 *          tailored for multi-die architectures such as the AMD Ryzen
 *          Threadripper 2990WX (32 cores across 4 Zeppelin dies / 4 NUMA nodes,
 *          where only 2 dies have direct DDR4 memory controllers attached).
 */

#include "numa_topology.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#ifndef MPOL_PREFERRED
#define MPOL_PREFERRED 1
#endif
#endif

namespace sop {

NumaTopology::NumaTopology() {
    discover();
}

/*
 * Algorithm: Sysfs Linux CPU Range List Parser
 * Part: Parses kernel CPU range strings (e.g., "0-7,32-39") into explicit core IDs.
 */
std::vector<int> NumaTopology::parse_cpu_list(const std::string& str) {
    std::vector<int> cpus;
    std::stringstream ss(str);
    std::string segment;
    while (std::getline(ss, segment, ',')) {
        if (segment.empty()) continue;
        auto dash = segment.find('-');
        if (dash == std::string::npos) {
            cpus.push_back(std::stoi(segment));
        } else {
            int start = std::stoi(segment.substr(0, dash));
            int end = std::stoi(segment.substr(dash + 1));
            for (int c = start; c <= end; ++c) {
                cpus.push_back(c);
            }
        }
    }
    return cpus;
}

/*
 * Algorithm: Asymmetric WX NUMA Topology Discovery
 * Part: Queries /sys/devices/system/node/node* to identify per-die CPU lists
 *       and local DRAM capacities, detecting compute-only dies (such as NUMA
 *       nodes 1 and 3 on the AMD Ryzen Threadripper 2990WX) and pairing them
 *       with their nearest memory-bearing die.
 */
void NumaTopology::discover() {
    nodes_.clear();
#ifdef __linux__
    const std::string base_dir = "/sys/devices/system/node";
    if (std::filesystem::exists(base_dir)) {
        for (int nid = 0; nid < 64; ++nid) {
            std::string node_dir = base_dir + "/node" + std::to_string(nid);
            if (!std::filesystem::exists(node_dir)) continue;

            NumaNodeDescriptor desc;
            desc.node_id = nid;

            std::ifstream cpu_in(node_dir + "/cpulist");
            if (cpu_in) {
                std::string line;
                std::getline(cpu_in, line);
                desc.cpu_ids = parse_cpu_list(line);
            }

            std::ifstream mem_in(node_dir + "/meminfo");
            if (mem_in) {
                std::string line;
                while (std::getline(mem_in, line)) {
                    if (line.find("MemTotal:") != std::string::npos) {
                        std::stringstream ls(line);
                        std::string tok;
                        uint64_t kb = 0;
                        while (ls >> tok) {
                            if (!tok.empty() && std::isdigit(static_cast<unsigned char>(tok[0]))) {
                                kb = std::stoull(tok);
                                break;
                            }
                        }
                        desc.memory_bytes = kb * 1024ULL;
                        break;
                    }
                }
            }

            desc.has_local_memory = (desc.memory_bytes > (64ULL << 20));
            desc.nearest_memory_node = nid;
            if (!desc.cpu_ids.empty()) {
                nodes_.push_back(std::move(desc));
            }
        }
    }
#endif

    if (nodes_.empty()) {
        NumaNodeDescriptor fallback;
        fallback.node_id = 0;
        fallback.has_local_memory = true;
        fallback.nearest_memory_node = 0;
        unsigned int hw = std::thread::hardware_concurrency();
        if (hw == 0) hw = 1;
        for (unsigned int i = 0; i < hw; ++i) {
            fallback.cpu_ids.push_back(static_cast<int>(i));
        }
        nodes_.push_back(std::move(fallback));
    }

    std::vector<int> mem_nodes;
    for (const auto& n : nodes_) {
        if (n.has_local_memory) mem_nodes.push_back(n.node_id);
    }
    if (!mem_nodes.empty()) {
        for (auto& n : nodes_) {
            if (!n.has_local_memory) {
                int best_m = mem_nodes[0];
                int best_dist = std::abs(n.node_id - best_m);
                for (int m : mem_nodes) {
                    int d = std::abs(n.node_id - m);
                    if (d < best_dist) {
                        best_dist = d;
                        best_m = m;
                    }
                }
                n.nearest_memory_node = best_m;
            }
        }
    }
}

/*
 * Algorithm: Dynamic Local Mode / Memory-Die-First Thread Scheduling
 * Part: Orders NUMA nodes so that dies with direct DRAM controllers are populated
 *       before compute-only dies when threads < total cores, while keeping
 *       sibling threads within the same CCX/NUMA node contiguous for work stealing.
 */
std::vector<WorkerAssignment> NumaTopology::plan_worker_assignments(int num_threads) const {
    std::vector<WorkerAssignment> plan;
    if (num_threads <= 0) num_threads = 1;

    std::vector<const NumaNodeDescriptor*> ordered_nodes;
    for (const auto& n : nodes_) ordered_nodes.push_back(&n);
    std::stable_sort(ordered_nodes.begin(), ordered_nodes.end(),
                     [](const NumaNodeDescriptor* a, const NumaNodeDescriptor* b) {
                         if (a->has_local_memory != b->has_local_memory) {
                             return a->has_local_memory > b->has_local_memory;
                         }
                         return a->node_id < b->node_id;
                     });

    std::vector<WorkerAssignment> pool;
    for (const auto* nd : ordered_nodes) {
        for (int cpu : nd->cpu_ids) {
            WorkerAssignment wa;
            wa.cpu_id = cpu;
            wa.numa_node = nd->node_id;
            wa.memory_numa_node = nd->nearest_memory_node;
            pool.push_back(wa);
        }
    }

    for (int t = 0; t < num_threads; ++t) {
        WorkerAssignment wa = pool[t % pool.size()];
        wa.worker_id = t;
        plan.push_back(wa);
    }
    return plan;
}

/*
 * Algorithm: OS Thread Affinity Pinning
 * Part: Binds the calling pthread to a specific hardware CPU core to prevent
 *       cross-die migration across the Infinity Fabric.
 */
bool NumaTopology::pin_calling_thread_to_cpu(int cpu_id) {
#ifdef __linux__
    if (cpu_id < 0) return false;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
#else
    (void)cpu_id;
    return false;
#endif
}

/*
 * Algorithm: NUMA-Aware Page Allocation with mbind and First-Touch Faulting
 * Part: Allocates page-aligned memory via mmap, binds physical pages to the
 *       target memory-hosting NUMA node via SYS_mbind, and faults pages in.
 */
void* NumaTopology::allocate_numa_aligned(size_t size_bytes, int memory_numa_node) {
    if (size_bytes == 0) return nullptr;
#ifdef __linux__
    void* ptr = mmap(nullptr, size_bytes, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        return std::aligned_alloc(64, ((size_bytes + 63) / 64) * 64);
    }
    if (memory_numa_node >= 0 && memory_numa_node < 64) {
        unsigned long nodemask = (1UL << memory_numa_node);
        (void)syscall(SYS_mbind, ptr, size_bytes, MPOL_PREFERRED, &nodemask, 64, 0);
    }
    std::memset(ptr, 0, size_bytes);
    return ptr;
#else
    void* ptr = std::aligned_alloc(64, ((size_bytes + 63) / 64) * 64);
    if (ptr) std::memset(ptr, 0, size_bytes);
    return ptr;
#endif
}

void NumaTopology::free_numa_aligned(void* ptr, size_t size_bytes) {
    if (!ptr || size_bytes == 0) return;
#ifdef __linux__
    munmap(ptr, size_bytes);
#else
    std::free(ptr);
#endif
}

} // namespace sop
