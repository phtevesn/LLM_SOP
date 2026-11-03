#include "numa_topology.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#if defined(__linux__)
#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#endif

namespace sop {

void NumaTopology::detect(int num_threads) {
    nodes.clear();
#if defined(__linux__)
    DIR* dir = opendir("/sys/devices/system/node");
    if (dir != nullptr) {
        struct dirent* entry;
        std::vector<int> node_ids;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name(entry->d_name);
            if (name.rfind("node", 0) == 0 && name.size() > 4 && std::isdigit(name[4])) {
                node_ids.push_back(std::stoi(name.substr(4)));
            }
        }
        closedir(dir);
        std::sort(node_ids.begin(), node_ids.end());

        for (int nid : node_ids) {
            NodeInfo info{nid, {}, true};
            std::ifstream cpulist("/sys/devices/system/node/node" + std::to_string(nid) + "/cpulist");
            if (cpulist) {
                std::string line;
                if (std::getline(cpulist, line)) {
                    std::stringstream ss(line);
                    std::string seg;
                    while (std::getline(ss, seg, ',')) {
                        auto dash = seg.find('-');
                        if (dash == std::string::npos) {
                            if (!seg.empty()) info.cpus.push_back(std::stoi(seg));
                        } else {
                            int a = std::stoi(seg.substr(0, dash));
                            int b = std::stoi(seg.substr(dash + 1));
                            for (int c = a; c <= b; ++c) info.cpus.push_back(c);
                        }
                    }
                }
            }
            std::ifstream meminfo("/sys/devices/system/node/node" + std::to_string(nid) + "/meminfo");
            if (meminfo) {
                std::string mline;
                while (std::getline(meminfo, mline)) {
                    if (mline.find("MemTotal:") != std::string::npos) {
                        std::stringstream mss(mline);
                        std::string tok;
                        long mem_kb = 0;
                        while (mss >> tok) {
                            if (std::isdigit(tok[0])) {
                                mem_kb = std::stol(tok);
                                break;
                            }
                        }
                        info.has_local_memory = (mem_kb > 0);
                        break;
                    }
                }
            }
            if (!info.cpus.empty()) {
                nodes.push_back(std::move(info));
            }
        }
    }
#endif
    if (nodes.empty()) {
        NodeInfo fallback{0, {}, true};
        int hw = static_cast<int>(std::thread::hardware_concurrency());
        if (hw <= 0) hw = 1;
        for (int i = 0; i < hw; ++i) fallback.cpus.push_back(i);
        nodes.push_back(std::move(fallback));
    }

    // Order NUMA nodes so memory-controller-owning nodes (e.g., Threadripper 2990WX Nodes 0 & 2)
    // are scheduled first, followed by compute dies (Nodes 1 & 3), distributing threads round-robin
    // across dies to maximize aggregate L3 cache capacity and memory bandwidth.
    std::vector<size_t> node_order(nodes.size());
    std::iota(node_order.begin(), node_order.end(), 0);
    std::stable_sort(node_order.begin(), node_order.end(), [&](size_t a, size_t b) {
        return nodes[a].has_local_memory > nodes[b].has_local_memory;
    });

    thread_to_cpu.resize(num_threads, -1);
    thread_to_node.resize(num_threads, 0);
    std::vector<size_t> cpu_cursor(nodes.size(), 0);

    for (int t = 0; t < num_threads; ++t) {
        size_t ord_idx = static_cast<size_t>(t) % node_order.size();
        size_t n_idx = node_order[ord_idx];
        const auto& ninfo = nodes[n_idx];
        int cpu = ninfo.cpus[cpu_cursor[n_idx] % ninfo.cpus.size()];
        cpu_cursor[n_idx]++;
        thread_to_cpu[t] = cpu;
        thread_to_node[t] = ninfo.node_id;
    }
}

void NumaTopology::pin_current_thread(int thread_idx) const {
#if defined(__linux__)
    if (thread_idx >= 0 && thread_idx < static_cast<int>(thread_to_cpu.size())) {
        int cpu = thread_to_cpu[thread_idx];
        if (cpu >= 0) {
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(cpu, &cpuset);
            pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
        }
    }
#else
    (void)thread_idx;
#endif
}

} // namespace sop