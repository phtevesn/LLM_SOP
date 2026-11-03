#include "numa.hpp"

#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

namespace sop {

namespace {

// Parses "0-7,16-23" style lists.
std::vector<int> parseCpuList(const std::string& s) {
    std::vector<int> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        size_t a = tok.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) continue;
        tok = tok.substr(a);
        size_t dash = tok.find('-');
        if (dash == std::string::npos) out.push_back(std::atoi(tok.c_str()));
        else {
            int lo = std::atoi(tok.substr(0, dash).c_str());
            int hi = std::atoi(tok.substr(dash + 1).c_str());
            for (int c = lo; c <= hi; ++c) out.push_back(c);
        }
    }
    return out;
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

}  // namespace

Topology discoverTopology() {
    Topology t;
    // CPUs allowed for this process (respects taskset / cgroups cpusets).
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) return t;
    std::vector<int> cpus;
    for (int c = 0; c < CPU_SETSIZE; ++c)
        if (CPU_ISSET(c, &allowed)) cpus.push_back(c);
    if (cpus.empty()) return t;

    // NUMA node of every cpu.
    std::map<int, int> nodeOf;
    int maxNode = 0;
    bool nodeInfo = false;
    for (int node = 0; node < 256; ++node) {
        std::string s;
        if (!readFile("/sys/devices/system/node/node" + std::to_string(node) + "/cpulist", s)) {
            if (node > 0) break;
            continue;
        }
        nodeInfo = true;
        for (int c : parseCpuList(s)) nodeOf[c] = node;
        maxNode = std::max(maxNode, node);
        std::string mi;
        if (readFile("/sys/devices/system/node/node" + std::to_string(node) + "/meminfo", mi)) {
            size_t p = mi.find("MemTotal:");
            if (p != std::string::npos) {
                long long kb = std::atoll(mi.c_str() + p + 9);
                if (kb > 0) t.memNodes.push_back(node);
            }
        } else {
            t.memNodes.push_back(node);
        }
    }
    t.numNodes = nodeInfo ? maxNode + 1 : 1;

    // Physical-core vs SMT-sibling classification.
    std::vector<int> primary, secondary;
    for (int c : cpus) {
        std::string s;
        bool isPrimary = true;
        if (readFile("/sys/devices/system/cpu/cpu" + std::to_string(c) + "/topology/thread_siblings_list", s)) {
            std::vector<int> sib = parseCpuList(s);
            if (!sib.empty() && *std::min_element(sib.begin(), sib.end()) != c) isPrimary = false;
        }
        (isPrimary ? primary : secondary).push_back(c);
    }

    // Round-robin over NUMA nodes inside each class so that few threads still spread out.
    auto interleave = [&](const std::vector<int>& list, std::vector<int>& out) {
        std::map<int, std::vector<int>> byNode;
        for (int c : list) byNode[nodeOf.count(c) ? nodeOf[c] : 0].push_back(c);
        bool any = true;
        size_t idx = 0;
        while (any) {
            any = false;
            for (auto& kv : byNode)
                if (idx < kv.second.size()) { out.push_back(kv.second[idx]); any = true; }
            ++idx;
        }
    };
    std::vector<int> order;
    interleave(primary, order);
    interleave(secondary, order);
    t.cpuOrder = order;
    for (int c : order) t.nodeOfOrderIdx.push_back(nodeOf.count(c) ? nodeOf[c] : 0);
    t.valid = true;
    return t;
}

bool pinCurrentThread(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}

void adviseInterleaved(void* ptr, size_t len, const std::vector<int>& memNodes) {
#ifdef MADV_HUGEPAGE
    madvise(ptr, len, MADV_HUGEPAGE);
#endif
#ifdef SYS_mbind
    if (memNodes.size() > 1) {
        unsigned long mask[16] = {0};
        unsigned long maxnode = 0;
        for (int nd : memNodes)
            if (nd >= 0 && nd < 1000) {
                mask[nd / 64] |= 1UL << (nd % 64);
                maxnode = std::max<unsigned long>(maxnode, (unsigned long)nd + 2);
            }
        const long MPOL_INTERLEAVE = 3;
        syscall(SYS_mbind, ptr, len, MPOL_INTERLEAVE, mask, maxnode, 0UL);  // errors ignored
    }
#else
    (void)memNodes;
#endif
}

}  // namespace sop
