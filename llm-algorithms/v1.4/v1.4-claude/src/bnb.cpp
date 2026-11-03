#include "bnb.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include "common.hpp"
#include "memo.hpp"

namespace sop {

namespace {

constexpr int kMemoMinRemaining = 5;     // do not memoise states with fewer unvisited nodes
constexpr int kDonateMinRemaining = 10;  // do not split subtrees with fewer unvisited nodes

struct Task {
    std::vector<int> prefix;  // path from the start node down to the frame node
    std::vector<int> cands;   // children of the frame node still to explore (ordered)
    bool root = false;
};

struct RootAP {
    std::vector<int64_t> U, V;
    std::vector<int16_t> pc;
    int64_t obj = 0;
    bool infeasible = false;
};

struct Shared {
    const Problem* master = nullptr;
    Incumbent* inc = nullptr;
    MemoTable* memo = nullptr;
    const Topology* topo = nullptr;
    const BnbConfig* cfg = nullptr;
    std::atomic<bool>* stop = nullptr;

    std::mutex mtx;
    std::condition_variable cv;     // wakes idle workers
    std::condition_variable supCv;  // wakes the supervisor (completion)
    std::deque<Task> pool;
    int busy = 0;
    bool done = false;
    std::atomic<int> idle{0};
    std::atomic<int> poolSize{0};

    std::once_flag rootOnce;
    RootAP root;

    std::atomic<uint64_t> nodesPublished{0};
    std::atomic<uint64_t> nodesTotal{0};

    std::vector<std::unique_ptr<Problem>> replicas;
    std::unique_ptr<std::once_flag[]> replicaOnce;

    std::vector<uint64_t> zs1, zs2, zl1, zl2;  // Zobrist keys (set / last node)

    bool getTask(Task& t) {
        std::unique_lock<std::mutex> lk(mtx);
        for (;;) {
            if (stop->load(std::memory_order_relaxed)) return false;
            if (!pool.empty()) {
                t = std::move(pool.front());
                pool.pop_front();
                poolSize.store((int)pool.size(), std::memory_order_relaxed);
                ++busy;
                return true;
            }
            if (busy == 0) {
                done = true;
                cv.notify_all();
                supCv.notify_all();
                return false;
            }
            idle.fetch_add(1, std::memory_order_relaxed);
            cv.wait(lk);
            idle.fetch_sub(1, std::memory_order_relaxed);
        }
    }
    void finishTask() {
        std::lock_guard<std::mutex> lk(mtx);
        --busy;
        if (busy == 0 && pool.empty()) {
            done = true;
            cv.notify_all();
            supCv.notify_all();
        }
    }
    void push(Task&& t) {
        {
            std::lock_guard<std::mutex> lk(mtx);
            pool.push_back(std::move(t));
            poolSize.store((int)pool.size(), std::memory_order_relaxed);
        }
        cv.notify_one();
    }
};

uint64_t splitmix(uint64_t& x) {
    uint64_t z = (x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

struct Frame {
    int node = 0;
    int idx = 0, cnt = 0;
    int savedPos = 0;
    int acnt = 0;  // number of unvisited nodes at this node
    int64_t g = 0;
    int64_t obj = 0;
    uint64_t sh1 = 0, sh2 = 0;  // hash of the visited set
};

enum class Status { Pruned, Done, Open };

class Worker {
public:
    Worker(Shared& s, const Problem& p) : S(s), P(p), n(p.n), tEnd(p.end), sStart(p.start) {
        const size_t stride = (size_t)n + 1;
        Ustk.assign((size_t)n * stride, 0);
        Vstk.assign((size_t)n * stride, 0);
        Pstk.assign((size_t)n * stride, -1);
        candStore.assign((size_t)n * n, 0);
        fr.assign(n + 1, Frame());
        path.assign(n + 1, 0);
        rem.assign(n, 0);
        inR.assign(n, 0);
        alist.assign(n, 0);
        apos.assign(n, 0);
        minv.assign(stride, kInf);
        way.assign(stride, 0);
        used.assign(stride, 0);
        ulist.assign(stride, 0);
        prow.assign(stride, -1);
        seq.assign(n, 0);
        seqPos.assign(n, 0);
        tmp.reserve(n);
    }

    uint64_t nodes = 0;

    void computeRootAP();
    void runTask(Task& t);
    void flushNodes() {
        S.nodesPublished.fetch_add(nodes - published, std::memory_order_relaxed);
        published = nodes;
    }

private:
    Shared& S;
    const Problem& P;
    const int n, tEnd, sStart;
    uint64_t published = 0;
    uint32_t tick = 0;
    bool halted = false;
    int base = 0;

    std::vector<int64_t> Ustk, Vstk;
    std::vector<int16_t> Pstk;
    std::vector<int16_t> candStore;
    std::vector<Frame> fr;
    std::vector<int> path;
    std::vector<int32_t> rem;
    std::vector<uint8_t> inR;
    std::vector<int> alist, apos;
    int acount = 0;
    std::vector<int64_t> minv;
    std::vector<int16_t> way;
    std::vector<uint8_t> used;
    std::vector<int> ulist;
    std::vector<int16_t> prow;
    std::vector<int> seq, seqPos;
    std::vector<std::pair<int64_t, int>> tmp;
    int64_t* Ud(int d) { return Ustk.data() + (size_t)d * ((size_t)n + 1); }
    int64_t* Vd(int d) { return Vstk.data() + (size_t)d * ((size_t)n + 1); }
    int16_t* Pd(int d) { return Pstk.data() + (size_t)d * ((size_t)n + 1); }
    int16_t* cand(int d) { return candStore.data() + (size_t)d * n; }

    void loadRootState();
    void loadRoot();
    void removeFromAlist(int j, int& savedPos) {
        const int p = apos[j];
        savedPos = p;
        const int lastIdx = acount - 1;
        const int z = alist[lastIdx];
        alist[p] = z;
        apos[z] = p;
        alist[lastIdx] = j;
        apos[j] = lastIdx;
        --acount;
    }
    void restoreToAlist(int j, int p) {
        ++acount;
        const int lastIdx = acount - 1;
        const int z = alist[p];
        alist[p] = j;
        apos[j] = p;
        alist[lastIdx] = z;
        apos[z] = lastIdx;
    }
    bool augment(int r, int64_t* U, int64_t* V, int16_t* pc, int last, int64_t gBase, int64_t ub,
                 int64_t& obj);
    Status enterChild(int d, int j, bool replay);
    void leaveChild(int d, int j);
    bool tryCompletion(int d1);
    void generateCands(int d);
    void expand(int d);
    void maybeDonate(int d);
    void offerPath(int upto, int64_t cost, const char* src);
};

// ---------------------------------------------------------------------------------------
// Assignment-problem machinery.
//
// For a partial path ending at `last` with unvisited set R (t = end node in R) we solve
// the assignment problem on rows/cols C = R + {last} with arcs
//   last -> j   for ready j in R,       i -> j for i,j in R (statically feasible arcs),
//   t -> last   with cost 0 (closing the cycle).
// Every feasible completion is a perfect matching, so the optimum is a lower bound.
// Duals (U rows, V cols) with U[i] + V[j] <= cost(i,j) are kept per depth, and a child
// state is warm-started from its parent (see enterChild), so only one or two augmenting
// path searches are needed per node.
// ---------------------------------------------------------------------------------------

bool Worker::augment(int r, int64_t* U, int64_t* V, int16_t* pc, int last, int64_t gBase,
                     int64_t ub, int64_t& obj) {
    const int root = n;
    const int t = tEnd;
    const int32_t* C = P.cost.data();
    const int m = acount;
    const int* al = alist.data();
    int64_t* mv_ = minv.data();
    uint8_t* us = used.data();
    int16_t* wy = way.data();
    for (int k = 0; k < m; ++k) {
        const int j = al[k];
        mv_[j] = kInf;
        us[j] = 0;
    }
    mv_[last] = kInf;
    us[last] = 0;
    us[root] = 0;
    pc[root] = (int16_t)r;
    int j0 = root;
    int ucnt = 0;
    for (;;) {
        us[j0] = 1;
        ulist[ucnt++] = j0;
        const int i0 = pc[j0];
        int64_t delta = kInf;
        int j1 = -1;
        if (i0 == t) {
            if (!us[last]) {
                const int64_t cur = -U[t] - V[last];
                if (cur < mv_[last]) { mv_[last] = cur; wy[last] = (int16_t)j0; }
            }
            for (int k = 0; k < m; ++k) {
                const int j = al[k];
                if (us[j]) continue;
                if (mv_[j] < delta) { delta = mv_[j]; j1 = j; }
            }
        } else {
            const int32_t* rc = C + (size_t)i0 * n;
            const int64_t ui = U[i0];
            if (i0 == last) {
                const int32_t* rm = rem.data();
                for (int k = 0; k < m; ++k) {
                    const int j = al[k];
                    if (us[j]) continue;
                    int64_t mv = mv_[j];
                    const int32_t cc = rc[j];
                    if (rm[j] == 0 && cc != kForbidden) {
                        const int64_t cur = (int64_t)cc - ui - V[j];
                        if (cur < mv) { mv = cur; mv_[j] = cur; wy[j] = (int16_t)j0; }
                    }
                    if (mv < delta) { delta = mv; j1 = j; }
                }
            } else {
                for (int k = 0; k < m; ++k) {
                    const int j = al[k];
                    if (us[j]) continue;
                    int64_t mv = mv_[j];
                    const int32_t cc = rc[j];
                    if (cc != kForbidden) {
                        const int64_t cur = (int64_t)cc - ui - V[j];
                        if (cur < mv) { mv = cur; mv_[j] = cur; wy[j] = (int16_t)j0; }
                    }
                    if (mv < delta) { delta = mv; j1 = j; }
                }
            }
        }
        if (!us[last] && mv_[last] < delta) { delta = mv_[last]; j1 = last; }
        if (j1 < 0 || delta >= kInf / 2) return false;  // no perfect matching: infeasible
        for (int q = 0; q < ucnt; ++q) {
            const int w = ulist[q];
            U[pc[w]] += delta;
            if (w != root) V[w] -= delta;
        }
        for (int k = 0; k < m; ++k) {
            const int j = al[k];
            if (!us[j] && mv_[j] < kInf) mv_[j] -= delta;
        }
        if (!us[last] && mv_[last] < kInf) mv_[last] -= delta;
        obj += delta;  // dual objective is monotone: usable for early pruning
        if (gBase + obj >= ub) return false;
        j0 = j1;
        if (pc[j0] < 0) break;
    }
    do {
        const int j1 = wy[j0];
        pc[j0] = pc[j1];
        j0 = j1;
    } while (j0 != root);
    return true;
}

void Worker::loadRootState() {
    for (int i = 0; i < n; ++i) { rem[i] = P.predCount[i]; inR[i] = 1; }
    acount = 0;
    for (int i = 0; i < n; ++i)
        if (i != sStart) { alist[acount] = i; apos[i] = acount; ++acount; }
    inR[sStart] = 0;
    for (const int32_t* s = P.hasseSucc.begin(sStart); s != P.hasseSucc.end(sStart); ++s) --rem[*s];
    path[0] = sStart;
}

void Worker::loadRoot() {
    loadRootState();
    const size_t stride = (size_t)n + 1;
    std::memcpy(Ud(0), S.root.U.data(), stride * sizeof(int64_t));
    std::memcpy(Vd(0), S.root.V.data(), stride * sizeof(int64_t));
    std::memcpy(Pd(0), S.root.pc.data(), stride * sizeof(int16_t));
    Frame& f = fr[0];
    f.node = sStart;
    f.g = 0;
    f.obj = S.root.obj;
    f.sh1 = S.zs1[sStart];
    f.sh2 = S.zs2[sStart];
    f.acnt = acount;
    f.idx = f.cnt = 0;
}

void Worker::computeRootAP() {
    loadRootState();
    RootAP& R = S.root;
    const size_t stride = (size_t)n + 1;
    R.U.assign(stride, 0);
    R.V.assign(stride, 0);
    R.pc.assign(stride, -1);
    int64_t* U = R.U.data();
    int64_t* V = R.V.data();
    int16_t* pc = R.pc.data();
    const int t = tEnd, last = sStart;
    const int32_t* C = P.cost.data();
    // Column reduction: V[j] = min_i arc(i, j); matched where the arg-min row is still free.
    std::vector<int> colOfRow(n, -1);
    V[last] = 0;
    pc[last] = (int16_t)t;
    colOfRow[t] = last;
    for (int k = 0; k < acount; ++k) {
        const int j = alist[k];
        int64_t best = kInf;
        int bi = -1;
        // row `last`
        if (rem[j] == 0 && C[(size_t)last * n + j] != kForbidden) { best = C[(size_t)last * n + j]; bi = last; }
        for (int q = 0; q < acount; ++q) {
            const int i = alist[q];
            if (i == j || i == t) continue;
            const int32_t cc = C[(size_t)i * n + j];
            if (cc != kForbidden && cc < best) { best = cc; bi = i; }
        }
        if (bi < 0) { R.infeasible = true; return; }
        V[j] = best;
        if (colOfRow[bi] < 0) { colOfRow[bi] = j; pc[j] = (int16_t)bi; }
    }
    int64_t obj = 0;
    for (int k = 0; k < acount; ++k) obj += V[alist[k]];
    obj += V[last];
    std::vector<int> rows(alist.begin(), alist.begin() + acount);
    rows.push_back(last);
    for (int i : rows) {
        if (colOfRow[i] >= 0) continue;
        if (!augment(i, U, V, pc, last, 0, kInf, obj)) { R.infeasible = true; return; }
    }
    obj = 0;
    for (int i : rows) obj += U[i];
    for (int k = 0; k < acount; ++k) obj += V[alist[k]];
    obj += V[last];
    R.obj = obj;
}

// Creates the child state at depth d+1 by moving from the node at depth d to node j.
Status Worker::enterChild(int d, int j, bool replay) {
    Frame& pf = fr[d];
    Frame& cf = fr[d + 1];
    const int a0 = pf.node;
    const int t = tEnd;
    const int32_t* C = P.cost.data();
    if (!replay) ++nodes;
    const int64_t ub = S.inc->cost();

    // Parent's matching, row -> col view (before alist changes).
    int16_t* pc0 = Pd(d);
    for (int k = 0; k < acount; ++k) {
        const int col = alist[k];
        prow[pc0[col]] = (int16_t)col;
    }
    prow[pc0[a0]] = (int16_t)a0;  // col a0 is matched to row t
    const int x = prow[a0];       // column matched to row a0
    const int r = pc0[j];         // row matched to column j
    const int b = prow[j];        // column matched to row j

    // --- state change -------------------------------------------------------------
    removeFromAlist(j, cf.savedPos);
    inR[j] = 0;
    for (const int32_t* s = P.hasseSucc.begin(j); s != P.hasseSucc.end(j); ++s) --rem[*s];
    path[d + 1] = j;
    cf.node = j;
    cf.g = pf.g + C[(size_t)a0 * n + j];
    cf.sh1 = pf.sh1 ^ S.zs1[j];
    cf.sh2 = pf.sh2 ^ S.zs2[j];
    cf.acnt = acount;
    cf.idx = cf.cnt = 0;

    // --- warm start of the assignment duals -----------------------------------------
    // Child problem = parent minus (row a0, col a0); column j now only receives the
    // closing arc t -> j; row j only keeps arcs to nodes that became ready.
    const size_t stride = (size_t)n + 1;
    int64_t* U1 = Ud(d + 1);
    int64_t* V1 = Vd(d + 1);
    int16_t* pc1 = Pd(d + 1);
    std::memcpy(U1, Ud(d), stride * sizeof(int64_t));
    std::memcpy(V1, Vd(d), stride * sizeof(int64_t));
    std::memcpy(pc1, pc0, stride * sizeof(int16_t));
    int ur[2];
    int nu = 0;
    pc1[x] = -1;
    if (r != a0) ur[nu++] = r;
    pc1[j] = (int16_t)t;
    V1[j] = -U1[t];
    const bool bValid = inR[b] && rem[b] == 0 && C[(size_t)j * n + b] != kForbidden;
    if (!bValid) {
        pc1[b] = -1;
        ur[nu++] = j;
    }
    int64_t obj = U1[j] + V1[j];
    for (int k = 0; k < acount; ++k) {
        const int col = alist[k];
        obj += U1[col] + V1[col];
    }
    if (cf.g + obj >= ub) return Status::Pruned;
    for (int q = 0; q < nu; ++q)
        if (!augment(ur[q], U1, V1, pc1, j, cf.g, ub, obj)) return Status::Pruned;
    cf.obj = obj;
    if (!replay && tryCompletion(d + 1)) return Status::Done;
    return Status::Open;
}

void Worker::leaveChild(int d, int j) {
    for (const int32_t* s = P.hasseSucc.begin(j); s != P.hasseSucc.end(j); ++s) ++rem[*s];
    restoreToAlist(j, fr[d + 1].savedPos);
    inR[j] = 1;
}

// If the optimal assignment is a single cycle that respects all precedences, it is an
// optimal completion of this node.
bool Worker::tryCompletion(int d1) {
    const int16_t* pc = Pd(d1);
    const int last = fr[d1].node;
    const int m = acount;
    for (int k = 0; k < m; ++k) {
        const int col = alist[k];
        prow[pc[col]] = (int16_t)col;
    }
    prow[pc[last]] = (int16_t)last;  // == prow[t] = last
    int cur = last, cnt = 0;
    for (;;) {
        const int nx = prow[cur];
        if (cnt >= m) return false;
        seq[cnt++] = nx;
        if (nx == tEnd) break;
        cur = nx;
    }
    if (cnt != m) return false;
    for (int k = 0; k < m; ++k) seqPos[seq[k]] = k;
    for (int k = 0; k < m; ++k) {
        const int v = seq[k];
        for (const int32_t* p = P.hassePred.begin(v); p != P.hassePred.end(v); ++p)
            if (inR[*p] && seqPos[*p] > k) return false;
    }
    int64_t cost = fr[d1].g;
    int prev = last;
    for (int k = 0; k < m; ++k) {
        const int32_t cc = P.c(prev, seq[k]);
        if (cc == kForbidden) return false;
        cost += cc;
        prev = seq[k];
    }
    if (cost < S.inc->cost()) {
        for (int k = 0; k < m; ++k) path[d1 + 1 + k] = seq[k];
        offerPath(d1 + m, cost, "B&B");
    }
    return true;
}

void Worker::offerPath(int upto, int64_t cost, const char* src) {
    std::vector<int> full(path.begin(), path.begin() + upto + 1);
    S.inc->offer(cost, full, src);
}

void Worker::generateCands(int d) {
    Frame& f = fr[d];
    const int last = f.node;
    const int32_t* row = P.cost.data() + (size_t)last * n;
    const int64_t* U = Ud(d);
    const int64_t* V = Vd(d);
    const int64_t Ul = U[last];
    tmp.clear();
    for (int k = 0; k < acount; ++k) {
        const int j = alist[k];
        if (rem[j] == 0 && row[j] != kForbidden) tmp.emplace_back((int64_t)row[j] - Ul - V[j], j);
    }
    std::sort(tmp.begin(), tmp.end());
    int16_t* cd = cand(d);
    for (size_t k = 0; k < tmp.size(); ++k) cd[k] = (int16_t)tmp[k].second;
    f.idx = 0;
    f.cnt = (int)tmp.size();
}

void Worker::maybeDonate(int d) {
    for (int dd = base; dd <= d; ++dd) {
        Frame& f = fr[dd];
        if (f.acnt < kDonateMinRemaining) continue;
        const int remaining = f.cnt - f.idx;
        int from;
        if (dd < d) {
            if (remaining < 1) continue;
            from = f.idx;
        } else {
            if (remaining < 2) continue;
            from = f.idx + 1;  // keep the most promising child for ourselves
        }
        Task t;
        t.prefix.assign(path.begin(), path.begin() + dd + 1);
        const int16_t* cd = cand(dd);
        t.cands.assign(cd + from, cd + f.cnt);
        f.cnt = from;
        S.push(std::move(t));
        return;
    }
}

void Worker::expand(int d) {
    Frame& f = fr[d];
    const int16_t* cd = cand(d);
    const int last = f.node;
    const int32_t* C = P.cost.data();
    const bool useMemo = S.memo && S.memo->enabled();
    while (f.idx < f.cnt) {
        if (((++tick) & 127u) == 0) {
            if (S.stop->load(std::memory_order_relaxed)) { halted = true; return; }
        }
        if (S.idle.load(std::memory_order_relaxed) > 0 && S.poolSize.load(std::memory_order_relaxed) == 0)
            maybeDonate(d);
        if (f.idx >= f.cnt) break;
        const int j = cd[f.idx++];
        const int64_t ub = S.inc->cost();
        const int64_t arc = C[(size_t)last * n + j];
        const int64_t rc = arc - Ud(d)[last] - Vd(d)[j];
        if (f.g + f.obj + rc >= ub) break;  // candidates are sorted by reduced cost
        const int64_t g1 = f.g + arc;
        if (acount == 1) {  // j is the end node: complete path
            ++nodes;
            if (g1 < ub) {
                path[d + 1] = j;
                offerPath(d + 1, g1, "B&B");
            }
            continue;
        }
        if (useMemo && acount - 1 >= kMemoMinRemaining) {
            const uint64_t k1 = (f.sh1 ^ S.zs1[j]) ^ S.zl1[j];
            const uint64_t k2 = (f.sh2 ^ S.zs2[j]) ^ S.zl2[j];
            if (S.memo->checkAndInsert(k1, k2, d + 1, g1)) continue;
        }
        const Status st = enterChild(d, j, false);
        if (st == Status::Open) {
            generateCands(d + 1);
            expand(d + 1);
        }
        leaveChild(d, j);
        if (halted) return;
        if ((nodes & 8191u) == 0) flushNodes();
    }
}

void Worker::runTask(Task& t) {
    halted = false;
    loadRoot();
    if (t.root) {
        base = 0;
        if (tryCompletion(0)) return;
        generateCands(0);
        expand(0);
        return;
    }
    base = (int)t.prefix.size() - 1;
    for (int k = 1; k <= base; ++k) {
        const Status st = enterChild(k - 1, t.prefix[k], true);
        if (st != Status::Open) return;  // pruned under the new incumbent
    }
    int16_t* cd = cand(base);
    for (size_t k = 0; k < t.cands.size(); ++k) cd[k] = (int16_t)t.cands[k];
    fr[base].idx = 0;
    fr[base].cnt = (int)t.cands.size();
    expand(base);
}

}  // namespace

BnbResult runBranchAndBound(const Problem& problem, Incumbent& incumbent, const Topology& topo,
                            const BnbConfig& cfg, std::atomic<bool>& stop) {
    BnbResult res;
    Shared S;
    S.master = &problem;
    S.inc = &incumbent;
    S.topo = &topo;
    S.cfg = &cfg;
    S.stop = &stop;
    const int n = problem.n;

    // Dominance table (only if path costs always fit in its 40-bit cost field).
    std::unique_ptr<MemoTable> memo;
    if (cfg.memoBytes > 0 && (double)n * (double)problem.maxArc < (double)MemoTable::kMaxCost / 2) {
        memo.reset(new MemoTable(cfg.memoBytes, topo.memNodes));
        if (memo->enabled()) {
            res.memoUsed = true;
            logMsg("dominance table: %.2f GiB", memo->bytes() / 1073741824.0);
        } else {
            memo.reset();
        }
    }
    S.memo = memo.get();

    uint64_t seed = 0x5DEECE66DULL ^ (uint64_t)n * 7919ULL;
    S.zs1.resize(n); S.zs2.resize(n); S.zl1.resize(n); S.zl2.resize(n);
    for (int i = 0; i < n; ++i) {
        S.zs1[i] = splitmix(seed); S.zs2[i] = splitmix(seed);
        S.zl1[i] = splitmix(seed); S.zl2[i] = splitmix(seed);
    }

    const bool usePin = cfg.pin && topo.valid && !topo.cpuOrder.empty();
    const int numNodes = std::max(1, topo.numNodes);
    S.replicas.resize(numNodes);
    S.replicaOnce.reset(new std::once_flag[numNodes]);

    Task rootTask;
    rootTask.root = true;
    S.pool.push_back(std::move(rootTask));
    S.poolSize.store(1);

    const int W = std::max(1, cfg.workers);
    std::vector<uint64_t> perWorker(W, 0);
    auto body = [&](int w) {
        int node = 0;
        if (usePin) {
            const size_t idx = (size_t)(cfg.cpuOffset + w) % topo.cpuOrder.size();
            pinCurrentThread(topo.cpuOrder[idx]);
            node = topo.nodeOfOrderIdx[idx];
            if (node < 0 || node >= numNodes) node = 0;
        }
        // NUMA-local replica of the read-only problem data (first touch by a pinned thread).
        const Problem* local = &problem;
        if (usePin && numNodes > 1) {
            std::call_once(S.replicaOnce[node], [&] { S.replicas[node].reset(new Problem(problem)); });
            local = S.replicas[node].get();
        }
        Worker worker(S, *local);
        std::call_once(S.rootOnce, [&] { worker.computeRootAP(); });
        if (!S.root.infeasible) {
            Task t;
            while (S.getTask(t)) {
                worker.runTask(t);
                worker.flushNodes();
                S.finishTask();
            }
        } else {
            std::lock_guard<std::mutex> lk(S.mtx);
            S.done = true;
            S.cv.notify_all();
            S.supCv.notify_all();
        }
        perWorker[w] = worker.nodes;
    };

    std::vector<std::thread> threads;
    for (int w = 0; w < W; ++w) threads.emplace_back(body, w);

    // Supervisor: waits for completion, reports progress, enforces the deadline.
    bool timedOut = false;
    {
        std::unique_lock<std::mutex> lk(S.mtx);
        double nextReport = nowSeconds() + 10.0;
        while (!S.done) {
            double now = nowSeconds();
            if (stop.load(std::memory_order_relaxed)) { timedOut = true; break; }
            if (cfg.deadline >= 0 && now >= cfg.deadline) { timedOut = true; break; }
            double wake = nextReport;
            if (cfg.deadline >= 0) wake = std::min(wake, cfg.deadline);
            double wait = std::max(0.001, std::min(0.25, wake - now));
            S.supCv.wait_for(lk, std::chrono::duration<double>(wait));
            if (nowSeconds() >= nextReport) {
                logMsg("progress: %llu nodes, best cost %lld, pool %d, idle %d",
                       (unsigned long long)S.nodesPublished.load(), (long long)incumbent.cost(),
                       S.poolSize.load(), S.idle.load());
                nextReport = nowSeconds() + 10.0;
            }
        }
    }
    if (timedOut) {
        {
            std::lock_guard<std::mutex> lk(S.mtx);
            stop.store(true);
        }
        S.cv.notify_all();
    }
    for (auto& th : threads) th.join();

    uint64_t total = 0;
    for (uint64_t v : perWorker) total += v;
    res.nodes = total;
    res.rootLowerBound = S.root.obj;
    res.completed = S.done && !timedOut && !stop.load();
    return res;
}

}  // namespace sop
