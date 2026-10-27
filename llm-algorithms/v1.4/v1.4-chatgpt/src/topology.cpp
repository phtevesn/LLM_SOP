#include "sop/topology.hpp"
#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <tuple>
#ifdef __linux__
#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#endif

namespace sop {
std::vector<Placement> discover_placement(unsigned workers) {
    std::vector<Placement> placements;
#ifdef __linux__
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) == 0) {
        struct Cpu { int id, node, core, socket; };
        std::map<int, std::vector<Cpu>> groups;
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) if (CPU_ISSET(cpu, &allowed)) {
            const std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(cpu);
            int core = cpu, socket = 0, node = 0;
            std::ifstream(base + "/topology/core_id") >> core;
            std::ifstream(base + "/topology/physical_package_id") >> socket;
            if (DIR* dir = opendir(base.c_str())) {
                while (auto* entry = readdir(dir)) {
                    const std::string name(entry->d_name);
                    if (name.size() > 4 && name.substr(0, 4) == "node" &&
                        name.find_first_not_of("0123456789", 4) == std::string::npos) {
                        node = std::stoi(name.substr(4)); break;
                    }
                }
                closedir(dir);
            }
            groups[node].push_back({cpu, node, core, socket});
        }
        std::set<std::pair<int, int>> used_cores;
        std::vector<Placement> first, siblings;
        // Round-robin NUMA nodes, taking one hardware thread per physical core
        // before SMT siblings. CPU numbers and memory-bearing nodes are not assumed.
        std::size_t index = 0;
        bool more = true;
        while (more) {
            more = false;
            for (const auto& group : groups) if (index < group.second.size()) {
                more = true;
                const auto c = group.second[index];
                auto& output = used_cores.insert({c.socket, c.core}).second ? first : siblings;
                output.push_back({c.id, c.node});
            }
            ++index;
        }
        first.insert(first.end(), siblings.begin(), siblings.end());
        for (unsigned i = 0; i < workers && i < first.size(); ++i) placements.push_back(first[i]);
    }
#endif
    placements.resize(workers); // surplus workers are left unpinned
    return placements;
}

bool bind_worker(int cpu) {
#ifdef __linux__
    if (cpu >= 0 && cpu < CPU_SETSIZE) {
        cpu_set_t set;
        CPU_ZERO(&set); CPU_SET(cpu, &set);
        return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
    }
#else
    (void)cpu;
#endif
    return false;
}
} // namespace sop
