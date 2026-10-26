// sop_solver.cpp -- exact parallel solver for the Sequential Ordering Problem (SOP).
//
// Parallel branch-and-bound with
//   * assignment-problem (AP) lower bound maintained incrementally (dynamic Hungarian, O(n^2)/node),
//   * history-based domination in a shared hash table (full keys, collision-safe),
//   * task pool with work donation for load balancing,
//   * a concurrently running iterated-local-search (ILS) heuristic that shares the incumbent.
//
// Build:  g++ -O3 -march=native -std=c++17 -pthread -o sop_solver sop_solver.cpp
// Usage:  sop_solver FILE.sop [--time-limit S] [--threads N] [--output out.csv] [--no-header]
//
// Debug/test-only options: --no-heur --no-history --no-pin --seed K --mem-mb M --quiet
// (see README.md).  Env SOP_DEBUG=1 re-validates every incumbent that is offered.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SOP_MUTANT
#define SOP_MUTANT 0  // test-only: deliberately broken variants used to prove the tests can fail
#endif

using namespace std;
typedef long long ll;
typedef uint64_t u64;
typedef chrono::steady_clock Clock;

static Clock::time_point g_t0;
static bool g_quiet = false;
static bool g_debug = false;
static inline double elapsed() { return chrono::duration<double>(Clock::now() - g_t0).count(); }
#define LOG(...)                                   \
    do {                                           \
        if (!g_quiet) {                            \
            fprintf(stderr, "[%8.3f] ", elapsed()); \
            fprintf(stderr, __VA_ARGS__);          \
            fputc('\n', stderr);                   \
        }                                          \
    } while (0)

static inline u64 splitmix64(u64 x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}
struct Rng {
    u64 s;
    explicit Rng(u64 seed) : s(splitmix64(seed) | 1ULL) {}
    u64 next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    int below(int n) { return (int)(next() % (u64)n); }
    double uni() { return (next() >> 11) * (1.0 / 9007199254740992.0); }
};

// ---------------------------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------------------------
struct Options {
    string file;
    double timeLimit = -1;  // <0: none
    int threads = 1;
    string output;
    bool noHeader = false;
    bool noPin = false, noHeur = false, noHistory = false;
    u64 seed = 12345;
    double memMB = -1;
};

static void usage() {
    fprintf(stderr,
            "usage: sop_solver FILE.sop [--time-limit SECONDS] [--threads N] [--output out.csv] [--no-header]\n"
            "  --threads 0 = one thread per physical core. Extra (debug) options: --no-heur --no-history\n"
            "  --no-pin --seed K --mem-mb M --quiet\n");
}

static bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; i++) {
        string a = argv[i];
        auto val = [&](const string& name, string& out) -> bool {
            if (a == name) {
                if (i + 1 >= argc) return false;
                out = argv[++i];
                return true;
            }
            if (a.rfind(name + "=", 0) == 0) {
                out = a.substr(name.size() + 1);
                return true;
            }
            return false;
        };
        string v;
        if (val("--time-limit", v)) {
            char* e;
            o.timeLimit = strtod(v.c_str(), &e);
            if (*e || o.timeLimit < 0) { fprintf(stderr, "bad --time-limit\n"); return false; }
        } else if (val("--threads", v)) {
            char* e;
            long t = strtol(v.c_str(), &e, 10);
            if (*e || t < 0) { fprintf(stderr, "bad --threads\n"); return false; }
            o.threads = (int)t;
        } else if (val("--output", v)) {
            o.output = v;
        } else if (val("--seed", v)) {
            o.seed = strtoull(v.c_str(), nullptr, 10);
        } else if (val("--mem-mb", v)) {
            o.memMB = strtod(v.c_str(), nullptr);
        } else if (a == "--no-header") o.noHeader = true;
        else if (a == "--no-pin") o.noPin = true;
        else if (a == "--no-heur") o.noHeur = true;
        else if (a == "--no-history") o.noHistory = true;
        else if (a == "--quiet") g_quiet = true;
        else if (a == "-h" || a == "--help") { usage(); exit(0); }
        else if (!a.empty() && a[0] == '-') { fprintf(stderr, "unknown option %s\n", a.c_str()); return false; }
        else if (o.file.empty()) o.file = a;
        else { fprintf(stderr, "more than one input file given\n"); return false; }
    }
    if (o.file.empty()) return false;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Instance parsing
// ---------------------------------------------------------------------------------------------
struct Instance {
    string name;
    int N = 0;
    vector<ll> T;  // N*N
};

static string trimS(const string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
static string upperS(string s) {
    for (auto& c : s) c = (char)toupper((unsigned char)c);
    return s;
}

static bool parseSop(const string& path, Instance& ins, string& err) {
    ifstream f(path, ios::binary);
    if (!f) { err = "cannot open file '" + path + "'"; return false; }
    string content((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());
    size_t pos = 0, len = content.size();
    bool inSection = false;
    long long dim = 0;
    string ewf = "FULL_MATRIX";
    while (pos < len) {
        size_t e = content.find('\n', pos);
        if (e == string::npos) e = len;
        string line = trimS(content.substr(pos, e - pos));
        pos = e + 1;
        if (line.empty()) continue;
        string up = upperS(line);
        if (up.rfind("EDGE_WEIGHT_SECTION", 0) == 0) { inSection = true; break; }
        size_t c = line.find(':');
        if (c != string::npos) {
            string key = upperS(trimS(line.substr(0, c))), val = trimS(line.substr(c + 1));
            if (key == "DIMENSION") dim = atoll(val.c_str());
            else if (key == "EDGE_WEIGHT_FORMAT") ewf = upperS(val);
        }
    }
    if (!inSection) { err = "no EDGE_WEIGHT_SECTION found"; return false; }
    if (ewf != "FULL_MATRIX") { err = "unsupported EDGE_WEIGHT_FORMAT '" + ewf + "' (only FULL_MATRIX)"; return false; }
    vector<ll> vals;
    if (dim > 0 && dim <= 5000) vals.reserve((size_t)dim * dim + 1);
    while (pos < len) {
        while (pos < len && isspace((unsigned char)content[pos])) pos++;
        if (pos >= len) break;
        size_t e = pos;
        while (e < len && !isspace((unsigned char)content[e])) e++;
        string tok = content.substr(pos, e - pos);
        pos = e;
        if (upperS(tok) == "EOF") break;
        char* endp;
        errno = 0;
        ll x = strtoll(tok.c_str(), &endp, 10);
        if (*endp != 0 || errno) {
            if (dim > 0 && (ll)vals.size() >= dim * dim) break;  // trailing junk after a full matrix
            err = "non-numeric token '" + tok + "' inside matrix";
            return false;
        }
        vals.push_back(x);
    }
    if (dim <= 0) {
        if (vals.empty()) { err = "missing DIMENSION"; return false; }
        dim = vals[0];
    }
    if (dim < 1 || dim > 5000) { err = "unsupported DIMENSION " + to_string(dim); return false; }
    size_t need = (size_t)dim * dim;
    size_t off = 0;
    if (vals.size() >= need + 1 && vals[0] == dim) off = 1;
    if (vals.size() < off + need) {
        err = "matrix has " + to_string(vals.size()) + " values, expected " + to_string(need) + " (+1 repeated dimension)";
        return false;
    }
    ins.N = (int)dim;
    ins.T.assign(vals.begin() + off, vals.begin() + off + need);
    size_t sl = path.find_last_of('/');
    ins.name = (sl == string::npos) ? path : path.substr(sl + 1);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Independent validity check on the ORIGINAL matrix (permutation, precedence, cost from scratch)
// ---------------------------------------------------------------------------------------------
static bool validatePath(const Instance& I, const vector<int>& p, ll& cost, string& err) {
    const int N = I.N;
    if ((int)p.size() != N) { err = "path length != N"; return false; }
    vector<int> pos(N, -1);
    for (int k = 0; k < N; k++) {
        if (p[k] < 0 || p[k] >= N) { err = "node out of range"; return false; }
        if (pos[p[k]] != -1) { err = "node repeated"; return false; }
        pos[p[k]] = k;
    }
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++)
            if (i != j && I.T[(size_t)i * N + j] < 0 && !(pos[j] < pos[i])) {
                err = "precedence violated: " + to_string(i) + " depends on " + to_string(j);
                return false;
            }
    cost = 0;
    for (int k = 0; k + 1 < N; k++) {
        ll c = I.T[(size_t)p[k] * N + p[k + 1]];
        if (c < 0) { err = "negative arc used"; return false; }
        cost += c;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Preprocessed problem: transitive closure, start/end derivation (dummy nodes if several),
// static edge feasibility, cost matrix with BIG for infeasible arcs.
// ---------------------------------------------------------------------------------------------
struct Prob {
    int N = 0, n = 0, s = -1, t = -1, W = 0;
    vector<u64> succ, pred;  // closure bitsets, n*W
    vector<ll> C;            // n*n; BIG if arc can never be used
    ll BIG = 0;
    vector<vector<int>> immSucc;
    vector<int> immPredCnt;
    bool cyclic = false;
    int numRoots = 0, numLeaves = 0;
    inline bool lt(int a, int b) const { return (succ[(size_t)a * W + (b >> 6)] >> (b & 63)) & 1ULL; }
};

static bool buildProb(const Instance& I, Prob& P, string& err) {
    const int N = I.N;
    const int W0 = (N + 63) / 64;
    vector<u64> r0((size_t)N * W0, 0);
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++)
            if (i != j && I.T[(size_t)i * N + j] < 0) r0[(size_t)j * W0 + (i >> 6)] |= 1ULL << (i & 63);  // j before i
    for (int k = 0; k < N; k++)
        for (int i = 0; i < N; i++)
            if ((r0[(size_t)i * W0 + (k >> 6)] >> (k & 63)) & 1ULL) {
                u64* a = &r0[(size_t)i * W0];
                const u64* b = &r0[(size_t)k * W0];
                for (int w = 0; w < W0; w++) a[w] |= b[w];
            }
    P.cyclic = false;
    for (int i = 0; i < N; i++)
        if ((r0[(size_t)i * W0 + (i >> 6)] >> (i & 63)) & 1ULL) P.cyclic = true;
    if (P.cyclic) { err = "precedence constraints contain a cycle (no feasible path)"; return false; }
    vector<char> hasPred(N, 0), hasSucc(N, 0);
    for (int a = 0; a < N; a++)
        for (int b = 0; b < N; b++)
            if ((r0[(size_t)a * W0 + (b >> 6)] >> (b & 63)) & 1ULL) { hasSucc[a] = 1; hasPred[b] = 1; }
    vector<int> roots, leaves;
    for (int i = 0; i < N; i++) {
        if (!hasPred[i]) roots.push_back(i);
        if (!hasSucc[i]) leaves.push_back(i);
    }
    P.numRoots = (int)roots.size();
    P.numLeaves = (int)leaves.size();
    const bool needS = roots.size() > 1, needT = leaves.size() > 1;
    const int n = N + (needS ? 1 : 0) + (needT ? 1 : 0);
    const int sIdx = needS ? N : roots[0];
    const int tIdx = needT ? N + (needS ? 1 : 0) : leaves[0];
    P.N = N; P.n = n; P.s = sIdx; P.t = tIdx; P.W = (n + 63) / 64;
    const int W = P.W;
    P.succ.assign((size_t)n * W, 0);
    P.pred.assign((size_t)n * W, 0);
    for (int i = 0; i < N; i++)
        for (int w = 0; w < W0; w++) P.succ[(size_t)i * W + w] = r0[(size_t)i * W0 + w];
    if (needS)
        for (int j = 0; j < n; j++)
            if (j != sIdx) P.succ[(size_t)sIdx * W + (j >> 6)] |= 1ULL << (j & 63);
    if (needT)
        for (int v = 0; v < n; v++)
            if (v != tIdx) P.succ[(size_t)v * W + (tIdx >> 6)] |= 1ULL << (tIdx & 63);
    for (int a = 0; a < n; a++)
        for (int b = 0; b < n; b++)
            if (P.lt(a, b)) P.pred[(size_t)b * W + (a >> 6)] |= 1ULL << (a & 63);
    auto feas = [&](int i, int j) -> bool {
        if (i == j || P.lt(j, i)) return false;
        const u64* a = &P.succ[(size_t)i * W];
        const u64* b = &P.pred[(size_t)j * W];
        for (int w = 0; w < W; w++)
            if (a[w] & b[w]) return false;  // some k with i<k<j must sit between them
        return true;
    };
    P.C.assign((size_t)n * n, 0);
    vector<char> F((size_t)n * n, 0);
    ll maxC = 1;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            bool f = feas(i, j);
            F[(size_t)i * n + j] = f;
            if (f && i < N && j < N) {
                ll c = I.T[(size_t)i * N + j];
                if (c < 0) { err = "internal: feasible arc with negative entry"; return false; }
                maxC = max(maxC, c);
            }
        }
    __int128 big = (__int128)(n + 1) * maxC + 1;
    if (big > (__int128)100000000000000000LL) { err = "cost values too large"; return false; }
    P.BIG = (ll)big;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            P.C[(size_t)i * n + j] = F[(size_t)i * n + j] ? ((i < N && j < N) ? I.T[(size_t)i * N + j] : 0) : P.BIG;
    P.immSucc.assign(n, {});
    P.immPredCnt.assign(n, 0);
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            if (P.lt(i, j) && F[(size_t)i * n + j]) { P.immSucc[i].push_back(j); P.immPredCnt[j]++; }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Incumbent (best solution shared by all threads)
// ---------------------------------------------------------------------------------------------
struct Incumbent {
    atomic<ll> cost{LLONG_MAX / 4};
    atomic<ll> rootLB{-1};
    atomic<ll> lastImproveNs{0};
    atomic<int> version{0};
    mutex m;
    vector<int> path;  // extended indices, length n
    function<void()> onOptimal;
    const Prob* P = nullptr;
    static const char* tag();
    bool offer(ll c, const vector<int>& p) {
        if (c >= cost.load(memory_order_relaxed)) return false;
        if (g_debug && P) {  // re-validate in debug mode
            const int n = P->n;
            vector<int> pos(n, -1);
            bool ok = (int)p.size() == n;
            ll cc = 0;
            for (int k = 0; ok && k < n; k++) { if (p[k] < 0 || p[k] >= n || pos[p[k]] != -1) ok = false; else pos[p[k]] = k; }
            for (int a = 0; ok && a < n; a++)
                for (int b = 0; b < n; b++)
                    if (P->lt(a, b) && !(pos[a] < pos[b])) ok = false;
            for (int k = 0; ok && k + 1 < n; k++) cc += P->C[(size_t)p[k] * n + p[k + 1]];
            if (!ok || cc != c) { fprintf(stderr, "DEBUG: invalid incumbent offered (ok=%d cc=%lld c=%lld)\n", (int)ok, cc, c); abort(); }
        }
        bool improved = false;
        {
            lock_guard<mutex> lk(m);
            if (c < cost.load()) {
                cost.store(c);
                path = p;
                version++;
                lastImproveNs.store((ll)(elapsed() * 1e9));
                improved = true;
            }
        }
        if (improved) {  // throttled progress log (stderr only)
            static atomic<ll> lastLogNs{-1000000000LL};
            ll now = (ll)(elapsed() * 1e9), prev = lastLogNs.load();
            if (now - prev > 250000000LL && lastLogNs.compare_exchange_strong(prev, now)) LOG("incumbent %lld (%s)", c, tag());
        }
        if (improved && c <= rootLB.load() && onOptimal) onOptimal();
        return improved;
    }
    void snapshot(vector<int>& out, ll& c) {
        lock_guard<mutex> lk(m);
        out = path;
        c = cost.load();
    }
};

static thread_local const char* t_tag = "init";
const char* Incumbent::tag() { return t_tag; }

// ---------------------------------------------------------------------------------------------
// NUMA / CPU topology (best effort, Linux sysfs)
// ---------------------------------------------------------------------------------------------
static vector<int> parseCpuList(const string& s) {
    vector<int> r;
    stringstream ss(s);
    string part;
    while (getline(ss, part, ',')) {
        part = trimS(part);
        if (part.empty()) continue;
        size_t d = part.find('-');
        if (d == string::npos) r.push_back(atoi(part.c_str()));
        else {
            int a = atoi(part.substr(0, d).c_str()), b = atoi(part.substr(d + 1).c_str());
            for (int x = a; x <= b; x++) r.push_back(x);
        }
    }
    return r;
}
static bool readFile(const string& p, string& out) {
    ifstream f(p);
    if (!f) return false;
    stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}
struct Topo {
    vector<int> pinOrder;         // cpu ids in the order worker threads are assigned
    map<int, int> cpuNode;        // cpu -> numa node
    vector<int> memNodes;         // numa nodes that own memory
    int numNodesWithCpus = 0;
    int physicalCores = 0;
};
static Topo detectTopo() {
    Topo T;
    cpu_set_t cs;
    CPU_ZERO(&cs);
    if (sched_getaffinity(0, sizeof(cs), &cs) != 0) return T;
    set<int> allowed;
    for (int c = 0; c < CPU_SETSIZE; c++)
        if (CPU_ISSET(c, &cs)) allowed.insert(c);
    // node membership
    map<int, vector<int>> nodeCpus;
    for (int nd = 0; nd < 256; nd++) {
        string s;
        if (!readFile("/sys/devices/system/node/node" + to_string(nd) + "/cpulist", s)) {
            if (nd > 0 && nodeCpus.empty()) continue;
            if (nd > 8 && nodeCpus.size() + 8 < (size_t)nd) break;
            continue;
        }
        for (int c : parseCpuList(s))
            if (allowed.count(c)) { nodeCpus[nd].push_back(c); T.cpuNode[c] = nd; }
        string mi;
        if (readFile("/sys/devices/system/node/node" + to_string(nd) + "/meminfo", mi)) {
            size_t p = mi.find("MemTotal:");
            if (p != string::npos && atoll(mi.c_str() + p + 9) > 0) T.memNodes.push_back(nd);
        }
    }
    if (nodeCpus.empty()) nodeCpus[0] = vector<int>(allowed.begin(), allowed.end());
    // leaders (one logical cpu per physical core) vs SMT siblings
    map<int, vector<int>> leaders, smt;
    set<int> seenLeader;
    for (auto& kv : nodeCpus) {
        for (int c : kv.second) {
            string s;
            int leader = c;
            if (readFile("/sys/devices/system/cpu/cpu" + to_string(c) + "/topology/thread_siblings_list", s)) {
                vector<int> sib = parseCpuList(s);
                int mn = c;
                for (int x : sib)
                    if (allowed.count(x)) mn = min(mn, x);
                leader = mn;
            }
            if (leader == c) { leaders[kv.first].push_back(c); T.physicalCores++; }
            else smt[kv.first].push_back(c);
        }
    }
    T.numNodesWithCpus = (int)nodeCpus.size();
    auto rr = [&](map<int, vector<int>>& m) {
        size_t idx = 0;
        bool any = true;
        while (any) {
            any = false;
            for (auto& kv : m)
                if (idx < kv.second.size()) { T.pinOrder.push_back(kv.second[idx]); any = true; }
            idx++;
        }
    };
    rr(leaders);
    rr(smt);
    return T;
}
static void pinToCpu(int cpu) {
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(cpu, &cs);
    pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
}

// ---------------------------------------------------------------------------------------------
// History table: bucketed hash with full keys + per-bucket spin lock.  Entries are only *hints*
// (domination), so evicting an entry is always safe.
// ---------------------------------------------------------------------------------------------
class HistTable {
public:
    int W = 0;
    size_t nb = 0, bucketBytes = 0, totalBytes = 0;
    char* mem = nullptr;
    static constexpr int SLOTS = 4;
    bool init(int W_, size_t budget, const vector<int>& memNodes) {
        W = W_;
        bucketBytes = (8 + (size_t)SLOTS * 8 * (2 + W) + 63) / 64 * 64;
        size_t want = budget / bucketBytes;
        if (want < 1024) want = 1024;
        nb = 1;
        while (nb * 2 <= want) nb *= 2;
        for (;;) {
            totalBytes = nb * bucketBytes;
            void* p = mmap(nullptr, totalBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            if (p != MAP_FAILED) { mem = (char*)p; break; }
            if (nb <= 1024) return false;
            nb /= 2;
        }
        if (memNodes.size() > 1) {  // interleave pages across NUMA nodes that have memory
            unsigned long mask[16];
            memset(mask, 0, sizeof(mask));
            int maxn = 0;
            for (int nd : memNodes)
                if (nd < 1024) { mask[nd / 64] |= 1UL << (nd % 64); maxn = max(maxn, nd); }
            long r = syscall(SYS_mbind, mem, totalBytes, 3 /*MPOL_INTERLEAVE*/, mask, 1024UL, 0U);
            if (r != 0) LOG("mbind(interleave) failed (ignored)");
        }
        return true;
    }
    ~HistTable() { if (mem) munmap(mem, totalBytes); }
    // returns true if the node (key,last) with prefix cost g is dominated by an existing entry
    bool dominated(const u64* key, u64 h, int last, ll g, int depth) {
        char* bp = mem + (h & (nb - 1)) * bucketBytes;
        u64* lock = (u64*)bp;
        while (__atomic_exchange_n(lock, 1ULL, __ATOMIC_ACQUIRE)) {
            while (__atomic_load_n(lock, __ATOMIC_RELAXED)) __builtin_ia32_pause();
        }
        u64* slots = (u64*)(bp + 8);
        const size_t SW = 2 + W;
        u64* freeS = nullptr;
        u64* victim = nullptr;
        int vdepth = -1;
        bool res = false, found = false;
        for (int i = 0; i < SLOTS; i++) {
            u64* sl = slots + (size_t)i * SW;
            u64 meta = sl[0];
            if (!(meta >> 63)) { if (!freeS) freeS = sl; continue; }
            if ((int)(meta & 0xFFFF) == last && memcmp(sl + 2, key, (size_t)W * 8) == 0) {
                found = true;
#if SOP_MUTANT == 2
                if ((ll)sl[1] <= g + 1) res = true; else sl[1] = (u64)g;
#else
                if ((ll)sl[1] <= g) res = true; else sl[1] = (u64)g;
#endif
                break;
            }
            int dd = (int)((meta >> 16) & 0xFFFF);
            if (dd > vdepth) { vdepth = dd; victim = sl; }
        }
        if (!found) {
            u64* tgt = freeS ? freeS : ((victim && depth <= vdepth) ? victim : nullptr);
            if (tgt) {
                tgt[0] = (1ULL << 63) | ((u64)depth << 16) | (u64)last;
                tgt[1] = (u64)g;
                memcpy(tgt + 2, key, (size_t)W * 8);
            }
        }
        __atomic_store_n(lock, 0ULL, __ATOMIC_RELEASE);
        return res;
    }
};

// ---------------------------------------------------------------------------------------------
// Heuristic: randomized greedy + iterated local search (segment insertion / or-opt, both
// directions, precedence-aware).  Used for a good initial/concurrent upper bound.
// ---------------------------------------------------------------------------------------------
class Heur {
public:
    Heur(const Prob& P_, Incumbent& inc_, u64 seed) : P(P_), inc(inc_), rng(seed), n(P_.n), s(P_.s), t(P_.t), C(P_.C.data()), BIG(P_.BIG) {
        pos.assign(n, 0);
    }
    void greedy(vector<int>& out, double rnd) {
        vector<int> rp = P.immPredCnt, ready;
        out.clear();
        out.push_back(s);
        for (int y : P.immSucc[s])
            if (--rp[y] == 0) ready.push_back(y);
        while ((int)out.size() < n) {
            int last = out.back(), pick = -1;
            if (ready.empty()) break;  // cannot happen for acyclic precedence
            if (rng.uni() < rnd) pick = rng.below((int)ready.size());
            else {
                ll best = LLONG_MAX;
                int cnt = 0;
                for (int k = 0; k < (int)ready.size(); k++) {
                    ll c = C[(size_t)last * n + ready[k]];
                    if (c < best) { best = c; pick = k; cnt = 1; }
                    else if (c == best && rng.below(++cnt) == 0) pick = k;
                }
            }
            int v = ready[pick];
            ready[pick] = ready.back();
            ready.pop_back();
            out.push_back(v);
            for (int y : P.immSucc[v])
                if (--rp[y] == 0) ready.push_back(y);
        }
    }
    ll evalSeq(const vector<int>& q) const {
        ll c = 0;
        for (int k = 0; k + 1 < n; k++) c += C[(size_t)q[k] * n + q[k + 1]];
        return c;
    }
    bool checkSeq(const vector<int>& q) const {  // debug: linear extension + finite arcs
        if ((int)q.size() != n) return false;
        vector<int> ps(n, -1);
        for (int k = 0; k < n; k++) { if (ps[q[k]] != -1) return false; ps[q[k]] = k; }
        for (int a = 0; a < n; a++)
            for (int b = 0; b < n; b++)
                if (P.lt(a, b) && !(ps[a] < ps[b])) return false;
        for (int k = 0; k + 1 < n; k++)
            if (C[(size_t)q[k] * n + q[k + 1]] >= BIG) return false;
        return q[0] == s && q[n - 1] == t;
    }
    // Run until stopFn() is true.
    void run(const function<bool()>& stopFn) {
        t_tag = "ILS";
        if (n <= 3) return;
        vector<int> cur, best, trial;
        greedy(cur, 0.0);
        seq = cur;
        setPos();
        cost = evalSeq(seq);
        inc.offer(cost, seq);
        localSearch();
        cur = seq;
        ll curCost = cost;
        inc.offer(curCost, cur);
        best = cur;
        ll bestCost = curCost;
        int seenVersion = inc.version.load();
        long iter = 0, stagn = 0;
        while (!stopFn()) {
            iter++;
            if (inc.version.load() != seenVersion) {
                seenVersion = inc.version.load();
                vector<int> gp;
                ll gc;
                inc.snapshot(gp, gc);
                if (gc < bestCost) { best = gp; bestCost = gc; cur = gp; curCost = gc; stagn = 0; }
            }
            seq = cur;
            setPos();
            int kicks = 2 + rng.below(4) + (stagn > 200 ? 6 : 0);
            for (int k = 0; k < kicks; k++) randomMove();
            cost = evalSeq(seq);
            localSearch();
            if (g_debug && !checkSeq(seq)) { fprintf(stderr, "DEBUG: ILS produced invalid sequence\n"); abort(); }
            if (cost <= curCost) { cur = seq; curCost = cost; }
            if (cost < bestCost) { best = seq; bestCost = cost; stagn = 0; inc.offer(cost, seq); }
            else stagn++;
            if (stagn > 100 + n) {  // restart from a fresh randomized greedy
                greedy(cur, 0.15);
                seq = cur;
                setPos();
                cost = evalSeq(seq);
                localSearch();
                cur = seq;
                curCost = cost;
                if (cost < bestCost) { best = seq; bestCost = cost; inc.offer(cost, seq); }
                stagn = 0;
            }
        }
    }

private:
    const Prob& P;
    Incumbent& inc;
    Rng rng;
    int n, s, t;
    const ll* C;
    ll BIG;
    vector<int> seq, pos;
    ll cost = 0;
    static constexpr int LMAX = 6;
    void setPos() { for (int k = 0; k < n; k++) pos[seq[k]] = k; }
    void applyForward(int i, int j, int q) {  // move seq[i..j] to just after position q (q>j)
        rotate(seq.begin() + i, seq.begin() + j + 1, seq.begin() + q + 1);
        for (int k = i; k <= q; k++) pos[seq[k]] = k;
    }
    void applyBackward(int i, int j, int q) {  // move seq[i..j] to between positions q and q+1 (q+1<=i-1)
        rotate(seq.begin() + q + 1, seq.begin() + i, seq.begin() + j + 1);
        for (int k = q + 1; k <= j; k++) pos[seq[k]] = k;
    }
    void randomMove() {
        const int m = n - 2;
        if (m < 2) return;
        int L = 1 + rng.below(min(m, 1 + (int)(rng.below(3) == 0 ? 20 : 5)));
        int i = 1 + rng.below(m - L + 1), j = i + L - 1;
        int qf = j + 1;
        while (qf <= n - 2) {
            int w = seq[qf];
            bool bad = false;
            for (int k = i; k <= j && !bad; k++) bad = P.lt(seq[k], w);
            if (bad) break;
            qf++;
        }
        int cf = qf - 1 - j;  // valid q in [j+1, qf-1]
        int qb = i - 2;
        while (qb >= 0) {
            int w = seq[qb + 1];
            bool bad = false;
            for (int k = i; k <= j && !bad; k++) bad = P.lt(w, seq[k]);
            if (bad) break;
            qb--;
        }
        int cb = i - 2 - qb;  // valid q in [qb+1, i-2]
        int tot = cf + cb;
        if (tot <= 0) return;
        int r = rng.below(tot);
        if (r < cf) applyForward(i, j, j + 1 + r);
        else applyBackward(i, j, qb + 1 + (r - cf));
    }
    void localSearch() {
        vector<int> ord;
        for (int guard = 0; guard < 1000; guard++) {
            bool any = false;
            ord.clear();
            for (int k = 1; k <= n - 2; k++) ord.push_back(k);
            for (int k = (int)ord.size() - 1; k > 0; k--) swap(ord[k], ord[rng.below(k + 1)]);
            for (int i : ord) {
                for (int L = 1; L <= LMAX; L++) {
                    int j = i + L - 1;
                    if (j > n - 2) break;
                    int a = seq[i - 1], b = seq[i], c = seq[j], d = seq[j + 1];
                    ll cad = C[(size_t)a * n + d];
                    if (cad >= BIG) continue;
                    ll rg = C[(size_t)a * n + b] + C[(size_t)c * n + d] - cad;
                    ll bestDelta = 0;
                    int bq = -1;
                    bool bfw = false;
                    for (int q = j + 1; q <= n - 2; q++) {
                        int w = seq[q];
                        bool bad = false;
                        for (int k = i; k <= j && !bad; k++) bad = P.lt(seq[k], w);
                        if (bad) break;
                        int r = seq[q + 1];
                        ll cwb = C[(size_t)w * n + b], ccr = C[(size_t)c * n + r];
                        if (cwb >= BIG || ccr >= BIG) continue;
                        ll delta = cwb + ccr - C[(size_t)w * n + r] - rg;
                        if (delta < bestDelta) { bestDelta = delta; bq = q; bfw = true; }
                    }
                    for (int q = i - 2; q >= 0; q--) {
                        int w = seq[q + 1];
                        bool bad = false;
                        for (int k = i; k <= j && !bad; k++) bad = P.lt(w, seq[k]);
                        if (bad) break;
                        int p = seq[q];
                        ll cpb = C[(size_t)p * n + b], ccw = C[(size_t)c * n + w];
                        if (cpb >= BIG || ccw >= BIG) continue;
                        ll delta = cpb + ccw - C[(size_t)p * n + w] - rg;
                        if (delta < bestDelta) { bestDelta = delta; bq = q; bfw = false; }
                    }
                    if (bq >= 0) {
                        if (bfw) applyForward(i, j, bq);
                        else applyBackward(i, j, bq);
                        cost += bestDelta;
                        any = true;
                    }
                }
            }
            if (!any) break;
        }
    }
};

// ---------------------------------------------------------------------------------------------
// Shared state for the parallel B&B
// ---------------------------------------------------------------------------------------------
struct Child { int node; ll lb; };
struct Task { vector<int> path; ll lb; };
struct TaskCmp {
    bool operator()(const Task& a, const Task& b) const {
        if (a.lb != b.lb) return a.lb > b.lb;
        return a.path.size() > b.path.size();
    }
};
struct RootState { vector<ll> u, v; vector<int> rm, cm; ll ap = 0; };

struct Shared {
    const Prob* P = nullptr;
    Incumbent* inc = nullptr;
    HistTable* hist = nullptr;
    vector<u64> Z, ZL;
    RootState root;
    mutex qm;
    condition_variable cv;
    priority_queue<Task, vector<Task>, TaskCmp> q;
    int active = 0;
    atomic<int> idle{0};
    atomic<size_t> qsize{0};
    bool done = false;
    atomic<bool> doneA{false};
    atomic<bool> stop{false};
    atomic<bool> optFlag{false};
    bool hasDeadline = false;
    Clock::time_point deadline;
    atomic<ll> totalNodes{0};
    // NUMA-local replicas of the cost matrix (only used for big matrices on multi-node machines)
    const Topo* topo = nullptr;
    mutex repM;
    map<int, vector<ll>> cRep;

    bool timeUp() const { return hasDeadline && Clock::now() >= deadline; }
    void wake() {
        { lock_guard<mutex> lk(qm); }
        cv.notify_all();
    }
    void requestStop() {
        stop.store(true);
        wake();
    }
    void pushMany(vector<Task>& ts) {
        if (ts.empty()) return;
        {
            lock_guard<mutex> lk(qm);
            for (auto& t : ts) q.push(std::move(t));
            qsize.store(q.size());
        }
        cv.notify_all();
    }
    bool getTask(Task& out) {
        unique_lock<mutex> lk(qm);
        active--;
        idle.fetch_add(1);
        for (;;) {
            if (stop.load() || optFlag.load()) { idle.fetch_sub(1); return false; }
            if (!q.empty()) {
                out = std::move(const_cast<Task&>(q.top()));
                q.pop();
                qsize.store(q.size());
                idle.fetch_sub(1);
                active++;
                return true;
            }
            if (done) { idle.fetch_sub(1); return false; }
            if (active == 0) {
                done = true;
                doneA.store(true);
                cv.notify_all();
                idle.fetch_sub(1);
                return false;
            }
            cv.wait(lk);
        }
    }
    bool joinAsWorker() {  // used by the heuristic thread once it stagnates
        lock_guard<mutex> lk(qm);
        if (done || stop.load() || optFlag.load()) return false;
        active++;
        return true;
    }
    const ll* localC(int cpu) {
        const Prob& p = *P;
        if (!topo || topo->numNodesWithCpus < 2 || (size_t)p.n * p.n * 8 < (1u << 20)) return p.C.data();
        auto it = topo->cpuNode.find(cpu);
        if (it == topo->cpuNode.end()) return p.C.data();
        lock_guard<mutex> lk(repM);
        auto& v = cRep[it->second];
        if (v.empty()) v = p.C;  // first-touch by the calling (pinned) thread
        return v.data();
    }
};

// ---------------------------------------------------------------------------------------------
// B&B worker: DFS with AP lower bound (dynamic Hungarian) + history domination
// ---------------------------------------------------------------------------------------------
class Worker {
public:
    Worker(Shared& S_, int cpuHint) : S(S_), P(*S_.P), n(S_.P->n), s(S_.P->s), t(S_.P->t), W(S_.P->W), BIG(S_.P->BIG) {
        C = S.localC(cpuHint);
        path.assign(n + 2, 0);
        gArr.assign(n + 3, 0);
        scheduled.assign(n, 0);
        remPred.assign(n, 0);
        bits.assign(W, 0);
        colList.reserve(n);
        colPos.assign(n, 0);
        u.assign(n, 0);
        v.assign(n, 0);
        rowMatch.assign(n, -1);
        colMatch.assign(n + 1, -1);
        snapU.assign((size_t)(n + 3) * n, 0);
        snapV.assign((size_t)(n + 3) * n, 0);
        snapRM.assign((size_t)(n + 3) * n, 0);
        snapCM.assign((size_t)(n + 3) * (n + 1), 0);
        ch.assign(n + 3, {});
        ci.assign(n + 3, 0);
        minv.assign(n + 1, 0);
        way.assign(n + 1, 0);
        used.assign(n + 1, 0);
        vis.reserve(n);
    }
    ll nodeCount() const { return nodes; }

    bool computeRoot() {
        resetRootState();
        for (int r = 0; r < n; r++) {
            if (r == t) continue;
            if ((r & 15) == 0 && S.timeUp()) return false;
            augment(r);
        }
        ll ap = apValue();
        saveSnap(1);
        S.root.u = u; S.root.v = v; S.root.rm = rowMatch; S.root.cm = colMatch; S.root.ap = ap;
        return true;
    }
    ll rootAP() const { return S.root.ap; }
    void collectRootTasks(vector<Task>& out, ll incCost) {
        loadRoot();
        for (int j : colList) {
            if (remPred[j] != 0) continue;
            ll c = C[(size_t)s * n + j];
            if (c >= BIG) continue;
            ll lb = S.root.ap + c - u[s] - v[j];
            if (lb >= incCost) continue;
            Task tk;
            tk.path = {s, j};
            tk.lb = lb;
            out.push_back(std::move(tk));
        }
    }
    void run() {
        Task tk;
        t_tag = "B&B";
        while (S.getTask(tk)) {
            if (tk.lb >= S.inc->cost.load(memory_order_relaxed)) continue;
            exploreTask(tk);
            if (S.stop.load() || S.optFlag.load()) break;
        }
        S.totalNodes.fetch_add(nodes);
        nodes = 0;
    }

private:
    Shared& S;
    const Prob& P;
    const int n, s, t, W;
    const ll BIG;
    const ll* C = nullptr;
    vector<int> path;
    vector<ll> gArr;
    int depth = 0, base = 0, last = 0;
    vector<char> scheduled;
    vector<int> remPred;
    vector<u64> bits;
    u64 hset = 0;
    vector<int> colList, colPos;
    vector<ll> u, v;
    vector<int> rowMatch, colMatch;
    vector<ll> snapU, snapV;
    vector<int> snapRM, snapCM;
    vector<vector<Child>> ch;
    vector<int> ci;
    vector<ll> minv;
    vector<int> way;
    vector<char> used;
    vector<int> vis;
    ll nodes = 0;
    long tick = 0;
    static constexpr ll INF = LLONG_MAX / 4;
    static constexpr int HMIN = 6;           // history lookups only when >= HMIN nodes remain
    static constexpr int DONATE_MIN = 8;     // donate only frames with >= this many unscheduled nodes

    void removeCol(int x) {
        int p = colPos[x], y = colList.back();
        colList[p] = y;
        colPos[y] = p;
        colList.pop_back();
    }
    void addCol(int x) {
        colPos[x] = (int)colList.size();
        colList.push_back(x);
    }
    void saveSnap(int d) {
        memcpy(&snapU[(size_t)d * n], u.data(), sizeof(ll) * n);
        memcpy(&snapV[(size_t)d * n], v.data(), sizeof(ll) * n);
        memcpy(&snapRM[(size_t)d * n], rowMatch.data(), sizeof(int) * n);
        memcpy(&snapCM[(size_t)d * (n + 1)], colMatch.data(), sizeof(int) * (n + 1));
    }
    void restoreSnap(int d) {
        memcpy(u.data(), &snapU[(size_t)d * n], sizeof(ll) * n);
        memcpy(v.data(), &snapV[(size_t)d * n], sizeof(ll) * n);
        memcpy(rowMatch.data(), &snapRM[(size_t)d * n], sizeof(int) * n);
        memcpy(colMatch.data(), &snapCM[(size_t)d * (n + 1)], sizeof(int) * (n + 1));
    }
    void resetRootState() {
        fill(scheduled.begin(), scheduled.end(), 0);
        fill(bits.begin(), bits.end(), 0);
        hset = 0;
        path[0] = s;
        gArr[1] = 0;
        depth = 1;
        scheduled[s] = 1;
        bits[s >> 6] |= 1ULL << (s & 63);
        hset ^= S.Z[s];
        remPred = P.immPredCnt;
        for (int y : P.immSucc[s]) remPred[y]--;
        colList.clear();
        for (int j = 0; j < n; j++)
            if (j != s) addCol(j);
        fill(u.begin(), u.end(), 0);
        fill(v.begin(), v.end(), 0);
        fill(rowMatch.begin(), rowMatch.end(), -1);
        fill(colMatch.begin(), colMatch.end(), -1);
        last = s;
    }
    void loadRoot() {
        resetRootState();
        u = S.root.u; v = S.root.v; rowMatch = S.root.rm; colMatch = S.root.cm;
        saveSnap(1);
    }
    ll apValue() const {
        ll sum = 0;
        for (int j : colList) sum += v[j] + u[colMatch[j]];
        return sum;
    }
    // Shortest augmenting path from free row r (Jonker-Volgenant / e-maxx Hungarian step).
    void augment(int r) {
        const int m = (int)colList.size();
        for (int k = 0; k < m; k++) { int j = colList[k]; minv[j] = INF; used[j] = 0; }
        colMatch[n] = r;
        int j0 = n;
        vis.clear();
        for (;;) {
            int i0 = colMatch[j0];
            const ll* crow = C + (size_t)i0 * n;
            const bool restr = (i0 == last);
            const ll ui = u[i0];
            ll delta = INF;
            int j1 = -1;
            for (int k = 0; k < m; k++) {
                int j = colList[k];
                if (used[j]) continue;
                ll c = (restr && remPred[j] != 0) ? BIG : crow[j];
                ll cur = c - ui - v[j];
                if (cur < minv[j]) { minv[j] = cur; way[j] = j0; }
                if (minv[j] < delta) { delta = minv[j]; j1 = j; }
            }
            for (int q : vis) { u[colMatch[q]] += delta; v[q] -= delta; }
            u[r] += delta;
            for (int k = 0; k < m; k++) { int j = colList[k]; if (!used[j]) minv[j] -= delta; }
            j0 = j1;
            if (colMatch[j0] == -1) break;
            used[j0] = 1;
            vis.push_back(j0);
        }
        do {
            int j1 = way[j0];
            int i = (j1 == n) ? r : colMatch[j1];
            colMatch[j0] = i;
            rowMatch[i] = j0;
            j0 = j1;
        } while (j0 != n);
        colMatch[n] = -1;
    }
    // Parent state (rows {l} u U\{t}, cols U) -> child state after scheduling x: delete row l,
    // delete column x, restrict row x to ready columns, re-augment at most two free rows.
    void apTransition(int l, int x) {
        int freeRows[2], nfr = 0;
        int cl = rowMatch[l], rx = colMatch[x];
        rowMatch[l] = -1;
        colMatch[x] = -1;
        if (cl != x) {
            rowMatch[rx] = -1;
            colMatch[cl] = -1;
            freeRows[nfr++] = rx;
        }
        int m = rowMatch[x];
        if (m >= 0 && remPred[m] != 0) {
            rowMatch[x] = -1;
            colMatch[m] = -1;
            freeRows[nfr++] = x;
        }
        last = x;
        for (int k = 0; k < nfr; k++) augment(freeRows[k]);
    }
    void undoSched(int x, bool apDirty, int d) {
        if (apDirty) restoreSnap(d);
        addCol(x);
        for (int y : P.immSucc[x]) remPred[y]++;
        hset ^= S.Z[x];
        bits[x >> 6] &= ~(1ULL << (x & 63));
        scheduled[x] = 0;
        depth = d;
        last = path[d - 1];
    }
    void undoChild() { undoSched(path[depth - 1], true, depth - 1); }

    void buildChildren(ll ng, ll ap, int x) {
        auto& vec = ch[depth];
        vec.clear();
        ci[depth] = 0;
        const ll cutoff = S.inc->cost.load(memory_order_relaxed);
        const ll b0 = ng + ap;
        for (int j : colList) {
            if (remPred[j] != 0) continue;
            ll c = C[(size_t)x * n + j];
            if (c >= BIG) continue;
            ll rc = c - u[x] - v[j];
#if SOP_MUTANT == 3
            ll lb = b0 + rc + 1;
#else
            ll lb = b0 + rc;
#endif
            if (lb < cutoff) vec.push_back({j, lb});
        }
        sort(vec.begin(), vec.end(), [](const Child& a, const Child& b) { return a.lb < b.lb || (a.lb == b.lb && a.node < b.node); });
    }

    // Schedule x after the current last node.  Returns true if the node survived all pruning
    // (state now at depth+1 with children built); false if pruned (state restored).
    bool enter(int x, bool replay) {
        const int d = depth;
        const int l = path[d - 1];
        const ll ng = gArr[d] + C[(size_t)l * n + x];
        path[d] = x;
        gArr[d + 1] = ng;
        depth = d + 1;
        scheduled[x] = 1;
        bits[x >> 6] |= 1ULL << (x & 63);
        hset ^= S.Z[x];
        for (int y : P.immSucc[x]) remPred[y]--;
        removeCol(x);
        if (!replay) nodes++;
        const int remaining = n - depth;
        bool ok = true, apDirty = false;
        if (remaining <= 1) {
            if (!replay && remaining == 1) {  // only t left: complete the path directly
                ll cl = C[(size_t)x * n + t];
                if (cl < BIG && ng + cl < S.inc->cost.load(memory_order_relaxed)) {
                    path[depth] = t;
                    vector<int> full(path.begin(), path.begin() + depth + 1);
                    S.inc->offer(ng + cl, full);
                }
            }
            ok = false;
        } else {
            if (!replay && S.hist && remaining >= HMIN) {
                u64 h = splitmix64(hset ^ S.ZL[x]);
                if (S.hist->dominated(bits.data(), h, x, ng, depth)) ok = false;
            }
            if (ok) {
                apDirty = true;
                apTransition(l, x);
                ll ap = apValue();
#if SOP_MUTANT == 1
                ll slack = 1;
#else
                ll slack = 0;
#endif
                if (ap >= BIG || ng + ap + slack >= S.inc->cost.load(memory_order_relaxed)) ok = false;
                else {
                    saveSnap(depth);
                    if (!replay) buildChildren(ng, ap, x);
                }
            }
        }
        if (!ok) { undoSched(x, apDirty, d); return false; }
        return true;
    }

    void donate() {
        for (int dd = base + 1; dd <= depth; dd++) {
            auto& vec = ch[dd];
            int rem = (int)vec.size() - ci[dd];
            if (rem <= 0) continue;
            if (n - dd < DONATE_MIN) break;
            vector<Task> tasks;
            vector<Child> keep;
            for (int q = ci[dd]; q < (int)vec.size(); q++) {
                if (((q - ci[dd]) & 1) == 0) {
                    Task tk;
                    tk.path.assign(path.begin(), path.begin() + dd);
                    tk.path.push_back(vec[q].node);
                    tk.lb = vec[q].lb;
                    tasks.push_back(std::move(tk));
                } else keep.push_back(vec[q]);
            }
            vec.resize(ci[dd]);
            for (auto& c : keep) vec.push_back(c);
            S.pushMany(tasks);
            return;
        }
    }

    void exploreTask(const Task& tk) {
        loadRoot();
        const int k = (int)tk.path.size();
        for (int i = 1; i <= k - 2; i++)
            if (!enter(tk.path[i], true)) return;  // ancestor pruned by a newer incumbent: task obsolete
        base = depth;
        enter(tk.path[k - 1], false);
        while (depth > base) {
            const int d = depth;
            if (ci[d] >= (int)ch[d].size()) { undoChild(); continue; }
            if ((++tick & 15) == 0) {
                if (S.stop.load(memory_order_relaxed) || S.optFlag.load(memory_order_relaxed)) return;
                if (S.timeUp()) { S.requestStop(); return; }
                if (S.idle.load(memory_order_relaxed) > 0 && S.qsize.load(memory_order_relaxed) == 0) donate();
                if (ci[d] >= (int)ch[d].size()) continue;
            }
            Child c = ch[d][ci[d]++];
            if (c.lb >= S.inc->cost.load(memory_order_relaxed)) { ci[d] = (int)ch[d].size(); continue; }  // sorted ascending
            enter(c.node, false);
        }
    }
};

// ---------------------------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------------------------
static string pathToString(const vector<int>& p) {
    string s;
    for (size_t i = 0; i < p.size(); i++) {
        if (i) s += ' ';
        s += to_string(p[i]);
    }
    return s;
}

struct Result {
    ll cost = -1;
    vector<int> path;
    bool optimal = false;
    ll nodes = 0;
};

static void emit(const Options& o, const Instance& I, const Result& R, double secs) {
    string header = "Instance,Instance Size,Time,Enumerated Nodes,Best Cost,Best Path,Optimal";
    char tbuf[64];
    snprintf(tbuf, sizeof(tbuf), "%.6f", secs);
    string row = I.name + "," + to_string(I.N) + "," + tbuf + "," + to_string(R.nodes) + "," + to_string(R.cost) + "," +
                 pathToString(R.path) + "," + (R.optimal ? "yes" : "no");
    if (!o.noHeader) printf("%s\n", header.c_str());
    printf("%s\n", row.c_str());
    fflush(stdout);
    if (!o.output.empty()) {
        struct stat st;
        bool needHeader = !(stat(o.output.c_str(), &st) == 0 && st.st_size > 0);
        FILE* f = fopen(o.output.c_str(), "a");
        if (!f) { fprintf(stderr, "error: cannot open output file '%s'\n", o.output.c_str()); return; }
        if (needHeader && !o.noHeader) fprintf(f, "%s\n", header.c_str());
        fprintf(f, "%s\n", row.c_str());
        fclose(f);
    }
}

static size_t defaultHistBudget(double memMB) {
    if (memMB > 0) return (size_t)(memMB * 1024 * 1024);
    double avail = 2048.0 * 1024 * 1024;
    string s;
    if (readFile("/proc/meminfo", s)) {
        size_t p = s.find("MemAvailable:");
        if (p != string::npos) avail = atof(s.c_str() + p + 13) * 1024.0;
    }
    string cg;
    if (readFile("/sys/fs/cgroup/memory.max", cg)) {
        cg = trimS(cg);
        if (!cg.empty() && isdigit((unsigned char)cg[0])) avail = min(avail, atof(cg.c_str()));
    }
    double b = min(avail * 0.30, 24.0 * 1024 * 1024 * 1024);
    return (size_t)max(b, 64.0 * 1024 * 1024);
}

int main(int argc, char** argv) {
    g_t0 = Clock::now();
    Options o;
    if (!parseArgs(argc, argv, o)) { usage(); return 1; }
    g_debug = getenv("SOP_DEBUG") != nullptr;
    Instance I;
    string err;
    if (!parseSop(o.file, I, err)) { fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
    LOG("parsed %s: N=%d", I.name.c_str(), I.N);

    Result R;
    auto finish = [&](bool optimal, const vector<int>& orig, ll nodes) -> int {
        R.nodes = nodes;
        if (orig.empty()) { R.cost = -1; R.optimal = false; }
        else {
            ll c;
            string e;
            if (!validatePath(I, orig, c, e)) { fprintf(stderr, "INTERNAL ERROR: final path failed validity check: %s\n", e.c_str()); return 2; }
            R.cost = c; R.path = orig; R.optimal = optimal;
        }
        emit(o, I, R, elapsed());
        return 0;
    };

    if (I.N == 1) return finish(true, vector<int>{0}, 0);
    Prob P;
    if (!buildProb(I, P, err)) {
        LOG("infeasible: %s", err.c_str());
        return finish(false, {}, 0);
    }
    LOG("n=%d (ext), roots=%d leaves=%d => start=%d end=%d%s", P.n, P.numRoots, P.numLeaves, P.s, P.t, (P.n > P.N) ? " [dummy start/end added]" : "");

    const int n = P.n;
    auto toOrig = [&](const vector<int>& ext) {
        vector<int> r;
        for (int v : ext)
            if (v < P.N) r.push_back(v);
        return r;
    };
    Incumbent inc;
    inc.P = &P;
    {
        Heur h0(P, inc, o.seed);
        vector<int> g;
        h0.greedy(g, 0.0);
        inc.offer(h0.evalSeq(g), g);
    }
    LOG("initial greedy cost=%lld", (long long)inc.cost.load());
    if (n == 2) return finish(true, toOrig(inc.path), 1);

    Shared S;
    S.P = &P;
    S.inc = &inc;
    if (o.timeLimit >= 0) {
        S.hasDeadline = true;
        S.deadline = g_t0 + chrono::duration_cast<Clock::duration>(chrono::duration<double>(o.timeLimit));
    }
    S.Z.resize(n);
    S.ZL.resize(n);
    for (int i = 0; i < n; i++) { S.Z[i] = splitmix64(0x1234567ULL + i * 2); S.ZL[i] = splitmix64(0x7654321ULL + i * 2 + 1); }

    int T = o.threads;
    Topo topo = detectTopo();
    if (T == 0) T = max(1, topo.physicalCores ? topo.physicalCores : (int)thread::hardware_concurrency());
    S.topo = &topo;
    const bool pin = !o.noPin && T > 1 && !topo.pinOrder.empty();
    auto cpuFor = [&](int idx) { return topo.pinOrder.empty() ? -1 : topo.pinOrder[idx % topo.pinOrder.size()]; };
    LOG("threads=%d pin=%d numa_nodes_with_cpus=%d mem_nodes=%d physical_cores=%d", T, (int)pin, topo.numNodesWithCpus, (int)topo.memNodes.size(), topo.physicalCores);

    HistTable hist;
    if (!o.noHistory) {
        size_t budget = defaultHistBudget(o.memMB);
        if (hist.init(P.W, budget, topo.memNodes)) { S.hist = &hist; LOG("history table: %.1f MB, %zu buckets x %d slots", hist.totalBytes / 1048576.0, hist.nb, HistTable::SLOTS); }
        else LOG("history table allocation failed; running without it");
    }

    bool proven = false;
    auto finishFromInc = [&](bool prov) -> int {
        vector<int> ext;
        ll c;
        inc.snapshot(ext, c);
        return finish(prov, toOrig(ext), S.totalNodes.load());
    };

    if (pin) pinToCpu(cpuFor(0));
    Worker w0(S, pin ? cpuFor(0) : -1);
    if (!w0.computeRoot()) {
        LOG("time limit reached while computing the root bound");
        return finishFromInc(false);
    }
    const ll rootLB = w0.rootAP();
    inc.rootLB.store(rootLB);
    S.totalNodes.fetch_add(1);  // the root node itself
    LOG("root AP lower bound = %lld, incumbent = %lld", (long long)rootLB, (long long)inc.cost.load());
    inc.onOptimal = [&]() { S.optFlag.store(true); S.wake(); };
    if (inc.cost.load() <= rootLB) {
        LOG("incumbent matches root lower bound: optimal");
        return finishFromInc(true);
    }

    auto stopCommon = [&]() -> bool {
        if (S.stop.load() || S.optFlag.load() || S.doneA.load()) return true;
        if (S.timeUp()) { S.requestStop(); return true; }
        return false;
    };

    if (T == 1) {
        if (!o.noHeur) {  // short ILS pre-phase, then B&B on this thread
            double budget = 0.02 + 0.003 * n;
            if (o.timeLimit >= 0) budget = min(budget, 0.1 * o.timeLimit);
            double tEnd = elapsed() + budget;
            Heur h(P, inc, o.seed + 1);
            h.run([&]() { return elapsed() >= tEnd || inc.cost.load() <= rootLB || stopCommon(); });
            LOG("after ILS pre-phase: incumbent = %lld", (long long)inc.cost.load());
        }
        if (inc.cost.load() <= rootLB) return finishFromInc(true);
        vector<Task> tasks;
        w0.collectRootTasks(tasks, inc.cost.load());
        S.active = 1;
        S.pushMany(tasks);
        w0.run();
    } else {
        vector<Task> tasks;
        w0.collectRootTasks(tasks, inc.cost.load());
        const int nBB = T - 1;
        S.active = nBB;
        S.pushMany(tasks);
        vector<thread> ths;
        for (int k = 0; k < nBB; k++)
            ths.emplace_back([&, k]() {
                int cpu = pin ? cpuFor(k + 1) : -1;
                if (pin) pinToCpu(cpu);
                Worker w(S, cpu);
                w.run();
            });
        // main thread: heuristic until it stagnates, then it joins the B&B pool
        if (!o.noHeur) {
            Heur h(P, inc, o.seed + 1);
            h.run([&]() {
                if (stopCommon()) return true;
                double now = elapsed(), since = now - inc.lastImproveNs.load() * 1e-9;
                return now > 1.0 && since > max(2.0, 0.3 * now);
            });
        }
        if (S.joinAsWorker()) w0.run();
        for (auto& th : ths) th.join();
    }
    proven = S.optFlag.load() || S.doneA.load();
    // A stop request means the search was cut off; only a legitimately completed search or a
    // bound-certified incumbent counts as proof.
    if (!S.optFlag.load() && !S.doneA.load()) proven = false;
    LOG("search finished: proven=%d nodes=%lld best=%lld", (int)proven, (long long)S.totalNodes.load(), (long long)inc.cost.load());
    return finishFromInc(proven);
}
