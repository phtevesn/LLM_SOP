#include "solver.hpp"

#include <algorithm>
#include <atomic>
#include <thread>
#include <unistd.h>

#include "bnb.hpp"
#include "common.hpp"
#include "heuristic.hpp"
#include "incumbent.hpp"
#include "numa.hpp"

namespace sop {

namespace {

size_t defaultMemoBytes() {
    long pages = sysconf(_SC_PHYS_PAGES);
    long psz = sysconf(_SC_PAGE_SIZE);
    double phys = (pages > 0 && psz > 0) ? (double)pages * (double)psz : 8.0 * 1073741824.0;
    double want = std::min(phys / 4.0, 32.0 * 1073741824.0);  // a quarter of RAM, at most 32 GiB
    return (size_t)want;
}

// Runs the ILS on `threads` threads until `endTime`.
void runIlsPhase(const Problem& P, Incumbent& inc, const Topology& topo, int threads, bool pin,
                 uint64_t seed, double endTime, std::atomic<bool>& stop, bool adopt) {
    std::vector<std::thread> ts;
    for (int w = 0; w < threads; ++w) {
        ts.emplace_back([&, w] {
            if (pin && topo.valid && !topo.cpuOrder.empty())
                pinCurrentThread(topo.cpuOrder[(size_t)w % topo.cpuOrder.size()]);
            IlsWorker ils(P, inc, seed + 7919ULL * (uint64_t)(w + 1));
            ils.run(endTime, stop, adopt);
        });
    }
    for (auto& t : ts) t.join();
}

}  // namespace

SolveResult solve(const Problem& P, const SolverOptions& opt) {
    SolveResult out;
    if (!P.feasible) {
        out.infeasibleInstance = true;
        logMsg("instance is infeasible: precedence constraints contain a cycle");
        return out;
    }
    Incumbent inc;
    std::atomic<bool> stop{false};
    const int threads = std::max(1, opt.threads);
    const Topology topo = discoverTopology();
    if (topo.valid)
        logMsg("topology: %zu usable CPUs, %d NUMA node ids (%zu with memory)", topo.cpuOrder.size(),
               topo.numNodes, topo.memNodes.size());

    if (P.n == 1) {
        inc.offer(0, {P.start}, "trivial");
        out.feasibleFound = true;
        out.path = {0};
        out.cost = 0;
        out.optimal = true;
        return out;
    }

    // ---- Phase 1: parallel ILS to obtain a strong initial upper bound ---------------
    double budget = std::min(1.0, 0.05 + 0.0008 * P.n);  // B&B dives find good tours quickly too
    if (P.n <= 12) budget = 0.01;
    double phase1End = nowSeconds() + budget;
    if (opt.timeLimit >= 0) phase1End = std::min(phase1End, nowSeconds() + 0.15 * std::max(0.0, opt.timeLimit - nowSeconds()));
    logMsg("phase 1: iterated local search on %d thread(s) for up to %.2fs", threads, std::max(0.0, phase1End - nowSeconds()));
    if (!opt.noHeuristic) runIlsPhase(P, inc, topo, threads, opt.pin, opt.seed, phase1End, stop, false);
    logMsg("phase 1 done: best cost %lld", inc.hasSolution() ? (long long)inc.cost() : -1LL);

    // ---- Phase 2: branch-and-bound (+ a few helper ILS threads) ---------------------
    const int helpers = (threads >= 8 && !opt.noHeuristic) ? threads / 8 : 0;
    BnbConfig cfg;
    cfg.workers = threads - helpers;
    cfg.cpuOffset = 0;
    cfg.pin = opt.pin;
    cfg.deadline = opt.timeLimit;
    cfg.memoBytes = opt.memoBytes ? opt.memoBytes : defaultMemoBytes();

    std::atomic<bool> helperStop{false};
    std::vector<std::thread> helperThreads;
    for (int h = 0; h < helpers; ++h) {
        helperThreads.emplace_back([&, h] {
            if (opt.pin && topo.valid && !topo.cpuOrder.empty())
                pinCurrentThread(topo.cpuOrder[(size_t)(cfg.workers + h) % topo.cpuOrder.size()]);
            IlsWorker ils(P, inc, opt.seed + 104729ULL * (uint64_t)(h + 1));
            ils.run(-1, helperStop, true);
        });
    }
    logMsg("phase 2: branch-and-bound on %d thread(s)%s", cfg.workers, helpers ? " + ILS helpers" : "");
    BnbResult br = runBranchAndBound(P, inc, topo, cfg, stop);
    helperStop.store(true);
    for (auto& t : helperThreads) t.join();

    logMsg("root assignment lower bound: %lld", (long long)br.rootLowerBound);
    out.nodes = br.nodes;
    if (inc.hasSolution()) {
        out.feasibleFound = true;
        out.path = toRealPath(P, inc.path());
        out.cost = inc.cost();
    }
    out.optimal = br.completed && out.feasibleFound;
    return out;
}

}  // namespace sop
