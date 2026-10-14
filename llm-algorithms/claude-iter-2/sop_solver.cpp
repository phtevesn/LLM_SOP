// =============================================================================
// sop_solver.cpp  --  Sequential Ordering Problem (SOP) solver
//
// Algorithm (see README.md for the full discussion):
//   1. Parse the TSPLIB-style .sop file (full matrix, negative entries encode
//      precedence constraints: T[i][j] < 0  =>  j must come before i).
//   2. Heuristic threads: iterated local search over segment-insertion
//      (path-preserving 3-exchange) moves with work queues, simulated-annealing
//      acceptance, restarts, and cooperation through a shared incumbent.
//   3. Exact thread (runs concurrently): precedence-aware arc elimination, a
//      cutting-plane LP (degree constraints + subtour cuts, own dense simplex)
//      for a strong root bound and reduced-cost fixing, then depth-first branch
//      and bound with an assignment-problem bound and a transposition table.
//      If it completes, the result is PROVEN OPTIMAL; otherwise a certified
//      lower bound and gap are reported.
//   4. The final path is validated from scratch (permutation, precedence, cost
//      recomputed) before it is reported.
//
// Build:  g++ -O2 -std=c++17 -pthread -o sop_solver sop_solver.cpp
// =============================================================================
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <set>
#include <queue>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace std;
using Clock = chrono::steady_clock;
typedef long long ll;
static const ll INF = numeric_limits<ll>::max() / 4;

static double secondsSince(const Clock::time_point& t0) {
    return chrono::duration<double>(Clock::now() - t0).count();
}

// -----------------------------------------------------------------------------
// Instance data
// -----------------------------------------------------------------------------
struct Instance {
    string file;                 // input file name (as shown in the CSV)
    string name;                 // NAME header (informational)
    int n = 0;                   // DIMENSION
    vector<ll> T;                // flat n*n matrix, T[i*n+j]
    vector<vector<int>> pred;    // direct predecessors:  j in pred[i]  <=>  T[i][j] < 0
    vector<vector<int>> succ;    // direct successors:    i in succ[j]
    vector<vector<uint64_t>> ancB; // transitive closure as bitsets: bit j of ancB[i] = 1 if j must precede i
    bool isAnc(int i, int j) const { return (ancB[i][j >> 6] >> (j & 63)) & 1; }

    ll at(int i, int j) const { return T[(size_t)i * n + j]; }

    // Build pred/succ lists and the transitive closure.  Returns false if the
    // precedence relation contains a cycle (instance infeasible).
    bool buildPrecedence() {
        pred.assign(n, {});
        succ.assign(n, {});
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++)
                if (i != j && at(i, j) < 0) {
                    pred[i].push_back(j);
                    succ[j].push_back(i);
                }
        // Kahn topological sort, building ancestor sets along the way.
        int W = (n + 63) / 64;
        ancB.assign(n, vector<uint64_t>(W, 0));
        vector<int> indeg(n), stack;
        for (int v = 0; v < n; v++) {
            indeg[v] = (int)pred[v].size();
            if (indeg[v] == 0) stack.push_back(v);
        }
        int seen = 0;
        while (!stack.empty()) {
            int v = stack.back();
            stack.pop_back();
            seen++;
            for (int u : pred[v]) {
                ancB[v][u >> 6] |= 1ULL << (u & 63);
                for (int k = 0; k < W; k++) ancB[v][k] |= ancB[u][k];
            }
            for (int s : succ[v])
                if (--indeg[s] == 0) stack.push_back(s);
        }
        return seen == n;
    }
};

// -----------------------------------------------------------------------------
// Parsing
// -----------------------------------------------------------------------------
static string trim(const string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
static string upper(string s) {
    for (auto& c : s) c = (char)toupper((unsigned char)c);
    return s;
}
static string baseName(const string& p) {
    size_t k = p.find_last_of("/\\");
    return k == string::npos ? p : p.substr(k + 1);
}

// Parse a .sop file.  Header lines are read by keyword until
// EDGE_WEIGHT_SECTION; afterwards the rest of the file is read as integers.
// Reading stops at the first non-numeric token (e.g. "EOF") or at end of file,
// so a missing EOF line is fine.  The matrix may wrap across lines freely.
static bool parseSop(const string& path, Instance& I, string& err) {
    ifstream in(path);
    if (!in) { err = "cannot open file"; return false; }
    I.file = baseName(path);
    string line;
    int dim = -1;
    bool inSection = false;
    while (getline(in, line)) {
        line = trim(line);
        if (line.empty()) continue;
        string key, val;
        size_t c = line.find(':');
        if (c != string::npos) { key = trim(line.substr(0, c)); val = trim(line.substr(c + 1)); }
        else key = line;
        key = upper(key);
        if (key == "EDGE_WEIGHT_SECTION") { inSection = true; break; }
        if (key == "NAME") I.name = val;
        else if (key == "DIMENSION") dim = atoi(val.c_str());
        else if (key == "EDGE_WEIGHT_FORMAT" && upper(val) != "FULL_MATRIX") {
            err = "unsupported EDGE_WEIGHT_FORMAT '" + val + "' (only FULL_MATRIX)";
            return false;
        }
        // TYPE, COMMENT, EDGE_WEIGHT_TYPE and unknown keys are metadata: ignored.
    }
    if (!inSection) { err = "EDGE_WEIGHT_SECTION not found"; return false; }

    vector<ll> vals;
    string tok;
    while (in >> tok) {
        char* endp = nullptr;
        ll v = strtoll(tok.c_str(), &endp, 10);
        if (endp == tok.c_str() || *endp != '\0') break;   // "EOF" or junk: stop
        vals.push_back(v);
    }
    if (dim <= 0 && !vals.empty()) dim = (int)vals[0];
    if (dim <= 0) { err = "missing/invalid DIMENSION"; return false; }
    size_t N2 = (size_t)dim * dim;
    size_t off = 0;
    // The section starts with the dimension repeated; skip it if present.
    if (vals.size() >= N2 + 1 && vals[0] == dim) off = 1;
    if (vals.size() < off + N2) {
        err = "matrix too short: expected " + to_string(N2) + " values, found " +
              to_string(vals.size() - off);
        return false;
    }
    I.n = dim;
    I.T.assign(vals.begin() + off, vals.begin() + off + N2);
    return true;
}

// -----------------------------------------------------------------------------
// Validation: permutation, precedence, cost recomputed from scratch.
// -----------------------------------------------------------------------------
static bool validatePath(const Instance& I, const vector<int>& p, ll& cost, string& why) {
    int n = I.n;
    if ((int)p.size() != n) { why = "path length != N"; return false; }
    vector<int> pos(n, -1);
    for (int k = 0; k < n; k++) {
        if (p[k] < 0 || p[k] >= n) { why = "node index out of range"; return false; }
        if (pos[p[k]] != -1) { why = "node repeated"; return false; }
        pos[p[k]] = k;
    }
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            if (i != j && I.at(i, j) < 0 && pos[j] > pos[i]) {
                why = "precedence violated: " + to_string(j) + " must precede " + to_string(i);
                return false;
            }
    cost = 0;
    for (int k = 0; k + 1 < n; k++) {
        ll c = I.at(p[k], p[k + 1]);
        if (c < 0) { why = "negative edge used"; return false; }
        cost += c;
    }
    return true;
}

// -----------------------------------------------------------------------------
// Shared incumbent: the best path found so far by any thread, plus a stop flag.
// The exact solver reads the upper bound from here at every node, so heuristic
// improvements immediately tighten its pruning.
// -----------------------------------------------------------------------------
struct Shared {
    atomic<ll> ub{INF};
    atomic<bool> stop{false};
    mutex m;
    vector<int> path;

    // Offer a solution; returns true if it became the new incumbent.
    bool offer(ll cost, const vector<int>& p) {
        if (cost >= ub.load(memory_order_relaxed)) return false;
        lock_guard<mutex> lk(m);
        if (cost >= ub.load()) return false;
        path = p;
        ub.store(cost);
        return true;
    }
    // Copy the incumbent if it is strictly better than `than`.
    bool fetchIfBetter(ll than, vector<int>& p, ll& cost) {
        if (ub.load(memory_order_relaxed) >= than) return false;
        lock_guard<mutex> lk(m);
        if (ub.load() >= than) return false;
        p = path; cost = ub.load();
        return true;
    }
};

// -----------------------------------------------------------------------------
// Heuristic: iterated local search with segment insertion (path-preserving
// 3-exchange) and don't-look-bit style work queues.
//
// Neighbourhood: remove a block of consecutive nodes of ANY length (up to
// maxSeg) and re-insert it, in the same orientation, in another gap of the path.
// Orientation is kept because costs are asymmetric; this is the reversal-free
// 3-exchange used by SOP-specific heuristics (HAS-SOP's SOP-3-exchange).  Moves
// are scored in O(1) and checked for precedence feasibility by looking only at
// the nodes the block jumps over: direct constraints suffice, because any
// violated chain must contain a violated direct arc between the block and a
// jumped node.
//
// Work queue: after a perturbation or an applied move only the nodes at changed
// edges are re-examined (as segment start and as segment end), so one ILS round
// costs O(touched * maxSeg * N) instead of a full O(N^2 * maxSeg) pass.  This is
// what keeps the search fast on large instances.
//
// Metaheuristic: perturb with random feasible segment moves, re-optimize, accept
// with a simulated-annealing rule, return to the best (or to the globally best
// path of another thread) after a stale period, and restart from a fresh
// randomized greedy path now and then.
// -----------------------------------------------------------------------------
struct Heuristic {
    const Instance& I;
    int n;
    mt19937_64 rng;
    vector<int> order, pos;
    ll cost = INF;
    int maxSeg;
    Shared* sh = nullptr;
    vector<int> work;              // nodes whose neighbourhood must be (re)examined
    vector<char> inWork;
    const Clock::time_point* t0p = nullptr;   // set by run(): lets localSearch honour the deadline
    double dlSec = 1e18;

    Heuristic(const Instance& inst, uint64_t seed) : I(inst), n(inst.n), rng(seed) {
        maxSeg = min(n, max(12, min(40, n / 2)));
        inWork.assign(n, 0);
    }

    ll e(int a, int b) const { return (a < 0 || b < 0) ? 0 : I.at(a, b); }
    void computePos() {
        pos.assign(n, 0);
        for (int k = 0; k < n; k++) pos[order[k]] = k;
    }
    ll recost() const {
        ll c = 0;
        for (int k = 0; k + 1 < n; k++) c += I.at(order[k], order[k + 1]);
        return c;
    }
    void push(int v) {
        if (v >= 0 && !inWork[v]) { inWork[v] = 1; work.push_back(v); }
    }
    void pushAll() {
        for (int v = 0; v < n; v++) push(order[v]);
        shuffle(work.begin(), work.end(), rng);
    }

    // Randomized nearest-neighbour: pick among the `rcl` cheapest available nodes.
    bool construct(int rcl) {
        vector<int> indeg(n), avail;
        for (int v = 0; v < n; v++) {
            indeg[v] = (int)I.pred[v].size();
            if (indeg[v] == 0) avail.push_back(v);
        }
        order.clear();
        int last = -1;
        while ((int)order.size() < n) {
            if (avail.empty()) return false;
            int pick;
            if (last < 0) pick = (int)(rng() % avail.size());
            else {
                vector<pair<ll, int>> cand;
                for (int k = 0; k < (int)avail.size(); k++) cand.push_back({I.at(last, avail[k]), k});
                sort(cand.begin(), cand.end());
                int lim = min<int>(rcl, (int)cand.size());
                pick = cand[rng() % lim].second;
            }
            int v = avail[pick];
            avail[pick] = avail.back();
            avail.pop_back();
            order.push_back(v);
            last = v;
            for (int s : I.succ[v])
                if (--indeg[s] == 0) avail.push_back(s);
        }
        computePos();
        cost = recost();
        return true;
    }

    // Move block [i..j] into gap g (gap g lies between order[g-1] and order[g]).
    void applyMove(int i, int j, int g) {
        if (g > j) rotate(order.begin() + i, order.begin() + j + 1, order.begin() + g);
        else       rotate(order.begin() + g, order.begin() + i, order.begin() + j + 1);
        computePos();
    }

    // Collect all precedence-feasible gaps for block [i..j].
    void feasibleGaps(int i, int j, vector<int>& gaps) const {
        gaps.clear();
        for (int g = j + 2; g <= n; g++) {                // forward: jump over order[g-1]
            int w = order[g - 1];
            bool bad = false;
            for (int u : I.pred[w]) if (pos[u] >= i && pos[u] <= j) { bad = true; break; }
            if (bad) break;
            gaps.push_back(g);
        }
        for (int g = i - 1; g >= 0; g--) {                // backward: jump over order[g]
            int w = order[g];
            bool bad = false;
            for (int u : I.succ[w]) if (pos[u] >= i && pos[u] <= j) { bad = true; break; }
            if (bad) break;
            gaps.push_back(g);
        }
    }

    // Best feasible re-insertion gap for block [i..j]; returns its delta (<0 if improving).
    ll bestGap(int i, int j, int& bestG) const {
        int s0 = order[i], s1 = order[j];
        int prev = i > 0 ? order[i - 1] : -1;
        int next = j + 1 < n ? order[j + 1] : -1;
        ll removeGain = e(prev, s0) + e(s1, next) - e(prev, next);
        ll bestDelta = 0;
        bestG = -1;
        for (int g = j + 2; g <= n; g++) {
            int w = order[g - 1];
            bool bad = false;
            for (int u : I.pred[w]) if (pos[u] >= i && pos[u] <= j) { bad = true; break; }
            if (bad) break;
            int b = g < n ? order[g] : -1;
            ll delta = e(w, s0) + e(s1, b) - e(w, b) - removeGain;
            if (delta < bestDelta) { bestDelta = delta; bestG = g; }
        }
        for (int g = i - 1; g >= 0; g--) {
            int w = order[g];
            bool bad = false;
            for (int u : I.succ[w]) if (pos[u] >= i && pos[u] <= j) { bad = true; break; }
            if (bad) break;
            int a = g > 0 ? order[g - 1] : -1;
            ll delta = e(a, s0) + e(s1, w) - e(a, w) - removeGain;
            if (delta < bestDelta) { bestDelta = delta; bestG = g; }
        }
        return bestDelta;
    }

    // Try to improve using blocks that start or end at node s.  Applies the first
    // improving move found and queues the nodes at all changed edges.
    bool tryNode(int s) {
        int p = pos[s];
        for (int mode = 0; mode < 2; mode++) {
            for (int L = 1; L <= maxSeg; L++) {
                int i = mode == 0 ? p : p - L + 1;
                int j = i + L - 1;
                if (i < 0 || j >= n) break;
                int g;
                ll delta = bestGap(i, j, g);
                if (g >= 0 && delta < 0) {
                    int s0 = order[i], s1 = order[j];
                    int prev = i > 0 ? order[i - 1] : -1;
                    int next = j + 1 < n ? order[j + 1] : -1;
                    int a = g > 0 ? order[g - 1] : -1;
                    int b = g < n ? order[g] : -1;
                    applyMove(i, j, g);
                    cost += delta;
                    push(prev); push(next); push(s0); push(s1); push(a); push(b);
                    return true;
                }
            }
        }
        return false;
    }

    // Process the work queue until no queued node can improve the path.
    void localSearch() {
        unsigned counter = 0;
        while (!work.empty()) {
            if (t0p && ((++counter) & 15) == 0 && stopped(*t0p, dlSec)) {      // out of time: drop the queue
                for (int v : work) inWork[v] = 0;
                work.clear();
                return;
            }
            int s = work.back();
            work.pop_back();
            inWork[s] = 0;
            while (tryNode(s)) {}
        }
    }

    // Random feasible segment moves (perturbation); queues the touched nodes.
    void perturb() {
        int moves = 1 + (int)(rng() % 3);
        vector<int> gaps;
        int lmax = max(1, min(n / 2, 20));
        for (int m = 0; m < moves; m++) {
            int L = 1 + (int)(rng() % lmax);
            if (L > n) L = n;
            int i = (int)(rng() % (n - L + 1));
            int j = i + L - 1;
            feasibleGaps(i, j, gaps);
            if (gaps.empty()) continue;
            int g = gaps[rng() % gaps.size()];
            int prev = i > 0 ? order[i - 1] : -1;
            int next = j + 1 < n ? order[j + 1] : -1;
            int s0 = order[i], s1 = order[j];
            int a = g > 0 ? order[g - 1] : -1;
            int b = g < n ? order[g] : -1;
            applyMove(i, j, g);
            push(prev); push(next); push(s0); push(s1); push(a); push(b);
        }
        cost = recost();
    }

    bool stopped(const Clock::time_point& t0, double deadline) const {
        return secondsSince(t0) >= deadline || (sh && sh->stop.load(memory_order_relaxed));
    }

    // Iterated local search until `deadline` (seconds since t0), until the shared
    // stop flag is set, or until `maxStale` consecutive rounds fail to improve the
    // best.  If `start` is given the search begins from that path.  The best path
    // is returned in bestOrder/bestCost and published to the shared incumbent.
    bool run(const Clock::time_point& t0, double deadline, long long maxStale,
             vector<int>& bestOrder, ll& bestCost, const vector<int>* start = nullptr) {
        t0p = &t0; dlSec = deadline;
        if (start) { order = *start; computePos(); cost = recost(); }
        else if (!construct(1)) return false;
        if (n <= 2) {
            bestOrder = order; bestCost = cost;
            if (sh) sh->offer(bestCost, bestOrder);
            return true;
        }
        pushAll();
        localSearch();
        bestOrder = order; bestCost = cost;
        if (sh) sh->offer(bestCost, bestOrder);
        double temp = max(1.0, 1.0 * (double)bestCost / (n - 1));
        uniform_real_distribution<double> U(0.0, 1.0);
        long long stale = 0;
        vector<int> prevOrder;
        while (stale < maxStale && !stopped(t0, deadline)) {
            prevOrder = order;
            ll prevCost = cost;
            perturb();
            localSearch();
            if (cost < bestCost) {
                bestCost = cost; bestOrder = order; stale = 0;
                if (sh) sh->offer(bestCost, bestOrder);
            } else stale++;
            if (cost > prevCost && U(rng) > exp(-(double)(cost - prevCost) / temp)) {
                order = prevOrder; computePos(); cost = prevCost;      // reject
            }
            if (stale > 0 && stale % (100LL * n) == 0) {                // back to best
                vector<int> sp; ll sc;
                if (sh && sh->fetchIfBetter(bestCost, sp, sc)) { bestOrder = sp; bestCost = sc; }  // adopt global best
                order = bestOrder; computePos(); cost = bestCost;
            }
            if (stale > 0 && stale % (400LL * n) == 0) {                // fresh restart
                if (construct(3)) { pushAll(); localSearch(); }
            }
        }
        return true;
    }
};

// -----------------------------------------------------------------------------
// Dinic max-flow on a small dense-ish graph with double capacities.  Used to
// separate subtour-elimination cuts from a fractional LP solution.
// -----------------------------------------------------------------------------
struct MaxFlow {
    struct E { int to; double cap; };
    int N;
    vector<E> es;
    vector<vector<int>> g;
    vector<int> level, it;
    explicit MaxFlow(int n) : N(n), g(n), level(n), it(n) {}
    void addArc(int u, int v, double c) {
        g[u].push_back((int)es.size()); es.push_back({v, c});
        g[v].push_back((int)es.size()); es.push_back({u, 0.0});
    }
    bool bfs(int s, int t) {
        fill(level.begin(), level.end(), -1);
        queue<int> q; q.push(s); level[s] = 0;
        while (!q.empty()) {
            int u = q.front(); q.pop();
            for (int id : g[u])
                if (es[id].cap > 1e-9 && level[es[id].to] < 0) { level[es[id].to] = level[u] + 1; q.push(es[id].to); }
        }
        return level[t] >= 0;
    }
    double dfs(int u, int t, double f) {
        if (u == t) return f;
        for (int& k = it[u]; k < (int)g[u].size(); k++) {
            int id = g[u][k];
            int v = es[id].to;
            if (es[id].cap > 1e-9 && level[v] == level[u] + 1) {
                double d = dfs(v, t, min(f, es[id].cap));
                if (d > 1e-12) { es[id].cap -= d; es[id ^ 1].cap += d; return d; }
            }
        }
        return 0;
    }
    double run(int s, int t) {
        double flow = 0;
        while (bfs(s, t)) {
            fill(it.begin(), it.end(), 0);
            double f;
            while ((f = dfs(s, t, 1e18)) > 1e-12) flow += f;
        }
        return flow;
    }
    // Nodes reachable from s in the residual graph (call after run()).
    vector<char> reachableFrom(int s) {
        vector<char> seen(N, 0);
        queue<int> q; q.push(s); seen[s] = 1;
        while (!q.empty()) {
            int u = q.front(); q.pop();
            for (int id : g[u])
                if (es[id].cap > 1e-9 && !seen[es[id].to]) { seen[es[id].to] = 1; q.push(es[id].to); }
        }
        return seen;
    }
    // Nodes that can reach t in the residual graph (call after run()).
    vector<char> reachingTo(int t) {
        vector<char> seen(N, 0);
        queue<int> q; q.push(t); seen[t] = 1;
        while (!q.empty()) {
            int v = q.front(); q.pop();
            for (int id : g[v]) {                     // id: v->u ; reverse arc id^1 : u->v
                int u = es[id].to;
                if (es[id ^ 1].cap > 1e-9 && !seen[u]) { seen[u] = 1; q.push(u); }
            }
        }
        return seen;
    }
};

// -----------------------------------------------------------------------------
// Cutting-plane LP for the root bound: the directed Dantzig-Fulkerson-Johnson
// relaxation of the path problem, written as a Hamiltonian-cycle problem on the
// N real nodes plus a dummy node D (D->s for possible starts, e->D for possible
// ends, cost 0):
//
//     min  sum c_a x_a
//     s.t. out-degree(i) = 1, in-degree(i) = 1   for all nodes (one row dropped
//                                                 as redundant)
//          x(arcs entering S) >= 1               for node sets S not containing D
//                                                 (subtour elimination, added by
//                                                 separation with max-flow)
//          x >= 0.
//
// Arcs already eliminated by precedence reasoning are simply absent.  The LP is
// solved with a dense tableau: primal two-phase simplex for the degree system,
// then dual simplex after each batch of cuts.  Dual simplex keeps dual
// feasibility, so after every pivot the objective is a valid lower bound and the
// reduced costs are valid for fixing arcs -- which is what the branch and bound
// uses to shrink the problem before searching.
// -----------------------------------------------------------------------------
class CutLP {
public:
    struct Arc { int u, v; };
    int m, A, Rb, maxCuts, R, ncuts, usedCols, stride;
    vector<Arc> arcs;
    vector<double> cost;
    vector<double> T, rhs, d;
    vector<int> basis;
    vector<char> banned;

    CutLP(int m_, const vector<Arc>& arcs_, const vector<double>& cost_, int maxCuts_)
        : m(m_), A((int)arcs_.size()), maxCuts(maxCuts_), ncuts(0), arcs(arcs_), cost(cost_) {
        Rb = 2 * m - 1;
        stride = A + Rb + maxCuts;
        T.assign((size_t)(Rb + maxCuts) * stride, 0.0);
        rhs.assign(Rb + maxCuts, 0.0);
        basis.assign(Rb + maxCuts, -1);
        d.assign(stride, 0.0);
        banned.assign(stride, 0);
        for (int j = 0; j < A; j++) {
            T[(size_t)arcs[j].u * stride + j] = 1.0;                       // out-degree row of tail
            if (arcs[j].v < m - 1) T[(size_t)(m + arcs[j].v) * stride + j] = 1.0;  // in-degree row of head
        }
        for (int r = 0; r < Rb; r++) {
            T[(size_t)r * stride + A + r] = 1.0;
            rhs[r] = 1.0;
            basis[r] = A + r;
            banned[A + r] = 1;                                              // artificials never enter
        }
        R = Rb;
        usedCols = A + Rb;
    }

    static size_t memoryNeeded(int m_, int A_, int maxCuts_) {
        return (size_t)(2 * m_ - 1 + maxCuts_) * (size_t)(A_ + 2 * m_ - 1 + maxCuts_);
    }

    double* row(int r) { return &T[(size_t)r * stride]; }

    void pivot(int r, int e) {
        double* pr = row(r);
        double inv = 1.0 / pr[e];
        static thread_local vector<int> nz;
        nz.clear();
        for (int j = 0; j < usedCols; j++) {
            if (pr[j] != 0.0) { pr[j] *= inv; nz.push_back(j); }
        }
        pr[e] = 1.0;
        rhs[r] *= inv;
        for (int i = 0; i < R; i++) {
            if (i == r) continue;
            double* pi = row(i);
            double f = pi[e];
            if (f == 0.0) continue;
            for (int j : nz) pi[j] -= f * pr[j];
            pi[e] = 0.0;
            rhs[i] -= f * rhs[r];
            if (fabs(rhs[i]) < 1e-11) rhs[i] = 0.0;
        }
        double f = d[e];
        if (f != 0.0) {
            for (int j : nz) d[j] -= f * pr[j];
            d[e] = 0.0;
        }
        basis[r] = e;
    }

    // Primal simplex on the current tableau (d must be consistent).
    // 0 = optimal, 1 = unbounded, 2 = time/iteration limit.
    int primal(const Clock::time_point& dl, long maxIter) {
        int degen = 0;
        for (long it = 0; it < maxIter; it++) {
            if ((it & 31) == 0 && Clock::now() >= dl) return 2;
            bool bland = degen > 40;
            int e = -1;
            double best = -1e-9;
            for (int j = 0; j < usedCols; j++) {
                if (banned[j]) continue;
                if (d[j] < best) { best = d[j]; e = j; if (bland) break; }
            }
            if (e < 0) return 0;
            int r = -1;
            double br = 1e300;
            for (int i = 0; i < R; i++) {
                double a = T[(size_t)i * stride + e];
                if (a > 1e-9) {
                    double ratio = rhs[i] / a;
                    if (r < 0 || ratio < br - 1e-12 ||
                        (ratio < br + 1e-12 && (bland ? basis[i] < basis[r] : a > T[(size_t)r * stride + e]))) {
                        br = ratio; r = i;
                    }
                }
            }
            if (r < 0) return 1;
            degen = (br < 1e-12) ? degen + 1 : 0;
            pivot(r, e);
        }
        return 2;
    }

    // Dual simplex: restores primal feasibility after cuts, keeping d >= 0.
    // 0 = optimal, 1 = infeasible, 2 = time/iteration limit.
    int dual(const Clock::time_point& dl, long maxIter) {
        for (long it = 0; it < maxIter; it++) {
            if ((it & 31) == 0 && Clock::now() >= dl) return 2;
            int r = -1;
            double worst = -1e-9;
            for (int i = 0; i < R; i++) if (rhs[i] < worst) { worst = rhs[i]; r = i; }
            if (r < 0) return 0;
            int e = -1;
            double br = 1e300;
            const double* pr = row(r);
            for (int j = 0; j < usedCols; j++) {
                if (banned[j]) continue;
                double a = pr[j];
                if (a < -1e-9) {
                    double ratio = max(d[j], 0.0) / (-a);
                    if (e < 0 || ratio < br - 1e-12 || (ratio < br + 1e-12 && -a > -pr[e])) { br = ratio; e = j; }
                }
            }
            if (e < 0) return 1;
            pivot(r, e);
        }
        return 2;
    }

    double objective() const {
        double z = 0;
        for (int r = 0; r < R; r++) if (basis[r] >= 0 && basis[r] < A) z += cost[basis[r]] * rhs[r];
        return z;
    }

    // Two-phase solve of the degree system.  False on failure/timeout.
    bool initialSolve(const Clock::time_point& dl) {
        // Phase 1: minimise the sum of artificials.
        fill(d.begin(), d.end(), 0.0);
        for (int j = 0; j < usedCols; j++) {
            double s = 0;
            for (int r = 0; r < Rb; r++) s += T[(size_t)r * stride + j];
            d[j] = (j >= A ? 1.0 : 0.0) - s;
        }
        int st = primal(dl, 2000000);
        if (st != 0) return false;
        double infeas = 0;
        for (int r = 0; r < R; r++) if (basis[r] >= A) infeas += rhs[r];
        if (infeas > 1e-6) return false;
        // Drive remaining (zero-level) artificials out of the basis where possible.
        for (int r = 0; r < Rb; r++) {
            if (basis[r] < A) continue;
            int e = -1;
            double best = 1e-7;
            for (int j = 0; j < A; j++) {
                double a = fabs(T[(size_t)r * stride + j]);
                if (a > best) { best = a; e = j; }
            }
            if (e >= 0) pivot(r, e);
        }
        // Phase 2: true costs.
        for (int j = 0; j < usedCols; j++) {
            double c = j < A ? cost[j] : 0.0;
            double s = 0;
            for (int r = 0; r < R; r++) {
                int b = basis[r];
                double cb = (b >= 0 && b < A) ? cost[b] : 0.0;
                if (cb != 0.0) s += cb * T[(size_t)r * stride + j];
            }
            d[j] = c - s;
        }
        st = primal(dl, 2000000);
        return st == 0;
    }

    // Add the row  sum coef_j x_j >= 1  and express it in the current basis.
    bool addCut(const vector<pair<int, double>>& coef) {
        if (ncuts >= maxCuts) return false;
        int r = R, s = A + Rb + ncuts;
        ncuts++; R++; usedCols++;
        double* pr = row(r);
        for (int j = 0; j < usedCols; j++) pr[j] = 0.0;
        for (auto& cj : coef) pr[cj.first] += cj.second;
        pr[s] = -1.0;
        rhs[r] = 1.0;
        for (int i = 0; i < r; i++) {
            int b = basis[i];
            if (b < 0 || b >= A) continue;
            double f = pr[b];
            if (f == 0.0) continue;
            const double* pi = row(i);
            for (int j = 0; j < usedCols; j++) pr[j] -= f * pi[j];
            pr[b] = 0.0;
            rhs[r] -= f * rhs[i];
        }
        for (int j = 0; j < usedCols; j++) pr[j] = -pr[j];     // make the surplus basic with +1
        rhs[r] = -rhs[r];
        basis[r] = s;
        d[s] = 0.0;
        return true;
    }

    // Recompute all reduced costs from the current basis (after costs changed).
    void reprice() {
        vector<double> cb(R, 0.0);
        for (int r = 0; r < R; r++) {
            int b = basis[r];
            cb[r] = (b >= 0 && b < A) ? cost[b] : 0.0;
        }
        for (int j = 0; j < usedCols; j++) {
            double s = 0;
            for (int r = 0; r < R; r++) if (cb[r] != 0.0) s += cb[r] * T[(size_t)r * stride + j];
            d[j] = (j < A ? cost[j] : 0.0) - s;
        }
    }

    // Forbid arc j: make it very expensive and keep it out of the basis from now on.
    void ban(int j, double bigCost) {
        cost[j] = bigCost;
        banned[j] = 1;
    }

    vector<double> primalValues() const {
        vector<double> x(A, 0.0);
        for (int r = 0; r < R; r++) if (basis[r] >= 0 && basis[r] < A) x[basis[r]] = rhs[r];
        return x;
    }
};
// -----------------------------------------------------------------------------
// Exact phase: preprocessing + cutting-plane LP fixing + depth-first B&B.
//
//   1. Arc elimination.  Arc u->v is removed when T[u][v] < 0, when v must
//      precede u, or when some node w must lie strictly between u and v in the
//      precedence order (u and v could then not be adjacent).
//   2. Root LP (CutLP): degree constraints plus subtour-elimination cuts found
//      by max-flow separation.  Gives a strong lower bound; if it reaches the
//      incumbent the incumbent is optimal.  Otherwise every arc whose reduced
//      cost pushes any path through it to >= incumbent is removed (reduced-cost
//      fixing).  Dual feasibility makes this valid even if the LP is stopped
//      early.  The cheaper assignment-problem bound with the same fixing is run
//      afterwards (and is the fallback when the LP is too large).
//   3. Depth-first branch and bound over what is left.  A node is a partial path
//      from a node without predecessors; children append one available node,
//      cheapest edge first.  Bound: assignment relaxation (Hungarian) of the
//      remaining path over surviving arcs.  Dominance: transposition table on
//      (visited set, last node) with 128-bit Zobrist keys.  The upper bound is
//      read from the shared incumbent, so heuristic threads tighten the search.
//   If the search completes, the incumbent is proven optimal; otherwise the
//   root bound still certifies a gap.
// -----------------------------------------------------------------------------
struct BBResult {
    bool exact = false;        // search finished (optimality proven)
    ll nodes = 0;              // "Enumerated Nodes": search nodes entered
    ll rootLB = 0;             // proven lower bound on the optimum
    ll lpLB = -1;              // LP bound (-1 if the LP was skipped)
    int arcsRemoved = 0;       // arcs eliminated by preprocessing + fixing
    int cuts = 0;              // subtour cuts added
    ll bcNodes = 0;            // LP nodes of the branch and cut (included in `nodes`)
};

class BranchAndBound {
public:
    BranchAndBound(const Instance& inst, int ttBits, uint64_t seed) : I(inst), n(inst.n) {
        mt19937_64 rg(seed ^ 0xabcdef12345ULL);
        for (int v = 0; v < n; v++) { zA.push_back(rg()); zLB.push_back(rg()); zB.push_back(rg()); }
        ttBits_ = ttBits;
        dependents.assign(n, {});
        for (int u = 0; u < n; u++)
            for (int w = 0; w < n; w++)
                if (I.isAnc(w, u)) dependents[u].push_back(w);   // w must come after u
        buildArcs();
    }

    BBResult run(Shared& shared, const Clock::time_point& deadlineTp, bool useLP, bool useBC, int dfsThreads = 1) {
        sh = &shared;
        deadline = deadlineTp;
        res = BBResult();
        timedOut = false;
        ll UB = sh->ub.load();
        ll best = 0;

        if (useLP) {
            ll l = lpStage(UB);
            if (l >= 0) { res.lpLB = l; best = max(best, l); }
            if (l >= UB) return finishProven(UB);
        }
        if (Clock::now() >= deadline) { res.rootLB = min(best, UB); countRemoved(); return res; }
        ll lb = rootFixing(sh->ub.load());
        if (lb < INF) best = max(best, lb);
        countRemoved();
        res.rootLB = min(best, UB);
        if (lb >= INF || lb >= sh->ub.load() || best >= sh->ub.load()) return finishProven(sh->ub.load());

        // Portfolio: the LP-based branch and cut runs in its own thread (on a copy
        // of the preprocessed state) while this thread runs the assignment-bound
        // tree search below.  Whichever proves optimality first stops the other.
        unique_ptr<BranchAndBound> bcSolver;
        thread bcThread;
        if (lpOk && useBC) {
            bcSolver = make_unique<BranchAndBound>(*this);
            bcSolver->res = BBResult();
            bcSolver->timedOut = false;
            BranchAndBound* b = bcSolver.get();
            bcThread = thread([b]() {
                bool ran = b->bcPhase();
                b->bcExact = ran && ((!b->timedOut && !b->incomplete) || b->bcInfeasible);
                if (b->bcExact) b->sh->stop.store(true);
            });
        }
        // Parallel tree search: the top of the tree is cut into tasks (short path
        // prefixes) that worker threads take from a shared counter.  Every worker
        // owns a private copy of the search state and a private transposition table
        // (the memory budget is split among them).
        int W = max(1, dfsThreads);
        int bitsW = ttBits_;
        for (int w = W; w > 1; w >>= 1) bitsW--;
        bitsW = max(14, bitsW);
        vector<unique_ptr<BranchAndBound>> workers;
        for (int k = 1; k < W; k++) {
            workers.push_back(make_unique<BranchAndBound>(*this));
            workers.back()->res = BBResult();
            workers.back()->timedOut = false;
        }
        initSearchState(bitsW);
        vector<Task> tasks = makeTasks(W);
        atomic<int> nextTask{0};
        vector<thread> wthreads;
        for (auto& w : workers) {
            BranchAndBound* wp = w.get();
            wthreads.emplace_back([wp, &tasks, &nextTask, bitsW]() {
                wp->initSearchState(bitsW);
                wp->workTasks(tasks, nextTask);
            });
        }
        workTasks(tasks, nextTask);
        for (auto& t : wthreads) t.join();
        bool allDone = !timedOut;
        for (auto& w : workers) {
            res.nodes += w->res.nodes;
            if (w->timedOut) allDone = false;
        }
        timedOut = !allDone;
        bool dfsExact = !timedOut;
        if (dfsExact) sh->stop.store(true);
        if (bcThread.joinable()) bcThread.join();
        if (bcSolver) {
            res.nodes += bcSolver->res.nodes;
            res.bcNodes = bcSolver->res.nodes;
        }
        res.exact = dfsExact || (bcSolver && bcSolver->bcExact);
        return res;
    }

private:
    const Instance& I;
    int n;
    Shared* sh = nullptr;
    Clock::time_point deadline;
    bool timedOut = false;
    BBResult res;
    vector<int> indeg, path;
    vector<char> visited;
    vector<uint64_t> zA, zB, zLB;
    vector<vector<int>> dependents;
    vector<vector<uint64_t>> descB;    // bit v of descB[u]: v must come after u
    vector<pair<int, int>> coverPairs; // (k,l): k must precede l with nothing forced in between
    // LP reduced costs (>= 0) from the final dual-feasible basis.  For every
    // feasible path, cost >= lpZ + sum of reduced costs of its arcs.
    bool haveRC = false;
    double lpZ = 0;
    vector<double> rcArc, rcStart, rcEnd;
    vector<double> hud, hvd, minvd, cbufd;   // double Hungarian scratch
    vector<char> ok;                   // ok[u*n+v]: arc u->v still allowed
    vector<char> canStart, canEnd;
    struct Entry { uint64_t a, b; ll g; };
    vector<Entry> tt;
    uint64_t ttMask = 0;
    int ttBits_ = 22;
    vector<ll> cbuf, hu, hv, minv;     // Hungarian scratch
    vector<int> hp, way;
    vector<char> used;
    static constexpr ll BIG = 1000000000000LL;

    BBResult finishProven(ll ub) {
        res.exact = true;
        res.nodes = max<ll>(res.nodes, 1);
        res.rootLB = ub;
        countRemoved();
        return res;
    }
    void countRemoved() {
        res.arcsRemoved = 0;
        for (size_t k = 0; k < ok.size(); k++) if (!ok[k]) res.arcsRemoved++;
    }

    // Static arc elimination from the precedence structure.
    void buildArcs() {
        ok.assign((size_t)n * n, 0);
        canStart.assign(n, 0);
        canEnd.assign(n, 0);
        int words = (n + 63) / 64;
        descB.assign(n, vector<uint64_t>(words, 0));
        for (int v = 0; v < n; v++)
            for (int u = 0; u < n; u++)
                if (I.isAnc(v, u)) descB[u][v >> 6] |= 1ULL << (v & 63);   // v comes after u
        // Covering precedence pairs (transitive reduction): k < l with no w in between.
        coverPairs.clear();
        for (int l = 0; l < n; l++)
            for (int k = 0; k < n; k++) {
                if (!I.isAnc(l, k)) continue;
                bool between = false;
                for (int w = 0; w < words; w++)
                    if (descB[k][w] & I.ancB[l][w]) { between = true; break; }
                if (!between) coverPairs.push_back({k, l});
            }
        for (int u = 0; u < n; u++)
            for (int v = 0; v < n; v++) {
                if (u == v || I.at(u, v) < 0 || I.isAnc(u, v)) continue;
                bool between = false;                     // some w with u < w < v
                for (int k = 0; k < words; k++)
                    if (descB[u][k] & I.ancB[v][k]) { between = true; break; }
                if (!between) ok[(size_t)u * n + v] = 1;
            }
        for (int v = 0; v < n; v++) {
            canStart[v] = I.pred[v].empty();
            canEnd[v] = dependents[v].empty();
        }
    }

    // ---- LP machinery -------------------------------------------------------
    struct Cand { double viol; vector<pair<int, double>> coef; vector<uint64_t> key; };

    // Master LP data from the root stage (all arcs surviving the static
    // elimination) and the pool of cuts found there, kept for the branch and cut.
    vector<CutLP::Arc> marc;
    vector<double> mcost;
    int mReal = 0;
    vector<vector<pair<int, double>>> pool;
    bool lpOk = false;
    bool incomplete = false;       // numerical/capacity trouble: optimality cannot be claimed
    bool bcInfeasible = false;
    bool bcExact = false;

    static ll roundedLB(double z) { return (ll)ceil(z - 1e-6 * (1.0 + fabs(z))); }

    // Separate violated cuts for the fractional point x on the given arc set:
    //  * subtour cuts   x(arcs entering S) >= 1, S not containing the dummy D,
    //    found by max-flow from D;
    //  * precedence cuts: if k must precede l (covering pair), the real arcs
    //    leaving any node set R with k in R, l outside carry >= 1 (the path
    //    segment from k to l uses real arcs only).
    void separate(const vector<CutLP::Arc>& arcs, const vector<double>& x,
                  const set<vector<uint64_t>>& seen, vector<Cand>& cands) {
        int m = n + 1, D = n, A = (int)arcs.size();
        int words = (m + 63) / 64;
        for (int t = 0; t < n; t++) {
            MaxFlow mf(m);
            for (int j = 0; j < A; j++) if (x[j] > 1e-9) mf.addArc(arcs[j].u, arcs[j].v, x[j]);
            double f = mf.run(D, t);
            if (f >= 1.0 - 1e-5) continue;
            vector<char> r1 = mf.reachableFrom(D), r2 = mf.reachingTo(t);
            for (int variant = 0; variant < 2; variant++) {
                vector<char> inS(m, 0);
                int cnt = 0;
                for (int v = 0; v < m; v++) {
                    inS[v] = variant == 0 ? !r1[v] : r2[v];
                    cnt += inS[v];
                }
                if (cnt < 2 || inS[D]) continue;
                vector<uint64_t> key(words, 0);
                for (int v = 0; v < m; v++) if (inS[v]) key[v >> 6] |= 1ULL << (v & 63);
                if (seen.count(key)) continue;
                vector<pair<int, double>> coef;
                double sum = 0;
                for (int j = 0; j < A; j++)
                    if (inS[arcs[j].v] && !inS[arcs[j].u]) { coef.push_back({j, 1.0}); sum += x[j]; }
                if (sum >= 1.0 - 1e-5) continue;
                cands.push_back({1.0 - sum, coef, key});
            }
        }
        for (auto& pr : coverPairs) {
            int k = pr.first, l = pr.second;
            MaxFlow mf(m);
            for (int j = 0; j < A; j++)
                if (x[j] > 1e-9 && arcs[j].u != D && arcs[j].v != D) mf.addArc(arcs[j].u, arcs[j].v, x[j]);
            double f = mf.run(k, l);
            if (f >= 1.0 - 1e-5) continue;
            vector<char> r1 = mf.reachableFrom(k), r2 = mf.reachingTo(l);
            for (int variant = 0; variant < 2; variant++) {
                vector<char> inR(m, 0);
                for (int v = 0; v < n; v++) inR[v] = variant == 0 ? r1[v] : !r2[v];
                if (!inR[k] || inR[l]) continue;
                vector<uint64_t> key(words + 1, 0);
                key[words] = 1;                                   // tag: precedence cut
                for (int v = 0; v < n; v++) if (inR[v]) key[v >> 6] |= 1ULL << (v & 63);
                if (seen.count(key)) continue;
                vector<pair<int, double>> out;
                double sum = 0;
                for (int j = 0; j < A; j++)
                    if (arcs[j].u != D && arcs[j].v != D && inR[arcs[j].u] && !inR[arcs[j].v]) {
                        out.push_back({j, 1.0});
                        sum += x[j];
                    }
                if (sum >= 1.0 - 1e-5) continue;
                cands.push_back({1.0 - sum, out, key});
            }
        }
        sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.viol > b.viol; });
    }

    // Root LP on all surviving arcs: cutting-plane loop, then reduced-cost
    // fixing against `UB`.  Returns the integer-rounded LP bound, or -1 if the LP
    // was skipped or failed.  Cuts are kept in `pool` for the branch and cut.
    ll lpStage(ll UB) {
        int m = n + 1, D = n;
        marc.clear(); mcost.clear(); pool.clear();
        for (int u = 0; u < n; u++)
            for (int v = 0; v < n; v++)
                if (ok[(size_t)u * n + v]) { marc.push_back({u, v}); mcost.push_back((double)I.at(u, v)); }
        mReal = (int)marc.size();
        for (int v = 0; v < n; v++) if (canStart[v]) { marc.push_back({D, v}); mcost.push_back(0.0); }
        for (int v = 0; v < n; v++) if (canEnd[v]) { marc.push_back({v, D}); mcost.push_back(0.0); }
        int A = (int)marc.size();
        int maxCuts = min(1500, 15 * m);
        if (CutLP::memoryNeeded(m, A, maxCuts) > 25000000ULL) return -1;       // too big: skip the LP
        for (double c : mcost) if (c > 1e8) return -1;                         // avoid numerically harsh data

        CutLP lp(m, marc, mcost, maxCuts);
        if (!lp.initialSolve(deadline)) return -1;

        set<vector<uint64_t>> seenCuts;
        for (int round = 0; round < 300; round++) {
            if (Clock::now() >= deadline) break;
            if (roundedLB(lp.objective()) >= sh->ub.load()) break;
            vector<double> x = lp.primalValues();
            vector<Cand> cands;
            separate(marc, x, seenCuts, cands);
            if (cands.empty()) break;
            int added = 0;
            for (auto& c : cands) {
                if (added >= 40) break;
                if (seenCuts.count(c.key)) continue;
                if (!lp.addCut(c.coef)) break;
                seenCuts.insert(c.key);
                pool.push_back(c.coef);
                added++;
            }
            if (added == 0) break;
            res.cuts += added;
            int st = lp.dual(deadline, 200000);
            if (st != 0) break;            // bound from a dual-feasible basis is still valid
        }
        double z = lp.objective();
        ll lb = roundedLB(z);
        lpZ = z;
        rcArc.assign((size_t)n * n, 0.0);
        rcStart.assign(n, 0.0);
        rcEnd.assign(n, 0.0);
        for (int j = 0; j < A; j++) {
            double dj = max(0.0, lp.d[j]);
            if (j < mReal) rcArc[(size_t)marc[j].u * n + marc[j].v] = dj;
            else if (marc[j].u == D) rcStart[marc[j].v] = dj;
            else rcEnd[marc[j].u] = dj;
        }
        haveRC = true;
        lpOk = true;
        ll cur = sh->ub.load();
        if (lb < cur) {                                                        // reduced-cost fixing
            for (int j = 0; j < A; j++) {
                double dj = max(0.0, lp.d[j]);
                if (roundedLB(z + dj) >= cur) {
                    if (j < mReal) ok[(size_t)marc[j].u * n + marc[j].v] = 0;
                    else if (marc[j].u == D) canStart[marc[j].v] = 0;
                    else canEnd[marc[j].u] = 0;
                }
            }
        }
        return max<ll>(lb, 0);
    }

    // ---- LP-based branch and cut --------------------------------------------
    // Re-solve the node LP after bans (primal simplex from the parent basis),
    // separate cuts (dual simplex), then reduced-cost fixing.  Returns false if
    // the node is pruned (or the search must stop).
    bool nodeLP(CutLP& lp, double& zOut) {
        lp.reprice();
        int st = lp.primal(deadline, 400000);
        if (st != 0) { if (Clock::now() >= deadline) timedOut = true; else incomplete = true; return false; }
        for (int round = 0; round < 60; round++) {
            double z = lp.objective();
            if (roundedLB(z) >= sh->ub.load()) { zOut = z; return false; }
            vector<double> x = lp.primalValues();
            vector<Cand> cands;
            separate(lp.arcs, x, set<vector<uint64_t>>(), cands);
            if (cands.empty()) break;
            int added = 0;
            for (auto& c : cands) {
                if (added >= 30) break;
                if (!lp.addCut(c.coef)) { incomplete = true; break; }
                added++;
            }
            if (added == 0) break;
            st = lp.dual(deadline, 200000);
            if (st == 1) return false;                         // LP infeasible: node pruned
            if (st == 2) { if (Clock::now() >= deadline) timedOut = true; else incomplete = true; return false; }
        }
        zOut = lp.objective();
        return roundedLB(zOut) < sh->ub.load();
    }

    // Decode an integral LP solution into a path and offer it.
    void handleIntegral(const CutLP& lp, const vector<double>& x) {
        int D = n;
        vector<int> succ(n + 1, -1);
        for (int j = 0; j < lp.A; j++) if (x[j] > 0.5) succ[lp.arcs[j].u] = lp.arcs[j].v;
        vector<int> p;
        vector<char> seen(n + 1, 0);
        int cur = succ[D];
        while (cur >= 0 && cur != D && !seen[cur] && (int)p.size() <= n) { seen[cur] = 1; p.push_back(cur); cur = succ[cur]; }
        bool good = ((int)p.size() == n && cur == D);
        if (good) {
            vector<int> pos(n);
            for (int k = 0; k < n; k++) pos[p[k]] = k;
            for (int v = 0; v < n && good; v++)
                for (int u : I.pred[v]) if (pos[u] > pos[v]) { good = false; break; }
        }
        if (!good) { incomplete = true; return; }
        ll c = 0;
        for (int k = 0; k + 1 < n; k++) c += I.at(p[k], p[k + 1]);
        sh->offer(c, p);
    }

    void bcDFS(CutLP& lp, int depth) {
        res.nodes++;
        if (Clock::now() >= deadline || sh->stop.load(memory_order_relaxed)) { timedOut = true; return; }
        double z;
        if (!nodeLP(lp, z)) return;
        ll ub = sh->ub.load();
        vector<double> x = lp.primalValues();
        // reduced-cost fixing at this node
        vector<char> isBasic(lp.A, 0);
        for (int r = 0; r < lp.R; r++) if (lp.basis[r] >= 0 && lp.basis[r] < lp.A) isBasic[lp.basis[r]] = 1;
        double bigc = 4.0 * (double)ub + 1000.0;
        for (int j = 0; j < lp.A; j++)
            if (!isBasic[j] && !lp.banned[j] && roundedLB(z + max(0.0, lp.d[j])) >= ub) lp.ban(j, bigc);
        // branching arc: most fractional
        int bj = -1;
        double bestf = 1e9;
        for (int j = 0; j < lp.A; j++) {
            double f = x[j];
            if (f > 1e-6 && f < 1.0 - 1e-6 && fabs(f - 0.5) < bestf) { bestf = fabs(f - 0.5); bj = j; }
        }
        if (bj < 0) { handleIntegral(lp, x); return; }
        auto forceOne = [&](CutLP& L) {
            for (int j = 0; j < L.A; j++)
                if (j != bj && (L.arcs[j].u == L.arcs[bj].u || L.arcs[j].v == L.arcs[bj].v)) L.ban(j, bigc);
        };
        auto forceZero = [&](CutLP& L) { L.ban(bj, bigc); };
        CutLP child(lp);
        if (x[bj] >= 0.5) {
            forceOne(child); bcDFS(child, depth + 1);
            if (timedOut) return;
            forceZero(lp); bcDFS(lp, depth + 1);
        } else {
            forceZero(child); bcDFS(child, depth + 1);
            if (timedOut) return;
            forceOne(lp); bcDFS(lp, depth + 1);
        }
    }

    // Build a compact LP on the arcs that survived all fixing, load the cut pool,
    // and run the branch and cut.  Returns false if it could not be set up.
    bool bcPhase() {
        int m = n + 1, D = n;
        int Am = (int)marc.size();
        vector<CutLP::Arc> arcs;
        vector<double> cost;
        vector<int> m2l(Am, -1);
        for (int j = 0; j < Am; j++) {
            bool alive = j < mReal ? ok[(size_t)marc[j].u * n + marc[j].v]
                                   : (marc[j].u == D ? canStart[marc[j].v] : canEnd[marc[j].u]);
            if (!alive) continue;
            m2l[j] = (int)arcs.size();
            arcs.push_back(marc[j]);
            cost.push_back(mcost[j]);
        }
        int maxCuts = (int)pool.size() + 250;
        if (arcs.empty() || CutLP::memoryNeeded(m, (int)arcs.size(), maxCuts) > 25000000ULL) return false;
        CutLP lp(m, arcs, cost, maxCuts);
        if (!lp.initialSolve(deadline)) return false;
        for (auto& cut : pool) {
            vector<pair<int, double>> coef;
            for (auto& cj : cut) if (m2l[cj.first] >= 0) coef.push_back({m2l[cj.first], cj.second});
            if (coef.empty()) { bcInfeasible = true; return true; }   // 0 >= 1: nothing better exists
            if (!lp.addCut(coef)) break;
        }
        int st = lp.dual(deadline, 500000);
        if (st == 1) { bcInfeasible = true; return true; }
        if (st == 2) { if (Clock::now() >= deadline) timedOut = true; return true; }
        bcDFS(lp, 0);
        return true;
    }

    // ---- Assignment-problem machinery ---------------------------------------
    ll hungarian(const vector<ll>& c, int m, vector<ll>* outU = nullptr, vector<ll>* outV = nullptr) {
        hu.assign(m + 1, 0); hv.assign(m + 1, 0);
        hp.assign(m + 1, 0); way.assign(m + 1, 0);
        minv.resize(m + 1); used.resize(m + 1);
        for (int i = 1; i <= m; i++) {
            hp[0] = i;
            int j0 = 0;
            fill(minv.begin(), minv.end(), INF);
            fill(used.begin(), used.end(), 0);
            do {
                used[j0] = 1;
                int i0 = hp[j0], j1 = 0;
                ll delta = INF;
                const ll* row = &c[(size_t)(i0 - 1) * m];
                for (int j = 1; j <= m; j++) {
                    if (used[j]) continue;
                    ll cur = row[j - 1] - hu[i0] - hv[j];
                    if (cur < minv[j]) { minv[j] = cur; way[j] = j0; }
                    if (minv[j] < delta) { delta = minv[j]; j1 = j; }
                }
                for (int j = 0; j <= m; j++) {
                    if (used[j]) { hu[hp[j]] += delta; hv[j] -= delta; }
                    else minv[j] -= delta;
                }
                j0 = j1;
            } while (hp[j0] != 0);
            do { int j1 = way[j0]; hp[j0] = hp[j1]; j0 = j1; } while (j0);
        }
        ll total = 0;
        for (int j = 1; j <= m; j++) total += c[(size_t)(hp[j] - 1) * m + (j - 1)];
        if (outU) *outU = hu;
        if (outV) *outV = hv;
        return total;
    }

    // Root assignment bound on nodes + dummy node, with reduced-cost fixing.
    ll rootFixing(ll UB) {
        int m = n + 1, D = n;
        ll lb = 0;
        for (int pass = 0; pass < 50; pass++) {
            if (Clock::now() >= deadline) return lb;
            vector<ll> c((size_t)m * m, BIG);
            for (int u = 0; u < n; u++)
                for (int v = 0; v < n; v++)
                    if (ok[(size_t)u * n + v]) c[(size_t)u * m + v] = I.at(u, v);
            for (int v = 0; v < n; v++) {
                if (canStart[v]) c[(size_t)D * m + v] = 0;
                if (canEnd[v]) c[(size_t)v * m + D] = 0;
            }
            vector<ll> U, V;
            lb = hungarian(c, m, &U, &V);
            if (lb >= BIG) return INF;                    // no feasible path at all
            if (lb >= UB) return lb;
            bool changed = false;
            for (int u = 0; u < n; u++)
                for (int v = 0; v < n; v++)
                    if (ok[(size_t)u * n + v]) {
                        ll rc = c[(size_t)u * m + v] - U[u + 1] - V[v + 1];
                        if (lb + rc >= UB) { ok[(size_t)u * n + v] = 0; changed = true; }
                    }
            for (int v = 0; v < n; v++) {
                if (canStart[v] && lb + (0 - U[D + 1] - V[v + 1]) >= UB) { canStart[v] = 0; changed = true; }
                if (canEnd[v] && lb + (0 - U[v + 1] - V[D + 1]) >= UB) { canEnd[v] = 0; changed = true; }
            }
            if (!changed) break;
        }
        return lb;
    }

    // Hungarian algorithm on doubles (reduced-cost matrices).
    double hungarianD(const vector<double>& c, int m) {
        const double INFD = 1e300;
        hud.assign(m + 1, 0); hvd.assign(m + 1, 0);
        hp.assign(m + 1, 0); way.assign(m + 1, 0);
        minvd.resize(m + 1); used.resize(m + 1);
        for (int i = 1; i <= m; i++) {
            hp[0] = i;
            int j0 = 0;
            fill(minvd.begin(), minvd.end(), INFD);
            fill(used.begin(), used.end(), 0);
            do {
                used[j0] = 1;
                int i0 = hp[j0], j1 = 0;
                double delta = INFD;
                const double* row = &c[(size_t)(i0 - 1) * m];
                for (int j = 1; j <= m; j++) {
                    if (used[j]) continue;
                    double cur = row[j - 1] - hud[i0] - hvd[j];
                    if (cur < minvd[j]) { minvd[j] = cur; way[j] = j0; }
                    if (minvd[j] < delta) { delta = minvd[j]; j1 = j; }
                }
                for (int j = 0; j <= m; j++) {
                    if (used[j]) { hud[hp[j]] += delta; hvd[j] -= delta; }
                    else minvd[j] -= delta;
                }
                j0 = j1;
            } while (hp[j0] != 0);
            do { int j1 = way[j0]; hp[j0] = hp[j1]; j0 = j1; } while (j0);
        }
        double total = 0;
        for (int j = 1; j <= m; j++) total += c[(size_t)(hp[j] - 1) * m + (j - 1)];
        return total;
    }

    // Bound on the extra reduced cost of completing the path from `last`
    // (assignment problem on reduced costs); returns a huge value if infeasible.
    double apBoundRC(int last) {
        static thread_local vector<int> U;
        U.clear();
        for (int v = 0; v < n; v++) if (!visited[v]) U.push_back(v);
        int k = (int)U.size();
        int m = k + 1;
        const double BIGD = 1e9;
        cbufd.assign((size_t)m * m, BIGD);
        for (int r = 0; r < m; r++) {
            int from = (r == 0) ? last : U[r - 1];
            const char* okRow = &ok[(size_t)from * n];
            for (int cidx = 0; cidx < k; cidx++) {
                int to = U[cidx];
                if (okRow[to]) cbufd[(size_t)r * m + cidx] = rcArc[(size_t)from * n + to];
            }
            if (r > 0 && canEnd[from]) {
                bool allow = true;
                for (int w : dependents[from]) if (!visited[w]) { allow = false; break; }
                if (allow) cbufd[(size_t)r * m + k] = rcEnd[from];
            }
        }
        double v = hungarianD(cbufd, m);
        return v >= BIGD * 0.5 ? 1e18 : v;
    }

    // ---- task machinery for the parallel tree search -------------------------
    struct Task { vector<int> pre; ll g; double gd; };

    void initSearchState(int bits) {
        tt.assign((size_t)1 << bits, Entry{0, 0, -1});
        ttMask = ((uint64_t)1 << bits) - 1;
        indeg.assign(n, 0);
        for (int v = 0; v < n; v++) indeg[v] = (int)I.pred[v].size();
        visited.assign(n, 0);
        path.clear();
    }

    // Place the nodes of a prefix and return the last one; ka/kb are the
    // transposition-table keys of the resulting state.
    int replay(const vector<int>& pre, uint64_t& ka, uint64_t& kb) {
        ka = 0; kb = 0;
        int last = -1;
        for (int v : pre) {
            if (last < 0) { ka = zA[v]; kb = zB[v] ^ zLB[v]; }
            else { ka ^= zA[v]; kb ^= zLB[last] ^ zLB[v] ^ zB[v]; }
            place(v);
            last = v;
        }
        return last;
    }
    void unreplay(const vector<int>& pre) {
        for (int k = (int)pre.size() - 1; k >= 0; k--) unplace(pre[k]);
    }

    // Cut the top of the search tree into enough tasks to keep W workers busy.
    vector<Task> makeTasks(int W) {
        vector<Task> fr;
        for (int v = 0; v < n; v++)
            if (indeg[v] == 0 && canStart[v]) fr.push_back({{v}, 0, haveRC ? rcStart[v] : 0.0});
        ll ub = sh->ub.load();
        for (int iter = 0; W > 1 && iter < 6 && fr.size() < (size_t)24 * W; iter++) {
            vector<Task> nx;
            bool grew = false;
            for (auto& t : fr) {
                if ((int)t.pre.size() >= n) { nx.push_back(t); continue; }
                uint64_t ka, kb;
                int last = replay(t.pre, ka, kb);
                for (int v = 0; v < n; v++) {
                    if (visited[v] || indeg[v] != 0 || !ok[(size_t)last * n + v]) continue;
                    ll g2 = t.g + I.at(last, v);
                    if (g2 >= ub) continue;
                    Task nt = t;
                    nt.pre.push_back(v);
                    nt.g = g2;
                    nt.gd += haveRC ? rcArc[(size_t)last * n + v] : 0.0;
                    nx.push_back(std::move(nt));
                    grew = true;
                }
                unreplay(t.pre);
            }
            fr.swap(nx);
            if (!grew) break;
        }
        sort(fr.begin(), fr.end(), [](const Task& a, const Task& b) { return a.gd < b.gd; });
        return fr;
    }

    void workTasks(const vector<Task>& tasks, atomic<int>& next) {
        while (!timedOut) {
            int i = next.fetch_add(1);
            if (i >= (int)tasks.size()) break;
            const Task& t = tasks[i];
            uint64_t ka, kb;
            int last = replay(t.pre, ka, kb);
            dfs(last, t.g, t.gd, ka, kb, (int)t.pre.size());
            unreplay(t.pre);
        }
    }

    void place(int v) {
        visited[v] = 1;
        path.push_back(v);
        for (int s : I.succ[v]) indeg[s]--;
    }
    void unplace(int v) {
        for (int s : I.succ[v]) indeg[s]++;
        path.pop_back();
        visited[v] = 0;
    }

    // Lower bound for completing the path from `last` through all unvisited
    // nodes (rows: last + unvisited; columns: unvisited + dummy end column).
    ll apBound(int last) {
        static thread_local vector<int> U;
        U.clear();
        for (int v = 0; v < n; v++) if (!visited[v]) U.push_back(v);
        int k = (int)U.size();
        int m = k + 1;
        cbuf.assign((size_t)m * m, BIG);
        for (int r = 0; r < m; r++) {
            int from = (r == 0) ? last : U[r - 1];
            const char* okRow = &ok[(size_t)from * n];
            for (int cidx = 0; cidx < k; cidx++) {
                int to = U[cidx];
                if (okRow[to]) cbuf[(size_t)r * m + cidx] = I.at(from, to);
            }
            if (r > 0 && canEnd[from]) {
                bool allow = true;
                for (int w : dependents[from]) if (!visited[w]) { allow = false; break; }
                if (allow) cbuf[(size_t)r * m + k] = 0;
            }
        }
        ll v = hungarian(cbuf, m);
        return v >= BIG ? INF : v;
    }

    void dfs(int last, ll g, double gd, uint64_t ka, uint64_t kb, int depth) {
        res.nodes++;
        if (Clock::now() >= deadline || sh->stop.load(memory_order_relaxed)) timedOut = true;
        if (timedOut) return;
        if (depth == n) { sh->offer(g, path); return; }
        ll ub = sh->ub.load(memory_order_relaxed);
        if (haveRC) {
            // Integer costs: the path cost is at least ceil(lpZ + prefix reduced cost + completion bound).
            double bound = lpZ + gd + apBoundRC(last);
            if (bound - 1e-6 * (1.0 + fabs(bound)) > (double)(ub - 1)) return;
        }
        ll lb = apBound(last);
        if (lb >= INF || g + lb >= ub) return;

        vector<pair<double, int>> ch;
        for (int v = 0; v < n; v++)
            if (!visited[v] && indeg[v] == 0 && ok[(size_t)last * n + v]) {
                double key = haveRC ? rcArc[(size_t)last * n + v] * 1e3 + (double)I.at(last, v) * 1e-3
                                    : (double)I.at(last, v);
                ch.push_back({key, v});
            }
        sort(ch.begin(), ch.end());
        for (auto& pr : ch) {
            int v = pr.second;
            ll g2 = g + I.at(last, v);
            double gd2 = gd + (haveRC ? rcArc[(size_t)last * n + v] : 0.0);
            if (g2 >= sh->ub.load(memory_order_relaxed)) continue;
            uint64_t ka2 = ka ^ zA[v], kb2 = kb ^ zLB[last] ^ zLB[v] ^ zB[v];
            Entry& e = tt[(ka2 ^ (kb2 * 0x9e3779b97f4a7c15ULL)) & ttMask];
            if (e.g >= 0 && e.a == ka2 && e.b == kb2 && e.g <= g2) continue;   // dominated
            e.a = ka2; e.b = kb2; e.g = g2;
            place(v);
            dfs(v, g2, gd2, ka2, kb2, depth + 1);
            unplace(v);
            if (timedOut) return;
        }
    }
};

// -----------------------------------------------------------------------------
// Per-instance driver
// -----------------------------------------------------------------------------
struct Row {
    string instance;
    int n = 0;
    double time = 0;
    ll enumerated = 0;
    ll cost = -1;
    vector<int> path;
    bool ok = false;
};

static string csvEscape(const string& s) {
    if (s.find_first_of(",\"\n") == string::npos) return s;
    string r = "\"";
    for (char c : s) { if (c == '"') r += '"'; r += c; }
    return r + "\"";
}

struct Options {
    double timeLimit = 60.0;
    int ttBits = 22;
    uint64_t seed = 12345;
    double proofShare = 0.0;     // 0 = automatic
    int heurThreads = 0;         // 0 = automatic
    int dfsThreads = 0;          // 0 = automatic
    bool noLP = false;
    bool noBC = false;
    bool noExact = false;
    bool noHeur = false;         // exact thread only (after the first quick heuristic)
};

static Row solveFile(const string& file, const Options& opt) {
    Row row;
    row.instance = baseName(file);
    Clock::time_point t0 = Clock::now();
    Instance I;
    string err;
    if (!parseSop(file, I, err)) {
        cerr << "[" << row.instance << "] ERROR: " << err << "\n";
        return row;
    }
    row.n = I.n;
    if (!I.buildPrecedence()) {
        cerr << "[" << row.instance << "] ERROR: precedence constraints contain a cycle (infeasible)\n";
        return row;
    }
    const double limit = opt.timeLimit > 0 ? opt.timeLimit : 1e18;
    auto at = [&](double secs) {
        return t0 + chrono::duration_cast<Clock::duration>(chrono::duration<double>(min(secs, 1e9)));
    };

    Shared sh;
    bool proven = false;
    ll enumerated = 0, lowerBound = 0;
    BBResult bbRes;
    unsigned hw = max(1u, thread::hardware_concurrency());

    if (I.n == 1) { sh.offer(0, {0}); proven = true; }
    else {
        // ---- Phase 1: quick single-threaded heuristic for a first upper bound
        {
            Heuristic H(I, opt.seed);
            vector<int> p; ll c;
            H.run(t0, min(1.0, 0.05 * limit), 100LL * I.n, p, c);
            sh.offer(c, p);
        }
        // ---- Phase 2: exact thread + cooperating heuristic threads ----------
        // Thread budget: one LP branch-and-cut thread, J heuristic threads and W tree-search
        // workers.  With few cores the tree search stays single-threaded.
        int J = opt.heurThreads > 0 ? opt.heurThreads : (hw >= 4 ? max(1, (int)hw / 4) : max(1, (int)hw - 1));
        if (opt.noHeur) J = 0;
        int W = opt.dfsThreads > 0 ? opt.dfsThreads : (hw >= 4 ? max(1, (int)hw - max(J, 1) - 1) : 1);
        double share = opt.proofShare;
        if (share <= 0) share = hw >= 4 ? 1.0 : (I.n <= 100 ? 0.5 : 0.15);
        double exactEnd = opt.timeLimit > 0 ? share * limit : 1e18;
        bool runExact = !opt.noExact;

        thread exactThread;
        if (runExact) {
            exactThread = thread([&]() {
                BranchAndBound bb(I, opt.ttBits, opt.seed);
                bbRes = bb.run(sh, at(exactEnd), !opt.noLP, !opt.noBC, W);
                if (bbRes.exact) sh.stop.store(true);
            });
        }
        vector<thread> workers;
        for (int k = 0; k < J; k++) {
            workers.emplace_back([&, k]() {
                Heuristic H(I, opt.seed + 7919ULL * (k + 1));
                H.sh = &sh;
                vector<int> p; ll c;
                H.run(t0, limit, numeric_limits<long long>::max(), p, c);
            });
        }
        if (runExact) exactThread.join();
        for (auto& w : workers) w.join();
        proven = bbRes.exact;
        enumerated = bbRes.nodes;
        lowerBound = bbRes.rootLB;
    }
    ll bestCost = sh.ub.load();
    vector<int> bestPath = sh.path;
    if (proven) lowerBound = bestCost;

    // ---- Validation ---------------------------------------------------------
    ll check;
    string why;
    if (bestPath.empty() || !validatePath(I, bestPath, check, why)) {
        cerr << "[" << row.instance << "] ERROR: final path failed validation: " << why << "\n";
        return row;
    }
    if (check != bestCost) {
        cerr << "[" << row.instance << "] WARNING: cost mismatch (" << bestCost << " vs recomputed "
             << check << "); reporting recomputed cost\n";
        bestCost = check;
    }
    row.time = secondsSince(t0);
    row.enumerated = enumerated;
    row.cost = bestCost;
    row.path = bestPath;
    row.ok = true;
    cerr << "[" << row.instance << "] N=" << I.n << " cost=" << bestCost << "  ";
    if (proven) cerr << "PROVEN OPTIMAL";
    else {
        double gap = lowerBound > 0 ? 100.0 * (bestCost - lowerBound) / (double)bestCost : 100.0;
        cerr << "BEST FOUND (not proven optimal; lower bound " << lowerBound << ", gap <= " << fixed
             << setprecision(1) << gap << "%)";
        cerr.unsetf(ios::floatfield);
    }
    if (bbRes.lpLB >= 0) cerr << "  [LP bound " << bbRes.lpLB << ", " << bbRes.cuts << " cuts, "
                              << bbRes.arcsRemoved << " arcs removed]";
    cerr << "  time=" << fixed << setprecision(3) << row.time << "s  nodes=" << enumerated << "\n";
    cerr.unsetf(ios::floatfield);
    return row;
}

static void usage(const char* prog) {
    cerr << "Usage: " << prog << " [options] file1.sop [file2.sop ...]\n"
         << "  -t, --time-limit SEC   per-instance wall-clock limit in seconds (default 60; 0 = unlimited,\n"
         << "                         i.e. run until optimality is proven)\n"
         << "  -o, --output FILE      write CSV to FILE (default: stdout)\n"
         << "  -j, --threads N        heuristic worker threads (default: cores/4 with >= 4 cores, else cores - 1)\n"
         << "  -w, --dfs-threads N    tree-search worker threads (default: the remaining cores with >= 4\n"
         << "                         cores, else 1); the LP branch-and-cut always has its own thread\n"
         << "  -p, --proof-share F    fraction (0..1] of the time limit the exact thread may use\n"
         << "                         (default: 1.0 with >= 4 cores, else 0.5, or 0.15 when N > 100)\n"
         << "  -b, --tt-bits B        transposition table has 2^B entries, 24 bytes each (default 22,\n"
         << "                         about 100 MB; range 10..28)\n"
         << "  -s, --seed N           random seed (default 12345)\n"
         << "      --no-lp            skip the cutting-plane LP (assignment bound only)\n"
         << "      --no-bc            skip the LP-based branch-and-cut thread\n"
         << "      --no-exact         heuristic only\n"
         << "      --no-heuristic     exact thread only (after one quick heuristic run)\n"
         << "  -h, --help             show this help\n";
}

int main(int argc, char** argv) {
    Options opt;
    string outPath;
    vector<string> files;
    for (int a = 1; a < argc; a++) {
        string s = argv[a];
        auto need = [&](const char* what) -> string {
            if (a + 1 >= argc) { cerr << "missing value for " << what << "\n"; exit(2); }
            return argv[++a];
        };
        if (s == "-t" || s == "--time-limit") opt.timeLimit = atof(need("time limit").c_str());
        else if (s == "-o" || s == "--output") outPath = need("output");
        else if (s == "-j" || s == "--threads") opt.heurThreads = atoi(need("threads").c_str());
        else if (s == "-w" || s == "--dfs-threads") opt.dfsThreads = atoi(need("dfs threads").c_str());
        else if (s == "-p" || s == "--proof-share") opt.proofShare = atof(need("proof share").c_str());
        else if (s == "-b" || s == "--tt-bits") opt.ttBits = atoi(need("tt bits").c_str());
        else if (s == "-s" || s == "--seed") opt.seed = (uint64_t)atoll(need("seed").c_str());
        else if (s == "--no-lp") opt.noLP = true;
        else if (s == "--no-bc") opt.noBC = true;
        else if (s == "--no-exact") opt.noExact = true;
        else if (s == "--no-heuristic") opt.noHeur = true;
        else if (s == "-h" || s == "--help") { usage(argv[0]); return 0; }
        else if (!s.empty() && s[0] == '-' && s.size() > 1) { cerr << "unknown option " << s << "\n"; usage(argv[0]); return 2; }
        else files.push_back(s);
    }
    if (files.empty()) { usage(argv[0]); return 2; }
    opt.ttBits = max(10, min(28, opt.ttBits));
    if (opt.proofShare > 1.0) opt.proofShare = 1.0;
    if (opt.noExact && opt.timeLimit <= 0) { cerr << "--no-exact needs a finite time limit\n"; return 2; }

    ostream* out = &cout;
    ofstream fout;
    if (!outPath.empty()) {
        fout.open(outPath);
        if (!fout) { cerr << "cannot open output file " << outPath << "\n"; return 2; }
        out = &fout;
    }
    *out << "Instance,Instance Size,Time,Enumerated Nodes,Best Cost,Best Path\n";
    int failures = 0;
    for (const string& f : files) {
        Row r = solveFile(f, opt);
        if (!r.ok) { failures++; continue; }
        ostringstream path;
        for (size_t k = 0; k < r.path.size(); k++) path << (k ? " " : "") << r.path[k];
        char tbuf[32];
        snprintf(tbuf, sizeof tbuf, "%.3f", r.time);
        *out << csvEscape(r.instance) << "," << r.n << "," << tbuf << "," << r.enumerated << ","
             << r.cost << "," << path.str() << "\n";
        out->flush();
    }
    return failures ? 1 : 0;
}
