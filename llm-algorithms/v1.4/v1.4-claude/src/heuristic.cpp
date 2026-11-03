#include "heuristic.hpp"

#include <algorithm>

#include "common.hpp"

namespace sop {

IlsWorker::IlsWorker(const Problem& p, Incumbent& i, uint64_t seed)
    : P(p), inc(i), n(p.n), rng(seed * 0x9E3779B97F4A7C15ULL + 0x1234567ULL) {
    path.reserve(n);
    pos.assign(n, 0);
    inWork.assign(n, 0);
}

uint64_t IlsWorker::next() {  // xorshift64*
    rng ^= rng >> 12;
    rng ^= rng << 25;
    rng ^= rng >> 27;
    return rng * 0x2545F4914F6CDD1DULL;
}

void IlsWorker::rebuildPos() {
    for (int k = 0; k < n; ++k) pos[path[k]] = k;
}

int64_t IlsWorker::computeCost() const {
    int64_t c = 0;
    for (int k = 0; k + 1 < n; ++k) c += P.c(path[k], path[k + 1]);
    return c;
}

// Randomised greedy construction: nearest ready node, with random deviations.
void IlsWorker::construct() {
    rem = P.predCount;
    path.clear();
    ready.clear();
    auto visit = [&](int v) {
        path.push_back(v);
        for (const int32_t* s = P.hasseSucc.begin(v); s != P.hasseSucc.end(v); ++s)
            if (--rem[*s] == 0) ready.push_back(*s);
    };
    visit(P.start);
    while ((int)path.size() < n) {
        int cur = path.back();
        int pick = -1;
        if (rnd(100) < 25) {
            pick = rnd((int)ready.size());
        } else {
            int64_t best = INT64_MAX;
            for (int k = 0; k < (int)ready.size(); ++k) {
                int64_t c = P.c(cur, ready[k]);
                // light noise breaks ties differently per restart
                c = c * 8 + rnd(8);
                if (c < best) { best = c; pick = k; }
            }
        }
        int v = ready[pick];
        ready[pick] = ready.back();
        ready.pop_back();
        visit(v);
    }
    rebuildPos();
    curCost = computeCost();
}

// Tries to move the segment path[i..j] (no reversal) to its best feasible position.
bool IlsWorker::tryMoveSegment(int i, int j) {
    const int a = path[i - 1], f = path[i], l = path[j], b = path[j + 1];
    const int32_t cab = P.c(a, b);
    if (cab == kForbidden) return false;
    const int64_t base = (int64_t)cab - P.c(a, f) - P.c(l, b);
    int64_t bestDelta = 0;
    int bestP = -1;
    bool bestForward = true;
    // forward: segment goes after node path[p], p >= j+1
    for (int p = j + 1; p <= n - 2; ++p) {
        const int x = path[p];
        bool blocked = false;
        for (int q = i; q <= j; ++q)
            if (P.prec[(size_t)path[q] * n + x]) { blocked = true; break; }
        if (blocked) break;
        const int y = path[p + 1];
        int64_t d = base + (int64_t)P.c(x, f) + P.c(l, y) - P.c(x, y);
        if (d < bestDelta) { bestDelta = d; bestP = p; bestForward = true; }
    }
    // backward: segment goes before node path[p], p <= i-1
    for (int p = i - 1; p >= 1; --p) {
        const int x = path[p];
        bool blocked = false;
        for (int q = i; q <= j; ++q)
            if (P.prec[(size_t)x * n + path[q]]) { blocked = true; break; }
        if (blocked) break;
        const int w = path[p - 1];
        int64_t d = base + (int64_t)P.c(w, f) + P.c(l, x) - P.c(w, x);
        if (d < bestDelta) { bestDelta = d; bestP = p; bestForward = false; }
    }
    if (bestP < 0) return false;
    touch(a); touch(b); touch(f); touch(l);
    if (bestForward) {
        touch(path[bestP]); touch(path[bestP + 1]);
        std::rotate(path.begin() + i, path.begin() + j + 1, path.begin() + bestP + 1);
        for (int k = i; k <= bestP; ++k) pos[path[k]] = k;
    } else {
        touch(path[bestP]); touch(path[bestP - 1]);
        std::rotate(path.begin() + bestP, path.begin() + i, path.begin() + j + 1);
        for (int k = bestP; k <= j; ++k) pos[path[k]] = k;
    }
    curCost += bestDelta;
    return true;
}

void IlsWorker::touchAll() {
    for (int k = n - 1; k >= 0; --k) touch(path[k]);
}

// Or-opt style local search driven by a work list ("don't-look bits"): only nodes near a
// changed edge are re-examined, so an ILS iteration costs far less than a full pass.
bool IlsWorker::localSearch(double endTime, const std::atomic<bool>& stop) {
    if (n < 4) { work.clear(); std::fill(inWork.begin(), inWork.end(), 0); return false; }
    const int maxLen = (n <= 400) ? 8 : 4;
    bool any = false;
    uint32_t pops = 0;
    while (!work.empty()) {
        const int idx = (work.size() > 1) ? rnd((int)work.size()) : 0;
        const int v = work[idx];
        work[idx] = work.back();
        work.pop_back();
        inWork[v] = 0;
        if ((++pops & 255u) == 0) {
            if (stop.load(std::memory_order_relaxed) || (endTime >= 0 && nowSeconds() >= endTime)) {
                for (int u : work) inWork[u] = 0;
                work.clear();
                break;
            }
        }
        const int i = pos[v];
        if (i < 1 || i > n - 2) continue;
        bool moved = false;
        for (int L = 1; L <= maxLen && !moved; ++L) {  // segments starting at v
            const int j = i + L - 1;
            if (j > n - 2) break;
            moved = tryMoveSegment(i, j);
        }
        for (int L = 2; L <= maxLen && !moved; ++L) {  // segments ending at v
            const int i2 = i - L + 1;
            if (i2 < 1) break;
            moved = tryMoveSegment(i2, i);
        }
        if (moved) { any = true; touch(v); }
    }
    return any;
}

// Local double-bridge: swaps two adjacent segments B, C if no b in B must precede a c in C.
bool IlsWorker::kick() {
    if (n < 6) return false;
    const int K = std::max(1, std::min(30, (n - 2) / 3));
    for (int attempt = 0; attempt < 10; ++attempt) {
        const int L1 = 1 + rnd(K), L2 = 1 + rnd(K);
        if (L1 + L2 > n - 2) continue;
        const int i = 1 + rnd(n - 2 - (L1 + L2) + 1);  // i + L1 + L2 - 1 <= n-2
        bool ok = true;
        for (int b = i; b < i + L1 && ok; ++b)
            for (int c = i + L1; c < i + L1 + L2; ++c)
                if (P.prec[(size_t)path[b] * n + path[c]]) { ok = false; break; }
        if (!ok) continue;
        std::rotate(path.begin() + i, path.begin() + i + L1, path.begin() + i + L1 + L2);
        for (int k = i; k < i + L1 + L2; ++k) pos[path[k]] = k;
        touch(path[i - 1]); touch(path[i]); touch(path[i + L2 - 1]); touch(path[i + L2]);
        touch(path[i + L1 + L2 - 1]); touch(path[i + L1 + L2]);
        return true;
    }
    return false;
}

void IlsWorker::offerCurrent(const char* src) {
    if (curCost < inc.cost()) inc.offer(curCost, path, src);
}

void IlsWorker::run(double endTime, const std::atomic<bool>& stop, bool adoptIncumbent) {
    if (n == 1) { path = {P.start}; inc.offer(0, path, "trivial"); return; }
    construct();
    touchAll();
    localSearch(endTime, stop);
    offerCurrent("ILS");
    std::vector<int> savedPath;
    int64_t bestLocal = curCost;
    std::vector<int> bestLocalPath = path;
    int stagnation = 0;
    const int stagLimit = 30 + n;
    while (!stop.load(std::memory_order_relaxed) && (endTime < 0 || nowSeconds() < endTime)) {
        savedPath = path;
        const int64_t savedCost = curCost;
        const int kicks = 1 + rnd(3);
        bool kicked = false;
        for (int k = 0; k < kicks; ++k) kicked |= kick();
        if (!kicked) { ++stagnation; if (n < 6) break; }
        curCost = computeCost();
        localSearch(endTime, stop);
        if (curCost <= savedCost) {
            if (curCost < bestLocal) {
                bestLocal = curCost;
                bestLocalPath = path;
                stagnation = 0;
                offerCurrent("ILS");
            } else {
                ++stagnation;
            }
        } else {
            path = savedPath;
            rebuildPos();
            curCost = savedCost;
            ++stagnation;
        }
        if (stagnation > stagLimit) {
            stagnation = 0;
            if (adoptIncumbent && rnd(2) == 0 && inc.hasSolution()) {
                path = inc.path();
                if ((int)path.size() == n) { rebuildPos(); curCost = computeCost(); }
                else { construct(); touchAll(); }
            } else if (rnd(3) == 0) {
                construct();
                touchAll();
                localSearch(endTime, stop);
            } else {
                path = bestLocalPath;
                rebuildPos();
                curCost = bestLocal;
                for (int k = 0; k < 6; ++k) kick();
                curCost = computeCost();
                localSearch(endTime, stop);
            }
            if (curCost < bestLocal) { bestLocal = curCost; bestLocalPath = path; }
            offerCurrent("ILS");
        }
    }
}

}  // namespace sop
