#include "instance.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>

#include "common.hpp"

namespace sop {

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string upper(std::string s) {
    for (auto& ch : s) ch = (char)std::toupper((unsigned char)ch);
    return s;
}

}  // namespace

RawInstance parseSopFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw FatalError("cannot open input file: " + path);
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    RawInstance raw;
    long long dim = -1;
    std::string type, fmt;
    size_t pos = 0;
    bool sectionFound = false;
    while (pos < data.size()) {
        size_t eol = data.find('\n', pos);
        if (eol == std::string::npos) eol = data.size();
        std::string line = trim(data.substr(pos, eol - pos));
        pos = (eol < data.size()) ? eol + 1 : data.size();
        if (line.empty()) continue;
        std::string up = upper(line);
        if (up.compare(0, 19, "EDGE_WEIGHT_SECTION") == 0) {
            sectionFound = true;
            break;
        }
        size_t colon = line.find(':');
        std::string key, val;
        if (colon != std::string::npos) {
            key = upper(trim(line.substr(0, colon)));
            val = trim(line.substr(colon + 1));
        } else {
            key = up;
        }
        if (key == "NAME") raw.name = val;
        else if (key == "DIMENSION") dim = std::atoll(val.c_str());
        else if (key == "TYPE") type = upper(val);
        else if (key == "EDGE_WEIGHT_FORMAT") fmt = upper(val);
        // COMMENT, EDGE_WEIGHT_TYPE and unknown keys are metadata only.
    }
    if (!sectionFound) throw FatalError("EDGE_WEIGHT_SECTION not found in " + path);
    if (!type.empty() && type != "SOP") logMsg("warning: TYPE is '%s', expected SOP", type.c_str());
    if (!fmt.empty() && fmt != "FULL_MATRIX")
        throw FatalError("unsupported EDGE_WEIGHT_FORMAT '" + fmt + "' (only FULL_MATRIX)");

    // Tokenise every integer up to the first non-numeric token (e.g. EOF).
    std::vector<int64_t> vals;
    if (dim > 0) vals.reserve((size_t)dim * (size_t)dim + 1);
    const char* p = data.c_str() + pos;
    const char* endp = data.c_str() + data.size();
    while (p < endp) {
        while (p < endp && std::isspace((unsigned char)*p)) ++p;
        if (p >= endp) break;
        if (!(std::isdigit((unsigned char)*p) || *p == '-' || *p == '+')) break;
        char* q = nullptr;
        long long v = std::strtoll(p, &q, 10);
        if (q == p) break;
        vals.push_back(v);
        p = q;
    }
    if (dim <= 0) {
        if (vals.empty()) throw FatalError("DIMENSION missing and no matrix data");
        dim = vals[0];
    }
    if (dim <= 0 || dim > 100000) throw FatalError("invalid DIMENSION");
    const size_t N = (size_t)dim;
    size_t offset = 0;
    if (vals.size() >= N * N + 1 && vals[0] == dim) offset = 1;  // repeated dimension line
    if (vals.size() < offset + N * N)
        throw FatalError("matrix truncated: expected " + std::to_string(N * N) + " values, found " +
                         std::to_string(vals.size() - offset));
    if (vals.size() > offset + N * N)
        logMsg("warning: %zu extra numeric tokens after the matrix ignored", vals.size() - offset - N * N);
    raw.n = (int)dim;
    raw.T.assign(vals.begin() + offset, vals.begin() + offset + N * N);
    for (int64_t v : raw.T)
        if (v > (int64_t)INT32_MAX - 1) throw FatalError("matrix value too large (> 2^31-2)");
    return raw;
}

namespace {

// Minimal dynamic bitset rows used for the closure computations.
struct BitMat {
    int n = 0, w = 0;
    std::vector<uint64_t> d;
    explicit BitMat(int n_) : n(n_), w((n_ + 63) / 64), d((size_t)n_ * ((n_ + 63) / 64), 0) {}
    uint64_t* row(int i) { return d.data() + (size_t)i * w; }
    const uint64_t* row(int i) const { return d.data() + (size_t)i * w; }
    bool get(int i, int j) const { return (row(i)[j >> 6] >> (j & 63)) & 1ULL; }
    void set(int i, int j) { row(i)[j >> 6] |= 1ULL << (j & 63); }
};

bool intersects(const uint64_t* a, const uint64_t* b, int w) {
    for (int k = 0; k < w; ++k)
        if (a[k] & b[k]) return true;
    return false;
}

Csr makeCsr(int n, const std::vector<std::pair<int, int>>& edges) {
    Csr g;
    g.off.assign(n + 1, 0);
    for (auto& e : edges) g.off[e.first + 1]++;
    for (int i = 0; i < n; ++i) g.off[i + 1] += g.off[i];
    g.adj.resize(edges.size());
    std::vector<int32_t> fill(g.off.begin(), g.off.end() - 1);
    for (auto& e : edges) g.adj[fill[e.first]++] = e.second;
    return g;
}

}  // namespace

Problem buildProblem(const RawInstance& raw) {
    Problem P;
    const int N = raw.n;
    P.nReal = N;

    // ---- 1. direct precedences and their transitive closure (on real nodes) --
    // pred[i] has bit j  <=>  j must precede i.
    BitMat pred(N);
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j)
            if (i != j && raw.at(i, j) < 0) pred.set(i, j);
    for (int k = 0; k < N; ++k)
        for (int i = 0; i < N; ++i)
            if (pred.get(i, k)) {
                uint64_t* ri = pred.row(i);
                const uint64_t* rk = pred.row(k);
                for (int w = 0; w < pred.w; ++w) ri[w] |= rk[w];
            }
    for (int i = 0; i < N; ++i)
        if (pred.get(i, i)) {
            P.feasible = false;
            P.n = N;
            return P;  // cyclic precedence: no feasible Hamiltonian path
        }

    // ---- 2. sources / sinks (nodes without predecessors / successors) --------
    std::vector<int> sources, sinks;
    std::vector<char> hasSucc(N, 0);
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j)
            if (pred.get(i, j)) hasSucc[j] = 1;
    for (int i = 0; i < N; ++i) {
        bool hasPred = false;
        for (int w = 0; w < pred.w && !hasPred; ++w) hasPred = pred.row(i)[w] != 0;
        if (!hasPred) sources.push_back(i);
        if (!hasSucc[i]) sinks.push_back(i);
    }
    P.addedSource = sources.size() > 1;
    P.addedSink = sinks.size() > 1;
    int n = N;
    int srcNode = -1, snkNode = -1;
    if (P.addedSource) srcNode = n++;
    if (P.addedSink) snkNode = n++;
    P.n = n;
    P.start = P.addedSource ? srcNode : sources[0];
    P.end = P.addedSink ? snkNode : sinks[0];

    // ---- 3. final closure matrix (bytes) including dummy nodes ---------------
    P.prec.assign((size_t)n * n, 0);
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j)
            if (pred.get(i, j)) P.prec[(size_t)j * n + i] = 1;  // j precedes i
    if (P.addedSource)
        for (int x = 0; x < n; ++x)
            if (x != srcNode) P.prec[(size_t)srcNode * n + x] = 1;
    if (P.addedSink)
        for (int x = 0; x < n; ++x)
            if (x != snkNode) P.prec[(size_t)x * n + snkNode] = 1;
    // If both dummies exist the pair (src, snk) is already ordered by the rules above.

    // ---- 4. bit matrices of the final closure -------------------------------
    BitMat succB(n), predB(n);
    for (int a = 0; a < n; ++a)
        for (int b = 0; b < n; ++b)
            if (P.prec[(size_t)a * n + b]) {
                succB.set(a, b);
                predB.set(b, a);
            }

    // ---- 5. arc costs with static feasibility filtering ----------------------
    P.cost.assign((size_t)n * n, kForbidden);
    std::vector<std::pair<int, int>> hasseEdges;
    for (int a = 0; a < n; ++a) {
        for (int b = 0; b < n; ++b) {
            if (a == b) continue;
            if (P.prec[(size_t)b * n + a]) continue;  // b must precede a: arc a->b impossible
            bool covering = true;
            if (P.prec[(size_t)a * n + b]) {
                // a must precede b: direct arc only if nothing is forced in between.
                covering = !intersects(succB.row(a), predB.row(b), succB.w);
                if (covering) hasseEdges.emplace_back(a, b);
                else continue;
            }
            int64_t c;
            if (a < N && b < N) {
                c = raw.at(a, b);
                if (c < 0) c = 0;  // cannot happen for allowed arcs, but stay safe
            } else {
                c = 0;  // arcs touching dummy nodes are free
            }
            P.cost[(size_t)a * n + b] = (int32_t)c;
            P.maxArc = std::max<int64_t>(P.maxArc, c);
        }
    }
    P.hasseSucc = makeCsr(n, hasseEdges);
    std::vector<std::pair<int, int>> rev;
    rev.reserve(hasseEdges.size());
    for (auto& e : hasseEdges) rev.emplace_back(e.second, e.first);
    P.hassePred = makeCsr(n, rev);
    P.predCount.assign(n, 0);
    for (int v = 0; v < n; ++v) P.predCount[v] = P.hassePred.degree(v);
    return P;
}

std::vector<int> toRealPath(const Problem& p, const std::vector<int>& internalPath) {
    std::vector<int> out;
    out.reserve(internalPath.size());
    for (int v : internalPath)
        if (v < p.nReal) out.push_back(v);
    return out;
}

}  // namespace sop
