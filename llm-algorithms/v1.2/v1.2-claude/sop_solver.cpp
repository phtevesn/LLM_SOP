// =============================================================================
//  sop_solver.cpp -- exact, parallel solver for the Sequential Ordering Problem
//
//  Algorithm (details in README.md):
//    * precedence closure / transitive reduction (bitsets), static arc elimination
//    * parallel iterated local search (precedence-feasible Or-opt) for an initial UB
//    * parallel depth-first branch & bound with
//        - incremental Assignment-Problem (AP) lower bound (dual warm start per node)
//        - (visited-set, last-node) dominance via a lock-free hashed transposition table
//        - dynamic work donation between threads, NUMA-aware thread placement
//  Build:  g++ -O3 -march=native -std=c++17 -pthread -o sop_solver sop_solver.cpp
// =============================================================================
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace std;
typedef long long i64;
typedef unsigned long long u64;
typedef unsigned int u32;

static const i64 INFC  = 1000000000000LL;   // "no arc" cost inside the AP matrix
static const i64 UBINF = LLONG_MAX / 4;     // "no incumbent"
static const i64 INFM  = LLONG_MAX / 2;     // Dijkstra infinity

static chrono::steady_clock::time_point g_t0;
static double nowSec() { return chrono::duration<double>(chrono::steady_clock::now() - g_t0).count(); }
#define LOG(...) do { fprintf(stderr, __VA_ARGS__); fflush(stderr); } while (0)

// ----------------------------------------------------------------------------
// Instance + preprocessing (shared, read-only after setup)
// ----------------------------------------------------------------------------
static int n = 0, N1 = 1, W = 1;
static vector<i64> Tm;                       // original n*n matrix
static vector<i64> C;                        // (n+1)*(n+1) AP costs; row n = virtual start S, col n = virtual end E
static vector<vector<u64>> Pb, Sb;           // transitive closure: Pb[v]=must precede v, Sb[u]=must follow u
static vector<vector<int>> succD;            // direct successors in transitive reduction
static vector<int> predCntD;                 // direct predecessor counts (transitive reduction)
static vector<u64> Zs, ZL;                   // Zobrist tables
static bool g_cyclic = false;

static atomic<i64> g_ub(UBINF);
static mutex g_bestMx;
static vector<int> g_bestPath;
static atomic<bool> g_stop(false);

static inline bool bitTest(const vector<u64>& b, int i) { return (b[i >> 6] >> (i & 63)) & 1ULL; }

static bool parseSop(const string& path, string& err) {
    ifstream f(path, ios::binary);
    if (!f) { err = "cannot open file"; return false; }
    string data((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());
    size_t pos = 0; long dim = -1; bool found = false; string fmt;
    auto up = [](string s) { for (auto& ch : s) ch = (char)toupper((unsigned char)ch); return s; };
    auto trim = [](const string& s) { size_t a = s.find_first_not_of(" \t\r\n"); if (a == string::npos) return string(); size_t b = s.find_last_not_of(" \t\r\n"); return s.substr(a, b - a + 1); };
    while (pos < data.size()) {
        size_t e = data.find('\n', pos); if (e == string::npos) e = data.size();
        string line = trim(data.substr(pos, e - pos)); pos = e + 1;
        string U = up(line);
        if (U.rfind("EDGE_WEIGHT_SECTION", 0) == 0) { found = true; break; }
        size_t c = line.find(':');
        string val = trim(c == string::npos ? string() : line.substr(c + 1));
        if (U.rfind("DIMENSION", 0) == 0) {
            if (c == string::npos) val = trim(line.substr(9));
            dim = strtol(val.c_str(), nullptr, 10);
        } else if (U.rfind("EDGE_WEIGHT_FORMAT", 0) == 0) fmt = up(val);
    }
    if (!found) { err = "EDGE_WEIGHT_SECTION not found"; return false; }
    if (!fmt.empty() && fmt != "FULL_MATRIX") LOG("warning: EDGE_WEIGHT_FORMAT=%s, assuming FULL_MATRIX\n", fmt.c_str());
    vector<i64> vals;
    const char* p = data.c_str() + pos; const char* end = data.c_str() + data.size();
    while (p < end) {
        while (p < end && isspace((unsigned char)*p)) ++p;
        if (p >= end) break;
        if (!(isdigit((unsigned char)*p) || *p == '-' || *p == '+')) break;   // EOF token etc.
        char* q; long long v = strtoll(p, &q, 10);
        if (q == p) break;
        vals.push_back(v); p = q;
    }
    if (dim <= 0 && !vals.empty()) dim = (long)vals[0];
    if (dim <= 0) { err = "bad DIMENSION"; return false; }
    size_t nn = (size_t)dim * dim, off = 0;
    if (vals.size() >= nn + 1 && vals[0] == dim) off = 1;
    else if (vals.size() >= nn) off = 0;
    else if (vals.size() == nn + 1) off = 1;
    else { err = "matrix has too few values (" + to_string(vals.size()) + " < " + to_string(nn) + ")"; return false; }
    n = (int)dim; Tm.assign(vals.begin() + off, vals.begin() + off + nn);
    return true;
}

static bool preprocess() {
    N1 = n + 1; W = (n + 63) / 64;
    i64 mx = 0;
    for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) if (i != j && Tm[(size_t)i * n + j] > mx) mx = Tm[(size_t)i * n + j];
    if (mx >= 1000000000LL) { LOG("error: arc costs >= 1e9 are not supported\n"); exit(2); }
    // direct constraints: T[i][j] < 0 => j precedes i
    vector<vector<int>> dp(n), ds(n);
    vector<int> indeg(n, 0);
    for (int i = 0; i < n; i++) for (int j = 0; j < n; j++)
        if (i != j && Tm[(size_t)i * n + j] < 0) { dp[i].push_back(j); ds[j].push_back(i); indeg[i]++; }
    vector<int> topo; topo.reserve(n);
    for (int i = 0; i < n; i++) if (!indeg[i]) topo.push_back(i);
    for (size_t h = 0; h < topo.size(); h++) for (int s : ds[topo[h]]) if (--indeg[s] == 0) topo.push_back(s);
    if ((int)topo.size() != n) { g_cyclic = true; return false; }
    Pb.assign(n, vector<u64>(W, 0)); Sb.assign(n, vector<u64>(W, 0));
    for (int v : topo) for (int p : dp[v]) { for (int w = 0; w < W; w++) Pb[v][w] |= Pb[p][w]; Pb[v][p >> 6] |= 1ULL << (p & 63); }
    for (int h = n - 1; h >= 0; h--) { int u = topo[h]; for (int s : ds[u]) { for (int w = 0; w < W; w++) Sb[u][w] |= Sb[s][w]; Sb[u][s >> 6] |= 1ULL << (s & 63); } }
    // transitive reduction
    succD.assign(n, {}); predCntD.assign(n, 0);
    vector<u64> tmp(W);
    for (int v = 0; v < n; v++) {
        fill(tmp.begin(), tmp.end(), 0);
        for (int w = 0; w < W; w++) { u64 x = Pb[v][w]; while (x) { int k = (w << 6) + __builtin_ctzll(x); x &= x - 1; for (int t = 0; t < W; t++) tmp[t] |= Pb[k][t]; } }
        for (int w = 0; w < W; w++) { u64 x = Pb[v][w] & ~tmp[w]; while (x) { int k = (w << 6) + __builtin_ctzll(x); x &= x - 1; succD[k].push_back(v); predCntD[v]++; } }
    }
    // AP cost matrix with static arc elimination
    C.assign((size_t)N1 * N1, INFC);
    for (int u = 0; u < n; u++) for (int v = 0; v < n; v++) {
        if (u == v) continue;
        if (bitTest(Pb[u], v)) continue;                       // v must precede u
        bool bad = false;                                      // some k with u < k < v forces non-adjacency
        for (int w = 0; w < W; w++) if (Sb[u][w] & Pb[v][w]) { bad = true; break; }
        if (bad) continue;
        i64 c = Tm[(size_t)u * n + v]; if (c < 0) continue;
        C[(size_t)u * N1 + v] = c;
    }
    for (int v = 0; v < n; v++) {
        bool src = true, snk = true;
        for (int w = 0; w < W; w++) { if (Pb[v][w]) src = false; if (Sb[v][w]) snk = false; }
        if (src) C[(size_t)n * N1 + v] = 0;       // S -> source
        if (snk) C[(size_t)v * N1 + n] = 0;       // sink -> E
    }
    // Zobrist
    u64 s = 0x9E3779B97F4A7C15ULL;
    auto sm = [&]() { u64 z = (s += 0x9E3779B97F4A7C15ULL); z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL; return z ^ (z >> 31); };
    Zs.resize(n + 1); ZL.resize(n + 1);
    for (int i = 0; i <= n; i++) { Zs[i] = sm(); ZL[i] = sm(); }
    return true;
}

static void tryUpdate(const vector<int>& path, i64 cost) {
    lock_guard<mutex> lk(g_bestMx);
    if (cost < g_ub.load()) { g_ub.store(cost); g_bestPath = path; }
}

// ----------------------------------------------------------------------------
// Thread pool bookkeeping for B&B
// ----------------------------------------------------------------------------
struct Task { vector<int> path; };           // empty path = whole tree
struct Pool {
    mutex m; condition_variable cv; deque<Task> q;
    int idle = 0, T = 1; bool done = false;
    atomic<int> idleHint{0};
} P;
static void requestStop() { g_stop.store(true); lock_guard<mutex> lk(P.m); P.cv.notify_all(); }

// ----------------------------------------------------------------------------
// Lock-free transposition table: bucket = 4 x (key^data, data) pairs (64 bytes)
// ----------------------------------------------------------------------------
struct TT {
    u64* d = nullptr; size_t nb = 0;
    void init(size_t bytes) {
        size_t b = 1; while (b * 2 * 64 <= bytes) b *= 2;
        nb = b;
        void* p = mmap(nullptr, nb * 64, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED) { nb = 1024; p = calloc(nb, 64); }
        d = (u64*)p;
    }
    // true => state (same set,last) already seen with cost <= g  => prune
    inline bool probe(u64 key, i64 g, int depth) {
        if (g > 0xFFFFFFFFLL) return false;
        u64* e = d + 8 * (key & (nb - 1));
        int victim = -1; i64 worst = -1;
        u64 nb64 = ((u64)depth << 32) | (u64)g;
        for (int k = 0; k < 4; k++) {
            u64 a = __atomic_load_n(&e[2 * k], __ATOMIC_RELAXED), b = __atomic_load_n(&e[2 * k + 1], __ATOMIC_RELAXED);
            if ((a ^ b) == key) {
                if ((i64)(b & 0xFFFFFFFFULL) <= g) return true;
                __atomic_store_n(&e[2 * k + 1], nb64, __ATOMIC_RELAXED);
                __atomic_store_n(&e[2 * k], key ^ nb64, __ATOMIC_RELAXED);
                return false;
            }
            if (a == 0 && b == 0) { if (worst != INT_MAX) { victim = k; worst = INT_MAX; } }
            else if (worst != INT_MAX) { i64 dd = (i64)(b >> 32); if (dd > worst) { worst = dd; victim = k; } }
        }
        __atomic_store_n(&e[2 * victim + 1], nb64, __ATOMIC_RELAXED);
        __atomic_store_n(&e[2 * victim], key ^ nb64, __ATOMIC_RELAXED);
        return false;
    }
};

// ----------------------------------------------------------------------------
// Branch & bound worker
// ----------------------------------------------------------------------------
struct Cand { int c; i64 bound; };
struct RootState { vector<i64> U, V; vector<int> RM, CM; i64 lb = 0; bool ok = false; } g_root;

// Generic dual-ascent augmentation (shortest augmenting path from free row r0 over the
// active columns act[0..na)).  `dual` is the current dual objective (sum u + sum v) and is
// updated; returns false on infeasibility or when gN+dual >= ub (prune).
static bool apAugment(i64* u, i64* v, int* rm, int* cm, const int* act, int na, int r0,
                      i64& dual, i64 gN, i64 ub, bool force,
                      vector<i64>& minv, vector<int>& way, vector<char>& used, vector<int>& usedL) {
    for (int k = 0; k < na; k++) { int j = act[k]; minv[j] = INFM; used[j] = 0; }
    int usedCnt = 0, i0 = r0, j0 = -1, j1 = -1;
    while (true) {
        const i64* Ci = &C[(size_t)i0 * N1]; i64 ui = u[i0];
        i64 delta = INFM; j1 = -1;
        for (int k = 0; k < na; k++) {
            int j = act[k]; if (used[j]) continue;
            i64 cur = Ci[j] - ui - v[j];
            if (cur < minv[j]) { minv[j] = cur; way[j] = j0; }
            if (minv[j] < delta) { delta = minv[j]; j1 = j; }
        }
        if (j1 < 0 || delta >= INFC / 2) return false;
        if (delta) {
            u[r0] += delta;
            for (int t = 0; t < usedCnt; t++) { int j = usedL[t]; u[cm[j]] += delta; v[j] -= delta; }
            for (int k = 0; k < na; k++) { int j = act[k]; if (!used[j]) minv[j] -= delta; }
            dual += delta;
            if (!force && gN + dual >= ub) return false;
        }
        used[j1] = 1; usedL[usedCnt++] = j1; j0 = j1;
        if (cm[j1] < 0) break;
        i0 = cm[j1];
    }
    do { int jp = way[j0]; int r = (jp < 0) ? r0 : cm[jp]; cm[j0] = r; rm[r] = j0; j0 = jp; } while (j0 >= 0);
    return true;
}

static bool computeRoot() {
    g_root.U.assign(N1, 0); g_root.V.assign(N1, 0); g_root.RM.assign(N1, -1); g_root.CM.assign(N1, -1);
    auto& U = g_root.U; auto& V = g_root.V; auto& RM = g_root.RM; auto& CM = g_root.CM;
    for (int j = 0; j < N1; j++) { i64 m = INFM; for (int i = 0; i < N1; i++) m = min(m, C[(size_t)i * N1 + j]); V[j] = m; }
    for (int i = 0; i < N1; i++) { i64 m = INFM; for (int j = 0; j < N1; j++) m = min(m, C[(size_t)i * N1 + j] - V[j]); U[i] = m; }
    for (int i = 0; i < N1; i++) for (int j = 0; j < N1; j++)
        if (CM[j] < 0 && C[(size_t)i * N1 + j] - U[i] - V[j] == 0) { RM[i] = j; CM[j] = i; break; }
    vector<int> act(N1); for (int j = 0; j < N1; j++) act[j] = j;
    vector<i64> minv(N1); vector<int> way(N1), usedL(N1); vector<char> used(N1);
    i64 dual = 0;
    for (int i = 0; i < N1; i++) { dual += U[i] + V[i]; }
    for (int i = 0; i < N1; i++) {
        if (RM[i] >= 0) continue;
        if (g_stop.load()) return false;
        if (!apAugment(U.data(), V.data(), RM.data(), CM.data(), act.data(), N1, i, dual, 0, UBINF, true, minv, way, used, usedL)) return false;
    }
    g_root.lb = dual; g_root.ok = true;
    return true;
}

struct Worker {
    int id = 0, cpu = -1; TT* tt = nullptr;
    u64 nodes = 0;
    // per-depth AP state
    vector<i64> U, V; vector<int> RM, CM;
    vector<i64> LB, g;
    vector<int> unvis, pos, remPred, path;
    vector<vector<Cand>> levelC; vector<int> nextI, endI;
    vector<i64> minv; vector<int> way, usedL, act; vector<char> used;
    u64 zset = 0; int base = 0;

    inline i64* Ud(int d) { return &U[(size_t)d * N1]; }
    inline i64* Vd(int d) { return &V[(size_t)d * N1]; }
    inline int* RMd(int d) { return &RM[(size_t)d * N1]; }
    inline int* CMd(int d) { return &CM[(size_t)d * N1]; }

    void init() {
        size_t L = (size_t)(n + 1) * N1;
        U.assign(L, 0); V.assign(L, 0); RM.assign(L, -1); CM.assign(L, -1);
        LB.assign(n + 2, 0); g.assign(n + 2, 0);
        unvis.resize(n); pos.resize(n); remPred = predCntD; path.assign(n + 1, 0);
        for (int i = 0; i < n; i++) { unvis[i] = i; pos[i] = i; }
        levelC.assign(n + 1, {}); nextI.assign(n + 2, 0); endI.assign(n + 2, 0);
        minv.assign(N1, 0); way.assign(N1, 0); usedL.assign(N1, 0); act.assign(N1, 0); used.assign(N1, 0);
        copy(g_root.U.begin(), g_root.U.end(), U.begin()); copy(g_root.V.begin(), g_root.V.end(), V.begin());
        copy(g_root.RM.begin(), g_root.RM.end(), RM.begin()); copy(g_root.CM.begin(), g_root.CM.end(), CM.begin());
        LB[0] = g_root.lb; g[0] = 0; zset = 0;
    }

    // move from depth d to d+1 by visiting c. force=true: replay (no pruning, no TT).
    bool enter(int d, int c, bool force) {
        const int l = (d == 0) ? n : path[d - 1];
        const int m = n - d;
        i64 gN = g[d] + C[(size_t)l * N1 + c];
        if (!force) {
            u64 key = zset ^ Zs[c] ^ ZL[c]; if (!key) key = 1;
            if (tt->probe(key, gN, d + 1)) return false;
        }
        i64 ub = force ? UBINF : g_ub.load(memory_order_relaxed);
        i64 *u0 = Ud(d), *v0 = Vd(d), *u1 = Ud(d + 1), *v1 = Vd(d + 1);
        int *rm0 = RMd(d), *cm0 = CMd(d), *rm1 = RMd(d + 1), *cm1 = CMd(d + 1);
        for (int k = 0; k < m; k++) {
            int r = unvis[k]; u1[r] = u0[r]; rm1[r] = rm0[r];
            if (r != c) { v1[r] = v0[r]; cm1[r] = cm0[r]; }
        }
        v1[n] = v0[n]; cm1[n] = cm0[n];
        i64 dual = LB[d] - u0[l] - v0[c];
        int r0 = cm0[c];
        bool repair = (r0 != l);
        if (repair) { rm1[r0] = -1; cm1[rm0[l]] = -1; }
        if (!force && gN + dual >= ub) return false;
        if (repair) {
            int na = 0;
            for (int k = 0; k < m; k++) { int x = unvis[k]; if (x != c) act[na++] = x; }
            act[na++] = n;
            if (!apAugment(u1, v1, rm1, cm1, act.data(), na, r0, dual, gN, ub, force, minv, way, used, usedL)) return false;
        }
        // commit
        path[d] = c;
        int p = pos[c], last = unvis[m - 1];
        unvis[p] = last; pos[last] = p; unvis[m - 1] = c; pos[c] = m - 1;
        for (int w : succD[c]) --remPred[w];
        zset ^= Zs[c];
        g[d + 1] = gN; LB[d + 1] = dual;
        return true;
    }
    void leave(int d) {
        int c = path[d];
        for (int w : succD[c]) ++remPred[w];
        zset ^= Zs[c];
    }

    void maybeDonate(int dcur) {
        if (n - base < 8) return;
        for (int lv = base; lv <= dcur; lv++) {
            if (nextI[lv] < endI[lv] && n - lv >= 8) {
                {
                    lock_guard<mutex> lk(P.m);
                    for (int k = nextI[lv]; k < endI[lv]; k++) {
                        Task t; t.path.assign(path.begin(), path.begin() + lv); t.path.push_back(levelC[lv][k].c);
                        P.q.push_back(std::move(t));
                    }
                    endI[lv] = nextI[lv];
                    P.cv.notify_all();
                }
                return;
            }
        }
    }

    void dfs(int d) {
        ++nodes;
        if (g_stop.load(memory_order_relaxed)) return;
        if (d == n) { if (g[d] < g_ub.load()) tryUpdate(vector<int>(path.begin(), path.begin() + n), g[d]); return; }
        const int l = (d == 0) ? n : path[d - 1];
        const int m = n - d;
        i64 ub = g_ub.load(memory_order_relaxed);
        auto& cl = levelC[d]; cl.clear();
        const i64* Cl = &C[(size_t)l * N1];
        i64 ul = Ud(d)[l]; const i64* v0 = Vd(d);
        i64 gl = g[d] + LB[d];
        for (int k = 0; k < m; k++) {
            int c = unvis[k];
            if (remPred[c]) continue;
            i64 cc = Cl[c]; if (cc >= INFC) continue;
            i64 b = gl + (cc - ul - v0[c]);
            if (b < ub) cl.push_back({c, b});
        }
        sort(cl.begin(), cl.end(), [&](const Cand& a, const Cand& b) { return a.bound < b.bound || (a.bound == b.bound && Cl[a.c] < Cl[b.c]); });
        nextI[d] = 0; endI[d] = (int)cl.size();
        while (nextI[d] < endI[d]) {
            if (g_stop.load(memory_order_relaxed)) return;
            if (P.idleHint.load(memory_order_relaxed) > 0 && (nodes & 7) == 0) maybeDonate(d);
            if (nextI[d] >= endI[d]) break;
            Cand cd = cl[nextI[d]++];
            if (cd.bound >= g_ub.load(memory_order_relaxed)) { endI[d] = nextI[d]; break; }   // sorted => all later are pruned too
            if (enter(d, cd.c, false)) { dfs(d + 1); leave(d); }
        }
        endI[d] = nextI[d] = 0;
    }

    void runTask(const Task& t) {
        int L = (int)t.path.size();
        if (L == 0) { base = 0; dfs(0); return; }
        for (int i = 0; i < L - 1; i++) if (!enter(i, t.path[i], true)) { for (int k = i - 1; k >= 0; k--) leave(k); return; }
        for (int i = 0; i <= L; i++) nextI[i] = endI[i] = 0;
        if (enter(L - 1, t.path[L - 1], false)) { base = L; dfs(L); leave(L - 1); }
        for (int k = L - 2; k >= 0; k--) leave(k);
    }

    bool getTask(Task& t) {
        unique_lock<mutex> lk(P.m);
        while (true) {
            if (g_stop.load()) return false;
            if (!P.q.empty()) { t = std::move(P.q.front()); P.q.pop_front(); return true; }
            if (P.done) return false;
            P.idle++; P.idleHint.store(P.idle);
            if (P.idle == P.T) { P.done = true; P.idle--; P.idleHint.store(P.idle); P.cv.notify_all(); return false; }
            P.cv.wait(lk);
            P.idle--; P.idleHint.store(P.idle);
        }
    }
    void run() {
        init();
        Task t;
        while (getTask(t)) {
            runTask(t);
            for (int i = 0; i <= n; i++) nextI[i] = endI[i] = 0;
        }
    }
};

// ----------------------------------------------------------------------------
// Iterated local search heuristic (precedence-feasible Or-opt)
// ----------------------------------------------------------------------------
struct ILS {
    vector<int> tour, pos, bestTour, saved; i64 cost = 0, bestCost = UBINF; mt19937_64 rng;
    vector<char> act; vector<int> stk; int touched[8], nt = 0;
    explicit ILS(u64 seed) : rng(seed) {}
    inline i64 arc(int a, int b) const { return Tm[(size_t)a * n + b]; }
    inline bool mustPrecede(int s, int w) const { return bitTest(Pb[w], s); }   // s before w
    void push(int x) { if (x >= 0 && !act[x]) { act[x] = 1; stk.push_back(x); } }
    void construct(double noise) {
        vector<int> rp = predCntD, ready, out; out.reserve(n);
        for (int i = 0; i < n; i++) if (!rp[i]) ready.push_back(i);
        uniform_real_distribution<double> ur(0, 1);
        while (!ready.empty()) {
            int pick;
            if (out.empty() || ur(rng) < noise) pick = (int)(rng() % ready.size());
            else { int last = out.back(); pick = 0; i64 bc = LLONG_MAX; for (size_t k = 0; k < ready.size(); k++) { i64 c = C[(size_t)last * N1 + ready[k]]; if (c >= INFC) c = LLONG_MAX / 2; else c = c - g_root.U[last] - g_root.V[ready[k]]; if (c < bc) { bc = c; pick = (int)k; } } }
            int v = ready[pick]; ready[pick] = ready.back(); ready.pop_back(); out.push_back(v);
            for (int w : succD[v]) if (--rp[w] == 0) ready.push_back(w);
        }
        tour = out; rebuild();
    }
    void rebuild() { pos.resize(n); cost = 0; for (int i = 0; i < n; i++) { pos[tour[i]] = i; if (i) cost += arc(tour[i - 1], tour[i]); } }
    bool segBlockedFwd(int i, int j, int w) const { for (int k = i; k <= j; k++) if (mustPrecede(tour[k], w)) return true; return false; }
    bool segBlockedBack(int i, int j, int w) const { for (int k = i; k <= j; k++) if (mustPrecede(w, tour[k])) return true; return false; }
    bool tryMove(int i, int len) {
        int j = i + len - 1; if (i < 0 || j >= n) return false;
        int a = i > 0 ? tour[i - 1] : -1, b = j < n - 1 ? tour[j + 1] : -1, s1 = tour[i], sj = tour[j];
        i64 rg = (a >= 0 ? arc(a, s1) : 0) + (b >= 0 ? arc(sj, b) : 0) - ((a >= 0 && b >= 0) ? arc(a, b) : 0);
        for (int p = j + 1; p < n; p++) {
            int w = tour[p]; if (segBlockedFwd(i, j, w)) break;
            int q = p + 1 < n ? tour[p + 1] : -1;
            i64 add = arc(w, s1) + (q >= 0 ? arc(sj, q) - arc(w, q) : 0);
            if (add - rg < 0) { rotate(tour.begin() + i, tour.begin() + j + 1, tour.begin() + p + 1); for (int k = i; k <= p; k++) pos[tour[k]] = k; cost += add - rg;
                push(a); push(b); push(s1); push(sj); push(w); push(q); return true; }
        }
        for (int p = i - 1; p >= 0; p--) {
            int w = tour[p]; if (segBlockedBack(i, j, w)) break;
            int pr = p > 0 ? tour[p - 1] : -1;
            i64 add = arc(sj, w) + (pr >= 0 ? arc(pr, s1) - arc(pr, w) : 0);
            if (add - rg < 0) { rotate(tour.begin() + p, tour.begin() + i, tour.begin() + j + 1); for (int k = p; k <= j; k++) pos[tour[k]] = k; cost += add - rg;
                push(a); push(b); push(s1); push(sj); push(w); push(pr); return true; }
        }
        return false;
    }
    void localSearch() {
        static const int lens[] = {1, 2, 3, 4, 6, 9, 14, 22, 35};
        while (!stk.empty()) {
            int x = stk.back(); stk.pop_back(); act[x] = 0;
            bool imp = true;
            while (imp) {
                imp = false;
                for (int L : lens) {
                    if (L > n) break;
                    if (tryMove(pos[x], L) || tryMove(pos[x] - L + 1, L)) { imp = true; break; }
                }
            }
        }
    }
    void kick() {
        int moves = 2 + (int)(rng() % 3);
        for (int t = 0; t < moves; t++) {
            int len = 1 + (int)(rng() % min(n, 10)); int i = (int)(rng() % (n - len + 1)); int j = i + len - 1;
            int fw = 0, bw = 0;
            for (int p = j + 1; p < n; p++) { if (segBlockedFwd(i, j, tour[p])) break; fw++; }
            for (int p = i - 1; p >= 0; p--) { if (segBlockedBack(i, j, tour[p])) break; bw++; }
            if (fw + bw == 0) continue;
            int r = (int)(rng() % (fw + bw)), p;
            if (r < fw) { p = j + 1 + r; push(tour[i]); push(tour[j]); push(tour[p]); if (i) push(tour[i - 1]); if (j + 1 < n) push(tour[j + 1]);
                          rotate(tour.begin() + i, tour.begin() + j + 1, tour.begin() + p + 1); if (p + 1 < n) push(tour[p + 1]); }
            else { p = i - 1 - (r - fw); push(tour[i]); push(tour[j]); push(tour[p]); if (p) push(tour[p - 1]); if (j + 1 < n) push(tour[j + 1]);
                   rotate(tour.begin() + p, tour.begin() + i, tour.begin() + j + 1); if (i + 1 < n) push(tour[i]); }
            for (int k = max(0, min(p, i) - 1); k < min(n, max(p, j) + 2); k++) pos[tour[k]] = k;
            push(tour[min(n - 1, max(0, p))]);
        }
        cost = 0; for (int i = 1; i < n; i++) cost += arc(tour[i - 1], tour[i]);
    }
    void run(double deadline) {
        act.assign(n, 0); stk.clear();
        construct(0.0); for (int i = 0; i < n; i++) push(i); localSearch();
        bestTour = tour; bestCost = cost; tryUpdate(tour, cost);
        i64 curCost = cost; vector<int> cur = tour; int it = 0;
        uniform_real_distribution<double> ur(0, 1);
        while (nowSec() < deadline && !g_stop.load()) {
            if (n < 3) break;
            if (++it % 3000 == 0) {   // restart from a perturbed construction
                construct(0.2); for (int i = 0; i < n; i++) push(i); localSearch(); cur = tour; curCost = cost;
            } else { kick(); localSearch(); }
            if (cost < bestCost) { bestCost = cost; bestTour = tour; tryUpdate(tour, cost); }
            double T = 0.004 * (double)bestCost / max(1, n / 20);   // small threshold-style acceptance
            if (cost <= curCost || ur(rng) < exp(-(double)(cost - curCost) / max(1.0, T))) { curCost = cost; cur = tour; }
            else { tour = cur; rebuild(); }
        }
    }
};

// ----------------------------------------------------------------------------
// NUMA / CPU placement (sysfs only, no libnuma dependency)
// ----------------------------------------------------------------------------
static vector<int> readList(const string& s) {
    vector<int> r; stringstream ss(s); string tok;
    while (getline(ss, tok, ',')) {
        size_t a = tok.find_first_of("0123456789"); if (a == string::npos) continue;
        size_t dash = tok.find('-');
        if (dash == string::npos) r.push_back(atoi(tok.c_str() + a));
        else { int lo = atoi(tok.c_str() + a), hi = atoi(tok.c_str() + dash + 1); for (int x = lo; x <= hi; x++) r.push_back(x); }
    }
    return r;
}
static string slurp(const string& p) { ifstream f(p); if (!f) return ""; stringstream ss; ss << f.rdbuf(); return ss.str(); }

struct Placement { vector<int> cpu, group; int ngroups = 1; };
static Placement planThreads(int T, bool pin) {
    Placement pl; pl.cpu.assign(T, -1); pl.group.assign(T, 0);
    if (!pin || T <= 1) return pl;
    cpu_set_t cs; CPU_ZERO(&cs);
    if (sched_getaffinity(0, sizeof(cs), &cs) != 0) return pl;
    vector<char> allowed(4096, 0); for (int c = 0; c < 4096; c++) if (CPU_ISSET(c, &cs)) allowed[c] = 1;
    struct Node { int id; vector<int> cpus; long mem; };
    vector<Node> nodes;
    for (int k = 0; k < 256; k++) {
        string cl = slurp("/sys/devices/system/node/node" + to_string(k) + "/cpulist");
        if (cl.empty()) { if (k > 0) break; else continue; }
        Node nd; nd.id = k; nd.cpus = readList(cl); nd.mem = 0;
        string mi = slurp("/sys/devices/system/node/node" + to_string(k) + "/meminfo");
        size_t p = mi.find("MemTotal:"); if (p != string::npos) nd.mem = atol(mi.c_str() + p + 9);
        nodes.push_back(nd);
    }
    if (nodes.empty()) { Node nd; nd.id = 0; nd.mem = 1; for (int c = 0; c < 4096; c++) if (allowed[c]) nd.cpus.push_back(c); nodes.push_back(nd); }
    // memory-bearing nodes first (Threadripper 2990WX: only 2 of 4 dies own memory controllers)
    stable_sort(nodes.begin(), nodes.end(), [](const Node& a, const Node& b) { return (a.mem > 0) > (b.mem > 0); });
    vector<pair<int,int>> seq;   // (cpu, node)
    for (int pass = 0; pass < 2; pass++)
        for (auto& nd : nodes) for (int c : nd.cpus) {
            if (c < 0 || c >= 4096 || !allowed[c]) continue;
            vector<int> sib = readList(slurp("/sys/devices/system/cpu/cpu" + to_string(c) + "/topology/thread_siblings_list"));
            bool primary = sib.empty() || sib[0] == c;
            if ((pass == 0) == primary) seq.push_back({c, nd.id});
        }
    if (seq.empty()) return pl;
    auto groupOf = [&](int nodeId) {
        for (auto& nd : nodes) if (nd.id == nodeId && nd.mem > 0) return nodeId;
        vector<int> dist = readList(slurp("/sys/devices/system/node/node" + to_string(nodeId) + "/distance"));
        // distance file is space separated; readList splits on commas only, so parse manually
        { stringstream ss(slurp("/sys/devices/system/node/node" + to_string(nodeId) + "/distance")); dist.clear(); int x; while (ss >> x) dist.push_back(x); }
        int best = nodeId, bd = INT_MAX;
        for (auto& nd : nodes) if (nd.mem > 0 && nd.id < (int)dist.size() && dist[nd.id] < bd) { bd = dist[nd.id]; best = nd.id; }
        return best;
    };
    vector<int> gids;
    for (int t = 0; t < T; t++) {
        auto pr = seq[t % seq.size()]; pl.cpu[t] = pr.first;
        int gn = groupOf(pr.second);
        auto it = find(gids.begin(), gids.end(), gn);
        if (it == gids.end()) { gids.push_back(gn); pl.group[t] = (int)gids.size() - 1; } else pl.group[t] = (int)(it - gids.begin());
    }
    pl.ngroups = (int)gids.size();
    return pl;
}
static void pinTo(int cpu) {
    if (cpu < 0) return;
    cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(cpu, &cs);
    pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
}

// ----------------------------------------------------------------------------
// Validation + output
// ----------------------------------------------------------------------------
static bool validate(const vector<int>& path, i64& cost, string& why) {
    if ((int)path.size() != n) { why = "wrong length"; return false; }
    vector<int> ps(n, -1);
    for (int i = 0; i < n; i++) { int v = path[i]; if (v < 0 || v >= n || ps[v] >= 0) { why = "not a permutation"; return false; } ps[v] = i; }
    for (int i = 0; i < n; i++) for (int j = 0; j < n; j++)
        if (i != j && Tm[(size_t)i * n + j] < 0 && ps[j] > ps[i]) { why = "precedence violated"; return false; }
    cost = 0; for (int i = 1; i < n; i++) cost += Tm[(size_t)path[i - 1] * n + path[i]];
    return true;
}

int main(int argc, char** argv) {
    g_t0 = chrono::steady_clock::now();
    string file, outPath; double timeLimit = -1; int threads = 1; bool noHeader = false, pin = true; long memMB = -1;
    for (int i = 1; i < argc; i++) {
        string a = argv[i];
        auto need = [&](const char* nm) { if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", nm); exit(1); } return string(argv[++i]); };
        if (a == "--time-limit") timeLimit = atof(need("--time-limit").c_str());
        else if (a == "--threads") threads = max(1, atoi(need("--threads").c_str()));
        else if (a == "--output") outPath = need("--output");
        else if (a == "--no-header") noHeader = true;
        else if (a == "--no-pin") pin = false;
        else if (a == "--mem-mb") memMB = atol(need("--mem-mb").c_str());
        else if (a == "-h" || a == "--help") { printf("usage: %s file.sop [--time-limit SECONDS] [--threads N] [--output out.csv] [--no-header] [--mem-mb MB] [--no-pin]\n", argv[0]); return 0; }
        else if (!a.empty() && a[0] == '-') { fprintf(stderr, "unknown option %s\n", a.c_str()); return 1; }
        else file = a;
    }
    if (file.empty()) { fprintf(stderr, "usage: %s file.sop [--time-limit SECONDS] [--threads N] [--output out.csv] [--no-header]\n", argv[0]); return 1; }
    string err;
    if (!parseSop(file, err)) { fprintf(stderr, "parse error: %s\n", err.c_str()); return 1; }
    double deadline = timeLimit > 0 ? timeLimit : 1e18;
    LOG("instance %s  N=%d  threads=%d\n", file.c_str(), n, threads);

    bool optimal = false; u64 totalNodes = 0;
    bool feasible = preprocess();
    // monitor thread: time limit + progress log
    atomic<bool> fin(false); mutex mm; condition_variable mcv;
    thread monitor([&]() {
        double lastLog = 0;
        unique_lock<mutex> lk(mm);
        while (!fin.load()) {
            mcv.wait_for(lk, chrono::milliseconds(50));
            double t = nowSec();
            if (t >= deadline && !g_stop.load()) { LOG("time limit reached\n"); requestStop(); }
            if (t - lastLog >= 10.0) { lastLog = t; LOG("[%.1fs] incumbent=%lld\n", t, g_ub.load() >= UBINF ? -1LL : (long long)g_ub.load()); }
        }
    });

    if (feasible) {
        // ---- root AP
        feasible = computeRoot();
        if (feasible) LOG("preprocessed: root AP lower bound = %lld  (%.2fs)\n", (long long)g_root.lb, nowSec());
        else if (!g_stop.load()) LOG("instance infeasible (no precedence-feasible Hamiltonian path)\n");
    } else LOG("instance infeasible (cyclic precedences)\n");

    if (feasible && !g_stop.load()) {
        Placement pl = planThreads(threads, pin);
        // ---- heuristic phase
        double hT = min(0.3 + (double)n * n * 4e-6, 5.0);
        if (timeLimit > 0) hT = min(hT, 0.1 * timeLimit);
        double hDead = nowSec() + hT;
        {
            vector<thread> th;
            for (int t = 0; t < threads; t++) th.emplace_back([&, t]() { pinTo(pl.cpu[t]); ILS h(0xC0FFEEULL + 7919ULL * t); h.run(hDead); });
            for (auto& x : th) x.join();
        }
        LOG("heuristic: UB = %lld  (%.2fs)  gap to AP root = %lld\n", g_ub.load() >= UBINF ? -1LL : (long long)g_ub.load(), nowSec(),
            g_ub.load() >= UBINF ? -1LL : (long long)(g_ub.load() - g_root.lb));
        if (g_ub.load() <= g_root.lb) optimal = !g_stop.load();   // matches the lower bound => optimal
        if (!optimal && !g_stop.load()) {
            // ---- exact phase
            size_t totalBytes;
            if (memMB > 0) totalBytes = (size_t)memMB << 20;
            else {
                long avail = 0; string mi = slurp("/proc/meminfo"); size_t p = mi.find("MemAvailable:");
                if (p != string::npos) avail = atol(mi.c_str() + p + 13);       // kB
                long mb = avail / 1024 / 4; if (mb < 64) mb = 64; if (mb > 16384) mb = 16384;
                totalBytes = (size_t)mb << 20;
            }
            int ng = pl.ngroups; vector<TT> tts(ng);
            for (int gI = 0; gI < ng; gI++) tts[gI].init(totalBytes / ng);
            LOG("branch & bound: %d thread(s), %d TT group(s), %zu MB transposition memory\n", threads, ng, totalBytes >> 20);
            P.T = threads; P.q.clear(); P.q.push_back(Task()); P.idle = 0; P.done = false;
            vector<Worker> ws(threads);
            vector<thread> th;
            for (int t = 0; t < threads; t++) th.emplace_back([&, t]() { pinTo(pl.cpu[t]); ws[t].id = t; ws[t].tt = &tts[pl.group[t]]; ws[t].run(); });
            for (auto& x : th) x.join();
            for (auto& w : ws) totalNodes += w.nodes;
            optimal = !g_stop.load();
        }
    }
    fin.store(true); { lock_guard<mutex> lk(mm); mcv.notify_all(); } monitor.join();
    if (g_stop.load()) optimal = false;

    // ---- final answer
    vector<int> best = g_bestPath; i64 bestCost = -1; string why;
    if (best.empty()) { optimal = false; }
    else if (!validate(best, bestCost, why)) { LOG("INTERNAL ERROR: final path invalid (%s)\n", why.c_str()); bestCost = -1; best.clear(); optimal = false; }
    else if (bestCost != g_ub.load()) LOG("warning: recomputed cost %lld differs from tracked cost %lld\n", (long long)bestCost, (long long)g_ub.load());
    if (!feasible && !g_stop.load()) optimal = false;
    double total = nowSec();

    string name = file; size_t sl = name.find_last_of("/\\"); if (sl != string::npos) name = name.substr(sl + 1);
    ostringstream row; row << name << "," << n << "," << fixed << setprecision(3) << total << "," << totalNodes << "," << bestCost << ",";
    for (size_t i = 0; i < best.size(); i++) row << (i ? " " : "") << best[i];
    row << "," << (optimal ? "yes" : "no") << "\n";
    const string header = "Instance,Instance Size,Time,Enumerated Nodes,Best Cost,Best Path,Optimal\n";
    if (!noHeader) fputs(header.c_str(), stdout);
    fputs(row.str().c_str(), stdout); fflush(stdout);
    if (!outPath.empty()) {
        struct stat st; bool needHeader = (stat(outPath.c_str(), &st) != 0) || st.st_size == 0;
        ofstream of(outPath, ios::app);
        if (!of) LOG("cannot open output file %s\n", outPath.c_str());
        else { if (needHeader && !noHeader) of << header; of << row.str(); }
    }
    LOG("done: cost=%lld optimal=%s nodes=%llu time=%.3fs\n", (long long)bestCost, optimal ? "yes" : "no", (unsigned long long)totalNodes, total);
    return 0;
}
