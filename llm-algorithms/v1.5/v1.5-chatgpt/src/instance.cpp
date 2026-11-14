// Robust TSPLIB matrix parsing, independent path checking, and precedence preprocessing.
#include "sop/instance.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <queue>
#include <map>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace sop {
namespace {
std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
Cost integer(const std::string& text, const std::string& where) {
    Cost v = 0;
    const char* a = text.data();
    const char* b = a + text.size();
    if (a != b && *a == '+') {
        ++a;
        if (a == b || *a == '-' || *a == '+') throw std::runtime_error("invalid integer " + where);
    }
    auto r = std::from_chars(a, b, v);
    if (a == b || r.ec != std::errc{} || r.ptr != b)
        throw std::runtime_error("invalid integer " + where + ": " + text.substr(0, 50));
    return v;
}
} // namespace

Instance Instance::read(const std::string& filename) {
    std::ifstream input(filename);
    if (!input) throw std::runtime_error("cannot open input: " + filename);
    Instance instance;
    std::unordered_map<std::string, std::string> metadata;
    std::string line;
    bool section = false, first_line = true;
    while (std::getline(input, line)) {
        if (first_line && line.compare(0, 3, "\xef\xbb\xbf") == 0) line.erase(0, 3);
        first_line = false;
        line = trim(line);
        if (line.empty()) continue;
        const auto split = line.find_first_of(": \t");
        const auto key = upper(line.substr(0, split));
        std::string value = split == std::string::npos ? "" : trim(line.substr(split));
        if (!value.empty() && value.front() == ':') value = trim(value.substr(1));
        if (key == "EDGE_WEIGHT_SECTION") {
            if (!value.empty()) throw std::runtime_error("EDGE_WEIGHT_SECTION must be on its own line");
            section = true; break;
        }
        if (key == "COMMENT") continue; // Free text, including numbers and colons.
        if (key == "NAME" || key == "TYPE" || key == "DIMENSION" ||
            key == "EDGE_WEIGHT_TYPE" || key == "EDGE_WEIGHT_FORMAT") {
            auto [it, inserted] = metadata.emplace(key, value);
            if (!inserted && it->second != value) throw std::runtime_error("conflicting header: " + key);
        }
        // Unknown header keywords are metadata, never matrix values.
    }
    if (!section) throw std::runtime_error("missing EDGE_WEIGHT_SECTION");
    if (!metadata.count("DIMENSION")) throw std::runtime_error("missing DIMENSION");
    Cost dimension = integer(metadata.at("DIMENSION"), "in DIMENSION");
    if (dimension < 1 || dimension > 1000) throw std::runtime_error("DIMENSION must be in 1..1000");
    instance.n = static_cast<int>(dimension);
    instance.name = metadata.count("NAME") ? metadata.at("NAME") : filename;
    for (const auto& spec : {std::pair<const char*, const char*>{"TYPE", "SOP"},
                            {"EDGE_WEIGHT_TYPE", "EXPLICIT"}, {"EDGE_WEIGHT_FORMAT", "FULL_MATRIX"}}) {
        auto it = metadata.find(spec.first);
        if (it != metadata.end() && upper(it->second) != spec.second)
            throw std::runtime_error(std::string("unsupported ") + spec.first + ": " + it->second);
    }
    std::string token;
    if (!(input >> token) || integer(token, "after EDGE_WEIGHT_SECTION") != dimension)
        throw std::runtime_error("missing or mismatched repeated dimension");
    const auto count = static_cast<std::size_t>(instance.n) * instance.n;
    instance.matrix.resize(count);
    // Along a root-to-leaf path, repeated warm-start dual adjustments can total
    // O(N^2 * max_edge), even though the objective is only O(N * max_edge).
    const Cost max_edge = INF / (16 * (dimension + 1) * (dimension + 1));
    for (std::size_t k = 0; k < count; ++k) {
        if (!(input >> token)) throw std::runtime_error("truncated matrix at entry " + std::to_string(k));
        Cost value = integer(token, "at matrix entry " + std::to_string(k));
        if (k / instance.n == k % instance.n && value != 0)
            throw std::runtime_error("matrix diagonal must be zero");
        if (value > max_edge) throw std::runtime_error("cost exceeds safe int64 limit " + std::to_string(max_edge));
        instance.matrix[k] = value;
    }
    if (input >> token) {
        if (upper(token) != "EOF") throw std::runtime_error("extra token after full matrix: " + token.substr(0, 50));
        if (input >> token) throw std::runtime_error("unexpected content after EOF");
    }
    if (input.bad()) throw std::runtime_error("I/O error while reading input");
    return instance;
}

Cost Instance::validate(const std::vector<int>& path) const {
    if (path.size() != static_cast<std::size_t>(n)) throw std::runtime_error("invalid path length");
    std::vector<int> position(n, -1);
    for (int k = 0; k < n; ++k) {
        int v = path[k];
        if (v < 0 || v >= n || position[v] != -1) throw std::runtime_error("invalid path permutation");
        position[v] = k;
    }
    for (int i = 0; i < n; ++i) for (int j = 0; j < n; ++j)
        if (i != j && at(i, j) < 0 && position[j] >= position[i])
            throw std::runtime_error("path violates precedence " + std::to_string(j) + " before " + std::to_string(i));
    Cost total = 0;
    for (int k = 1; k < n; ++k) {
        Cost edge = at(path[k - 1], path[k]);
        if (edge < 0) throw std::runtime_error("path uses a precedence marker as an edge");
        total += edge;
    }
    return total;
}

bool Graph::available(int v, const std::vector<std::uint64_t>& visited) const {
    for (int w = 0; w < words; ++w)
        if (pred[static_cast<std::size_t>(v) * words + w] & ~visited[w]) return false;
    return true;
}

Graph Graph::build(const Instance& in, Control& control,
                   const std::function<void(const std::vector<int>&)>& on_order) {
    Graph g;
    g.n = in.n; g.m = in.n + 1; g.words = (in.n + 63) / 64;
    g.predecessors.resize(g.n); g.successors.resize(g.n);
    std::vector<int> indegree(g.n, 0);
    for (int i = 0; i < g.n; ++i) {
        if (control.stopped()) return g;
        for (int j = 0; j < g.n; ++j) if (i != j && in.at(i, j) < 0) {
            g.predecessors[i].push_back(j); g.successors[j].push_back(i); ++indegree[i];
        }
    }
    // Cost/precedence twins are interchangeable labels. A canonical order within
    // each exact twin class removes symmetric paths while retaining an optimum.
    auto normalized = [](Cost x) { return x < 0 ? Cost{-1} : x; };
    auto twins = [&](int a, int b) {
        if (in.at(a, b) < 0 || in.at(a, b) != in.at(b, a)) return false;
        for (int k = 0; k < g.n; ++k) if (k != a && k != b)
            if (normalized(in.at(a, k)) != normalized(in.at(b, k)) ||
                normalized(in.at(k, a)) != normalized(in.at(k, b))) return false;
        return true;
    };
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::vector<std::vector<int>>> classes;
    for (int i = 0; i < g.n; ++i) {
        if (control.stopped()) return g;
        std::uint64_t row_hash = 0, col_hash = 0;
        for (int k = 0; k < g.n; ++k) {
            row_hash += mix64(static_cast<std::uint64_t>(normalized(in.at(i, k))));
            col_hash += mix64(static_cast<std::uint64_t>(normalized(in.at(k, i))));
        }
        auto& bucket = classes[{row_hash, col_hash}];
        bool found = false;
        for (auto& group : bucket) if (twins(group.front(), i)) {
            int previous = group.back();
            g.successors[previous].push_back(i); g.predecessors[i].push_back(previous); ++indegree[i];
            ++g.symmetry_edges; group.push_back(i); found = true; break;
        }
        if (!found) bucket.push_back({i});
    }
    // Kahn topological sorting detects cycles, extrema, and a unique linear order.
    std::priority_queue<int, std::vector<int>, std::greater<int>> ready;
    for (int i = 0; i < g.n; ++i) {
        if (!indegree[i]) { ready.push(i); g.sources.push_back(i); }
        if (g.successors[i].empty()) g.sinks.push_back(i);
    }
    g.unique_order = true;
    while (!ready.empty()) {
        if (ready.size() > 1) g.unique_order = false;
        int v = ready.top(); ready.pop(); g.topological.push_back(v);
        for (int w : g.successors[v]) if (--indegree[w] == 0) ready.push(w);
    }
    if (static_cast<int>(g.topological.size()) != g.n) { g.cyclic = true; g.complete = true; return g; }
    // Publish the first feasible order before closure/reductions, so a later
    // preprocessing timeout retains a solution already found within the budget.
    if (on_order) on_order(g.topological);
    // DAG transitive closure: predecessor bitsets are propagated in topological order.
    g.pred.assign(static_cast<std::size_t>(g.n) * g.words, 0);
    g.succ.assign(g.pred.size(), 0);
    for (int v : g.topological) {
        if (control.stopped()) return g;
        auto* pv = &g.pred[static_cast<std::size_t>(v) * g.words];
        for (int p : g.predecessors[v]) {
            const auto* pp = &g.pred[static_cast<std::size_t>(p) * g.words];
            for (int w = 0; w < g.words; ++w) pv[w] |= pp[w];
            pv[p / 64] |= std::uint64_t{1} << (p % 64);
        }
    }
    for (int j = 0; j < g.n; ++j) for (int i = 0; i < g.n; ++i)
        if (g.before(i, j)) g.succ[static_cast<std::size_t>(i) * g.words + j / 64] |= std::uint64_t{1} << (j % 64);
    g.costs.assign(static_cast<std::size_t>(g.m) * g.m, INF);
    for (int i = 0; i < g.n; ++i) {
        if (control.stopped()) return g;
        for (int j = 0; j < g.n; ++j) {
            if (i == j || g.before(j, i)) continue;
            bool intervening = false;
            // If i < k < j, i and j cannot be consecutive in any feasible path.
            for (int w = 0; w < g.words; ++w)
                if (g.succ[static_cast<std::size_t>(i) * g.words + w] &
                    g.pred[static_cast<std::size_t>(j) * g.words + w]) { intervening = true; break; }
            if (!intervening) g.costs[static_cast<std::size_t>(i) * g.m + j] = in.at(i, j);
        }
    }
    // A zero-cost dummy closes paths into cycles without choosing fixed endpoints.
    for (int s : g.sources) g.costs[static_cast<std::size_t>(g.n) * g.m + s] = 0;
    for (int t : g.sinks) g.costs[static_cast<std::size_t>(t) * g.m + g.n] = 0;
    g.zobrist.resize(g.n);
    for (int i = 0; i < g.n; ++i) g.zobrist[i] = mix64(static_cast<std::uint64_t>(i) + 0x534f50ULL);
    g.complete = true;
    return g;
}
} // namespace sop
