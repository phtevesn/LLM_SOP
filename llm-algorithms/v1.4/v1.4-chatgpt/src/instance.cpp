#include "sop/instance.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <queue>
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
Cost integer(const std::string& token, const std::string& where) {
    Cost value = 0;
    const char* begin = token.data();
    if (!token.empty() && token.front() == '+') ++begin;
    const auto parsed = std::from_chars(begin, token.data() + token.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size())
        throw std::runtime_error("Invalid integer " + where + ": " + token);
    return value;
}
} // namespace

Instance read_instance(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("Cannot open input: " + path);
    Instance p;
    std::string line;
    bool section = false;
    std::unordered_map<std::string, std::string> headers;
    while (std::getline(in, line)) {
        if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xef &&
            static_cast<unsigned char>(line[1]) == 0xbb && static_cast<unsigned char>(line[2]) == 0xbf)
            line.erase(0, 3);
        line = trim(line);
        if (line.empty()) continue;
        const auto split = line.find_first_of(": \t");
        const std::string key = line.substr(0, split);
        std::string value = split == std::string::npos ? "" : trim(line.substr(split));
        if (!value.empty() && value.front() == ':') value = trim(value.substr(1));
        if (key == "EDGE_WEIGHT_SECTION") {
            if (!value.empty()) throw std::runtime_error("EDGE_WEIGHT_SECTION must be on its own line");
            section = true;
            break;
        }
        if (key == "COMMENT") continue;
        if (key != "NAME" && key != "TYPE" && key != "DIMENSION" &&
            key != "EDGE_WEIGHT_TYPE" && key != "EDGE_WEIGHT_FORMAT") continue;
        if (headers.count(key) && headers[key] != value)
            throw std::runtime_error("Conflicting header: " + key);
        headers[key] = value;
    }
    if (!section || !headers.count("DIMENSION"))
        throw std::runtime_error("Missing DIMENSION or EDGE_WEIGHT_SECTION");
    const Cost dimension = integer(headers.at("DIMENSION"), "in DIMENSION");
    if (dimension < 1 || dimension > MAX_N) throw std::runtime_error("DIMENSION must be 1..1000");
    p.n = static_cast<int>(dimension);
    p.words = (p.n + 63) / 64;
    p.name = headers["NAME"];
    for (const auto& spec : {std::make_pair("TYPE", "SOP"),
                              std::make_pair("EDGE_WEIGHT_TYPE", "EXPLICIT"),
                              std::make_pair("EDGE_WEIGHT_FORMAT", "FULL_MATRIX")}) {
        if (headers.count(spec.first) && headers.at(spec.first) != spec.second)
            throw std::runtime_error(std::string("Unsupported ") + spec.first + ": " + headers.at(spec.first));
    }
    std::string token;
    if (!(in >> token) || integer(token, "after EDGE_WEIGHT_SECTION") != dimension)
        throw std::runtime_error("Repeated matrix dimension is missing or differs from DIMENSION");
    p.original.resize(std::size_t(p.n) * p.n);
    // Leave a generous arithmetic margin for primal costs, dual potentials,
    // differences and additions. Larger integers are rejected, never clamped.
    const Cost max_edge = INF / (16 * (p.n + 1));
    for (int i = 0; i < p.n; ++i) for (int j = 0; j < p.n; ++j) {
        if (!(in >> token)) throw std::runtime_error("Truncated matrix at row " + std::to_string(i) + ", column " + std::to_string(j));
        const Cost v = integer(token, "at (" + std::to_string(i) + "," + std::to_string(j) + ")");
        if (i == j && v != 0) throw std::runtime_error("Matrix diagonal must be zero");
        if (v > max_edge) throw std::runtime_error("Edge cost exceeds safe int64 range for this dimension");
        p.original[std::size_t(i) * p.n + j] = v;
    }
    if (in >> token) {
        if (token != "EOF") throw std::runtime_error("Unexpected data after the matrix: " + token);
        if (in >> token) throw std::runtime_error("Unexpected data after EOF");
    }
    if (in.bad()) throw std::runtime_error("I/O failure reading input");
    return p;
}

bool preprocess(Instance& p, const Deadline& deadline) {
    p.pred.assign(p.n, Bits{});
    p.succ.assign(p.n, Bits{});
    p.successors.resize(p.n);
    p.indegree.assign(p.n, 0);
    for (int i = 0; i < p.n; ++i) {
        if (deadline.expired()) return false;
        for (int j = 0; j < p.n; ++j) if (i != j && p.raw(i, j) < 0) {
            insert(p.pred[i], j);
            p.successors[j].push_back(i);
            ++p.indegree[i];
        }
    }
    std::queue<int> ready;
    auto degree = p.indegree;
    for (int i = 0; i < p.n; ++i) {
        if (!degree[i]) { ready.push(i); p.sources.push_back(i); }
        if (p.successors[i].empty()) p.sinks.push_back(i);
    }
    std::vector<int> order;
    while (!ready.empty()) {
        const int u = ready.front(); ready.pop();
        order.push_back(u);
        for (int v : p.successors[u]) if (--degree[v] == 0) ready.push(v);
    }
    if (static_cast<int>(order.size()) != p.n) { p.acyclic = false; return true; }
    // Propagate predecessor sets in topological order, then transpose them.
    for (int u : order) {
        if (deadline.expired()) return false;
        for (int v : p.successors[u])
            for (int w = 0; w < p.words; ++w) p.pred[v][w] |= p.pred[u][w];
    }
    for (int i = 0; i < p.n; ++i)
        for (int j = 0; j < p.n; ++j) if (contains(p.pred[i], j)) insert(p.succ[j], i);
    p.arcs.assign(std::size_t(p.n + 1) * (p.n + 1), INF);
    for (int i = 0; i < p.n; ++i) {
        if (deadline.expired()) return false;
        for (int j = 0; j < p.n; ++j) {
            if (i == j || contains(p.pred[i], j)) continue;
            bool intermediate = false;
            if (contains(p.pred[j], i)) {
                for (int w = 0; w < p.words; ++w)
                    if (p.succ[i][w] & p.pred[j][w]) { intermediate = true; break; }
            }
            if (!intermediate) p.arcs[std::size_t(i) * (p.n + 1) + j] = p.raw(i, j);
        }
    }
    for (int i : p.sources) p.arcs[std::size_t(p.n) * (p.n + 1) + i] = 0;
    for (int i : p.sinks) p.arcs[std::size_t(i) * (p.n + 1) + p.n] = 0;
    return !deadline.expired();
}

Cost validate_path(const Instance& p, const std::vector<int>& path) {
    if (static_cast<int>(path.size()) != p.n) throw std::runtime_error("Validation: wrong path length");
    std::vector<int> position(p.n, -1);
    for (int k = 0; k < p.n; ++k) {
        const int v = path[k];
        if (v < 0 || v >= p.n || position[v] != -1) throw std::runtime_error("Validation: path is not a permutation");
        position[v] = k;
    }
    for (int i = 0; i < p.n; ++i) for (int j = 0; j < p.n; ++j)
        if (i != j && p.raw(i, j) < 0 && position[j] >= position[i])
            throw std::runtime_error("Validation: violated precedence");
    Cost sum = 0;
    for (int k = 1; k < p.n; ++k) {
        const Cost c = p.raw(path[k - 1], path[k]);
        if (c < 0 || c >= INF || sum > INF - c) throw std::runtime_error("Validation: invalid edge or cost overflow");
        sum += c;
    }
    return sum;
}
} // namespace sop
