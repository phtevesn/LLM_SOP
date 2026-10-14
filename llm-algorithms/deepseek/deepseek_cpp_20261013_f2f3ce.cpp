// ============================================================================
// Sequential Ordering Problem (SOP) solver.
//
// Approach:
//   * N <= 20 : exact Held-Karp DP over precedence-reachable masks.
//   * N >  20 : Best-First Branch-and-Bound with
//                 - admissible arc-based lower bound,
//                 - history-based domination (hash table on (mask,last)),
//                 - forced-move propagation,
//                 - parent-pointer path reconstruction.
//
// Guarantees:
//   * Every reported path is re-validated: permutation, precedence, endpoints,
//     cost recomputed from the matrix.
//   * DP results are proven optimal. B&B results are proven optimal if the
//     search exhausts within the time limit; otherwise best-found.
//
// Build:
//   g++ -O2 -std=c++17 -o sop_solver sop_solver.cpp
// Run:
//   ./sop_solver prob.42.sop --time=60 --out=res.csv
// ============================================================================

#include <bits/stdc++.h>
#include <chrono>
#include <unordered_map>
#include <queue>
using namespace std;

// ---------------------------------------------------------------------------
// Instance
// ---------------------------------------------------------------------------
struct Instance {
    string              name;
    int                 N = 0;
    vector<vector<int>> T;
    vector<uint32_t>    pred;
    int                 start = -1, end = -1;
};

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------
static void trimStr(string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    if (a == string::npos) { s.clear(); return; }
    s = s.substr(a, b - a + 1);
}

static Instance parseSOP(const string& path) {
    ifstream in(path);
    if (!in) throw runtime_error("Cannot open " + path);
    Instance inst;
    inst.name = path;

    string line;
    int  dim = -1;
    bool inMat = false, gotRepeat = false;
    vector<int> vals;

    while (getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        string t = line;
        trimStr(t);
        if (t.empty()) continue;

        if (!inMat) {
            auto starts = [&](const char* k){ return t.rfind(k, 0) == 0; };
            if (starts("NAME")) {
                auto c = t.find(':');
                if (c != string::npos) {
                    string v = t.substr(c + 1);
                    trimStr(v);
                    if (!v.empty()) inst.name = v;
                }
            } else if (starts("DIMENSION")) {
                auto c = t.find(':');
                if (c != string::npos) dim = stoi(t.substr(c + 1));
            } else if (starts("EDGE_WEIGHT_SECTION")) {
                inMat = true;
                if (dim <= 0) throw runtime_error("DIMENSION missing before matrix");
                inst.N = dim;
                vals.reserve((size_t)dim * dim);
            }
        } else {
            if (t.rfind("EOF", 0) == 0) break;
            istringstream iss(t);
            long long x;
            while (iss >> x) {
                if (!gotRepeat) {
                    if ((int)x == dim) { gotRepeat = true; }
                    else { gotRepeat = true; vals.push_back((int)x); }
                } else {
                    vals.push_back((int)x);
                }
                if ((int)vals.size() >= dim * dim) break;
            }
            if ((int)vals.size() >= dim * dim) break;
        }
    }

    if (inst.N <= 0) throw runtime_error("N not parsed");
    if ((int)vals.size() < inst.N * inst.N)
        throw runtime_error("Matrix incomplete: got " + to_string(vals.size()) +
                            ", expected " + to_string(inst.N * inst.N));

    inst.T.assign(inst.N, vector<int>(inst.N));
    for (int i = 0; i < inst.N; ++i)
        for (int j = 0; j < inst.N; ++j)
            inst.T[i][j] = vals[(size_t)i * inst.N + j];

    inst.pred.assign(inst.N, 0);
    for (int i = 0; i < inst.N; ++i)
        for (int j = 0; j < inst.N; ++j)
            if (i != j && inst.T[i][j] < 0)
                inst.pred[i] |= (1u << j);

    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < inst.N; ++i) {
            uint32_t add = 0, m = inst.pred[i];
            while (m) { int j = __builtin_ctz(m); m &= m - 1; add |= inst.pred[j]; }
            uint32_t np = inst.pred[i] | add;
            if (np != inst.pred[i]) { inst.pred[i] = np; changed = true; }
        }
    }

    vector<int> indeg(inst.N, 0), outdeg(inst.N, 0);
    for (int i = 0; i < inst.N; ++i) {
        uint32_t m = inst.pred[i];
        while (m) { int j = __builtin_ctz(m); m &= m - 1; indeg[i]++; outdeg[j]++; }
    }
    int s = -1, e = -1, nsrc = 0, nsnk = 0;
    for (int i = 0; i < inst.N; ++i) {
        if (indeg[i]  == 0) { nsrc++; if (s < 0) s = i; }
        if (outdeg[i] == 0) { nsnk++; if (e < 0) e = i; }
    }
    if (nsrc != 1) s = 0;
    if (nsnk != 1) e = inst.N - 1;
    if (s == e) throw runtime_error("start == end");
    inst.start = s;
    inst.end   = e;
    return inst;
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------
struct Valid {
    bool      ok = false;
    long long cost = -1;
    string    err;
};

static Valid validate(const Instance& I, const vector<int>& p) {
    Valid v;
    int N = I.N;
    if ((int)p.size() != N) { v.err = "size mismatch"; return v; }
    vector<char> seen(N, 0);
    vector<int>  pos(N, -1);
    for (int i = 0; i < N; ++i) {
        int x = p[i];
        if (x < 0 || x >= N) { v.err = "node out of range"; return v; }
        if (seen[x]) { v.err = "duplicate node"; return v; }
        seen[x] = 1;
        pos[x]  = i;
    }
    if (p.front() != I.start) { v.err = "start mismatch"; return v; }
    if (p.back()  != I.end)   { v.err = "end mismatch";   return v; }
    for (int i = 0; i < N; ++i) {
        uint32_t m = I.pred[i];
        while (m) {
            int j = __builtin_ctz(m); m &= m - 1;
            if (pos[j] > pos[i]) { v.err = "precedence violated"; return v; }
        }
    }
    long long c = 0;
    for (int i = 0; i + 1 < N; ++i) {
        int a = p[i], b = p[i + 1];
        if (I.T[a][b] < 0) { v.err = "negative arc used in path"; return v; }
        c += I.T[a][b];
    }
    v.ok = true;
    v.cost = c;
    return v;
}

// ---------------------------------------------------------------------------
// Exact DP
// ---------------------------------------------------------------------------
struct Result {
    long long   cost = LLONG_MAX;
    vector<int> path;
    long long   expansions = 0;
    bool        proved = false;
};

static Result solveDP(const Instance& I, double tlimit) {
    Result R;
    int N = I.N;
    if (N > 20) return R;
    const uint32_t FULL = (N == 32) ? 0xFFFFFFFFu : ((1u << N) - 1);
    const int INF = INT_MAX / 2;

    size_t states = (size_t)1 << N;
    vector<int> dp (states * (size_t)N, INF);
    vector<int> par(states * (size_t)N, -1);
    auto idx = [&](uint32_t m, int last) { return (size_t)m * N + last; };

    dp[idx(1u << I.start, I.start)] = 0;
    auto t0 = chrono::steady_clock::now();

    for (uint32_t mask = 0; mask <= FULL; ++mask) {
        if (!(mask & (1u << I.start))) continue;
        if ((mask & 0xFFFFu) == 0) {
            double el = chrono::duration<double>(
                chrono::steady_clock::now() - t0).count();
            if (el > tlimit) return R;
        }
        uint32_t m = mask;
        while (m) {
            int last = __builtin_ctz(m); m &= m - 1;
            int cur = dp[idx(mask, last)];
            if (cur >= INF) continue;
            for (int nxt = 0; nxt < N; ++nxt) {
                if (mask & (1u << nxt)) continue;
                if ((I.pred[nxt] & ~mask) != 0) continue;
                int c = I.T[last][nxt];
                if (c < 0) continue;
                uint32_t nm = mask | (1u << nxt);
                int nc = cur + c;
                size_t k = idx(nm, nxt);
                if (nc < dp[k]) { dp[k] = nc; par[k] = last; }
                R.expansions++;
            }
        }
    }

    int best = dp[idx(FULL, I.end)];
    if (best >= INF) return R;
    R.cost = best;
    R.proved = true;
    vector<int> p;
    uint32_t mask = FULL;
    int cur = I.end;
    while (cur != -1) {
        p.push_back(cur);
        int pv = par[idx(mask, cur)];
        mask ^= (1u << cur);
        cur = pv;
    }
    reverse(p.begin(), p.end());
    R.path = move(p);
    return R;
}

// ---------------------------------------------------------------------------
// Branch-and-Bound (with parent-pointer reconstruction)
// ---------------------------------------------------------------------------
namespace bnb {

struct Node {
    uint32_t    visited;
    int         last;
    int         cost;
    long long   lb;
    int         depth;
    int         parent;   // arena index of parent, -1 for root
};

struct History {
    unordered_map<uint64_t, int> tbl;
    static inline uint64_t key(uint32_t mask, int last) {
        return ((uint64_t)mask << 6) | (uint64_t)(last & 0x3F);
    }
    bool pruneOrInsert(uint32_t mask, int last, int cost) {
        uint64_t k = key(mask, last);
        auto it = tbl.find(k);
        if (it != tbl.end() && cost >= it->second) return true;
        tbl[k] = cost;
        return false;
    }
};

class Solver {
public:
    Solver(const Instance& inst, double tlim)
        : I(inst), N(inst.N), tlimit(tlim) {
        minIn.assign(N, INT_MAX);
        for (int v = 0; v < N; ++v)
            for (int u = 0; u < N; ++u) {
                if (u == v) continue;
                int c = I.T[u][v];
                if (c < 0) continue;
                if (minIn[v] > c) minIn[v] = c;
            }
    }

    Result run() {
        auto t0 = chrono::steady_clock::now();
        FULL = (N == 32) ? 0xFFFFFFFFu : ((1u << N) - 1);

        // Arena of nodes with stable indices (deque keeps references stable).
        deque<Node> arena;

        Node root;
        root.visited = 1u << I.start;
        root.last    = I.start;
        root.cost    = 0;
        root.depth   = 0;
        root.parent  = -1;
        root.lb      = lowerBound(root.last, root.visited, root.cost);
        arena.push_back(root);

        // Priority queue stores (lb, arena_index).
        auto cmp = [](const pair<long long,int>& a, const pair<long long,int>& b) {
            return a.first > b.first;
        };
        priority_queue<pair<long long,int>,
                       vector<pair<long long,int>>, decltype(cmp)> pq(cmp);
        pq.push({root.lb, 0});

        bestCost = LLONG_MAX;
        bestIdx  = -1;
        long long expansions = 0;

        while (!pq.empty()) {
            if ((expansions & 0x3FF) == 0) {
                double el = chrono::duration<double>(
                    chrono::steady_clock::now() - t0).count();
                if (el > tlimit) break;
            }

            auto [lbTop, idx] = pq.top(); pq.pop();
            Node cur = arena[idx];   // copy
            ++expansions;

            if (cur.lb >= bestCost) continue;

            if (cur.visited == FULL) {
                if (cur.last == I.end && cur.cost < bestCost) {
                    bestCost = cur.cost;
                    bestIdx  = idx;
                }
                continue;
            }

            // Forced move
            int forced = forcedNext(cur.visited);
            if (forced >= 0) {
                int c = I.T[cur.last][forced];
                if (c < 0) continue;
                Node child;
                child.visited = cur.visited | (1u << forced);
                child.last    = forced;
                child.cost    = cur.cost + c;
                child.depth   = cur.depth + 1;
                child.parent  = idx;
                if (history.pruneOrInsert(child.visited, child.last, child.cost))
                    continue;
                child.lb = lowerBound(child.last, child.visited, child.cost);
                if (child.lb >= bestCost) continue;
                arena.push_back(child);
                int cidx = (int)arena.size() - 1;
                pq.push({child.lb, cidx});
                continue;
            }

            // Branch
            uint32_t rem = (~cur.visited) & FULL;
            while (rem) {
                int v = __builtin_ctz(rem); rem &= rem - 1;
                if (I.pred[v] & ~cur.visited) continue;
                int c = I.T[cur.last][v];
                if (c < 0) continue;
                int nc = cur.cost + c;
                if (history.pruneOrInsert(cur.visited | (1u << v), v, nc))
                    continue;
                Node child;
                child.visited = cur.visited | (1u << v);
                child.last    = v;
                child.cost    = nc;
                child.depth   = cur.depth + 1;
                child.parent  = idx;
                child.lb      = lowerBound(child.last, child.visited, nc);
                if (child.lb >= bestCost) continue;
                arena.push_back(child);
                int cidx = (int)arena.size() - 1;
                pq.push({child.lb, cidx});
            }
        }

        Result R;
        R.expansions = expansions;
        R.proved = pq.empty();
        if (bestIdx >= 0) {
            R.cost = bestCost;
            // Walk parent pointers from bestIdx back to root.
            vector<int> rev;
            int i = bestIdx;
            while (i >= 0) {
                rev.push_back(arena[i].last);
                i = arena[i].parent;
            }
            reverse(rev.begin(), rev.end());
            R.path = move(rev);
        }
        return R;
    }

private:
    const Instance& I;
    int N = 0;
    double tlimit;
    uint32_t FULL = 0;
    vector<int> minIn;
    long long bestCost = LLONG_MAX;
    int bestIdx = -1;
    History history;

    long long lowerBound(int last, uint32_t visited, int curCost) const {
        uint32_t rem = (~visited) & FULL;
        if (rem == 0) return curCost;

        long long sumMinIn = 0;
        {
            uint32_t r = rem;
            while (r) {
                int v = __builtin_ctz(r); r &= r - 1;
                if (minIn[v] >= INT_MAX) return LLONG_MAX;
                sumMinIn += minIn[v];
            }
        }
        long long lbB = curCost + sumMinIn;

        int mnOut = INT_MAX, argOut = -1;
        {
            uint32_t r = rem;
            while (r) {
                int v = __builtin_ctz(r); r &= r - 1;
                int c = I.T[last][v];
                if (c < 0) continue;
                if (c < mnOut) { mnOut = c; argOut = v; }
            }
        }
        long long lbA = LLONG_MAX;
        if (mnOut < INT_MAX) {
            long long s = 0; bool ok = true;
            uint32_t r = rem;
            while (r) {
                int v = __builtin_ctz(r); r &= r - 1;
                if (v == argOut) continue;
                if (minIn[v] >= INT_MAX) { ok = false; break; }
                s += minIn[v];
            }
            if (ok) lbA = curCost + (long long)mnOut + s;
        }
        return max(lbA, lbB);
    }

    int forcedNext(uint32_t visited) const {
        uint32_t rem = (~visited) & FULL;
        int forced = -1, cnt = 0;
        uint32_t r = rem;
        while (r) {
            int v = __builtin_ctz(r); r &= r - 1;
            if ((I.pred[v] & ~visited) == 0) { forced = v; cnt++; }
        }
        return (cnt == 1) ? forced : -1;
    }
};

} // namespace bnb

// ---------------------------------------------------------------------------
// CSV helper
// ---------------------------------------------------------------------------
static string csvField(const string& s) {
    bool need = false;
    for (char c : s) if (c == ',' || c == '"' || c == '\n' || c == '\r') { need = true; break; }
    if (!need) return s;
    string o = "\"";
    for (char c : s) { if (c == '"') o += "\"\""; else o += c; }
    o += "\"";
    return o;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    vector<string> files;
    double tlimit  = 60.0;
    string outPath;

    for (int i = 1; i < argc; ++i) {
        string a = argv[i];
        if (a.rfind("--time=", 0) == 0) tlimit = stod(a.substr(7));
        else if (a.rfind("--out=", 0) == 0) outPath = a.substr(6);
        else if (a == "-h" || a == "--help") {
            cerr << "Usage: " << argv[0]
                 << " file1.sop [file2.sop ...] [--time=SECONDS] [--out=results.csv]\n";
            return 0;
        } else if (!a.empty() && a[0] == '-') {
            cerr << "Unknown option: " << a << "\n";
            return 1;
        } else files.push_back(a);
    }
    if (files.empty()) {
        cerr << "Usage: " << argv[0]
             << " file1.sop [file2.sop ...] [--time=SECONDS] [--out=results.csv]\n";
        return 1;
    }

    ostream* out = &cout;
    ofstream fout;
    if (!outPath.empty()) {
        fout.open(outPath);
        if (!fout) { cerr << "Cannot open " << outPath << "\n"; return 1; }
        out = &fout;
    }
    (*out) << "Instance,Instance Size,Time,Enumerated Nodes,Best Cost,Best Path\n";

    for (const string& f : files) {
        auto w0 = chrono::steady_clock::now();
        try {
            Instance I = parseSOP(f);
            Result R;
            if (I.N <= 20) {
                R = solveDP(I, tlimit);
                if (!R.proved || R.path.empty()) {
                    bnb::Solver S(I, tlimit);
                    R = S.run();
                }
            } else {
                bnb::Solver S(I, tlimit);
                R = S.run();
            }

            double el = chrono::duration<double>(
                chrono::steady_clock::now() - w0).count();

            string pathStr;
            long long cost = -1;
            if (!R.path.empty()) {
                Valid v = validate(I, R.path);
                if (v.ok) {
                    cost = v.cost;
                    ostringstream ps;
                    for (size_t i = 0; i < R.path.size(); ++i) {
                        if (i) ps << ' ';
                        ps << R.path[i];
                    }
                    pathStr = ps.str();
                } else {
                    pathStr = "INVALID: " + v.err;
                }
            } else {
                pathStr = "NO SOLUTION FOUND";
            }

            (*out) << csvField(I.name) << ','
                   << I.N << ','
                   << fixed << setprecision(3) << el << ','
                   << R.expansions << ','
                   << (cost < 0 ? string("NA") : to_string(cost)) << ','
                   << csvField(pathStr) << '\n';
            out->flush();
        } catch (const exception& e) {
            double el = chrono::duration<double>(
                chrono::steady_clock::now() - w0).count();
            (*out) << csvField(f) << ",0,"
                   << fixed << setprecision(3) << el << ",0,NA,"
                   << csvField(string("ERROR: ") + e.what()) << '\n';
            out->flush();
        }
    }
    return 0;
}