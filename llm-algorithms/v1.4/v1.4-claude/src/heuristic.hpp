// heuristic.hpp - iterated local search (ILS) producing upper bounds.
#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "incumbent.hpp"
#include "instance.hpp"

namespace sop {

class IlsWorker {
public:
    IlsWorker(const Problem& p, Incumbent& inc, uint64_t seed);
    // Runs until stop is set or nowSeconds() >= endTime (endTime < 0: until stop).
    // If adoptIncumbent is true the worker restarts from the shared best on stagnation.
    void run(double endTime, const std::atomic<bool>& stop, bool adoptIncumbent);

private:
    const Problem& P;
    Incumbent& inc;
    const int n;
    uint64_t rng;
    std::vector<int> path, pos;
    int64_t curCost = 0;
    std::vector<int> rem, ready;
    std::vector<int> work;    // nodes whose neighbourhood must be (re)examined
    std::vector<char> inWork;
    void touch(int v) { if (!inWork[v]) { inWork[v] = 1; work.push_back(v); } }
    void touchAll();

    uint64_t next();
    int rnd(int bound) { return (int)(next() % (uint64_t)bound); }
    void construct();
    int64_t computeCost() const;
    void rebuildPos();
    bool localSearch(double endTime, const std::atomic<bool>& stop);
    bool tryMoveSegment(int i, int j);
    bool kick();
    void offerCurrent(const char* src);
};

}  // namespace sop
