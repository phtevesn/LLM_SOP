#include "sop_instance.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <random>
#include <utility>

namespace sop {

std::string Instance::extract_basename(const std::string& p) {
    auto pos = p.find_last_of("/\\");
    return (pos == std::string::npos) ? p : p.substr(pos + 1);
}

bool Instance::parse(const std::string& path) {
    filepath = path;
    filename = extract_basename(path);
    std::ifstream in(path);
    if (!in.is_open()) {
        std::cerr << "[Error] Cannot open file: " << path << "\n";
        return false;
    }

    n = -1;
    std::string token;
    bool found_section = false;
    while (in >> token) {
        std::string key = token;
        if (!key.empty() && key.back() == ':') {
            key.pop_back();
        }
        if (key == "NAME") {
            if (token.back() != ':') {
                std::string peek;
                if (in >> peek) {
                    if (peek == ":") in >> header_name;
                    else header_name = peek;
                }
            } else {
                in >> header_name;
            }
        } else if (key == "DIMENSION") {
            std::string val_str;
            if (token.back() != ':') {
                in >> val_str;
                if (val_str == ":") in >> val_str;
            } else {
                in >> val_str;
            }
            try {
                n = std::stoi(val_str);
            } catch (...) {
                return false;
            }
        } else if (key == "EDGE_WEIGHT_SECTION") {
            found_section = true;
            break;
        }
    }

    if (!found_section) {
        std::cerr << "[Error] Missing EDGE_WEIGHT_SECTION in " << path << "\n";
        return false;
    }

    std::vector<int64_t> values;
    if (n > 0) values.reserve(static_cast<size_t>(n) * n + 2);
    std::string val_tok;
    while (in >> val_tok) {
        if (val_tok == "EOF" || val_tok == "DISPLAY_DATA_SECTION") break;
        try {
            size_t idx = 0;
            int64_t v = std::stoll(val_tok, &idx, 10);
            if (idx == val_tok.size()) {
                values.push_back(v);
                if (n > 0 && values.size() == static_cast<size_t>(n) * n + 1) {
                    if (values[0] == n) break;
                }
            }
        } catch (...) {
            break;
        }
    }

    if (n <= 0 && !values.empty()) {
        int64_t cand_n = values[0];
        if (cand_n > 0 && cand_n <= MAX_NODES &&
            values.size() >= static_cast<size_t>(cand_n) * cand_n + 1) {
            n = static_cast<int>(cand_n);
        }
    }
    if (n <= 0 || n > MAX_NODES) {
        std::cerr << "[Error] Invalid or unsupported DIMENSION: " << n << "\n";
        return false;
    }

    size_t expected = static_cast<size_t>(n) * n;
    if (values.size() == expected + 1 && values[0] == n) {
        raw_mat.assign(values.begin() + 1, values.end());
    } else if (values.size() == expected) {
        raw_mat = std::move(values);
    } else if (values.size() > expected + 1 && values[0] == n) {
        raw_mat.assign(values.begin() + 1, values.begin() + 1 + expected);
    } else {
        std::cerr << "[Error] Expected " << expected << " matrix entries for N=" << n
                  << ", got " << values.size() << "\n";
        return false;
    }

    preprocess();
    return true;
}

void Instance::preprocess() {
    nwords = (n + 63) / 64;
    preds.assign(n, Bitset1024());
    succs.assign(n, Bitset1024());

    // T[i][j] < 0 means node i depends on node j -> j must precede i.
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            if (i != j && raw_mat[i * n + j] < 0) {
                preds[i].set(j);
                succs[j].set(i);
            }
        }
    }

    // Bitset Floyd-Warshall for transitive closure of precedences O(N^3 / 64)
    for (int k = 0; k < n; ++k) {
        for (int i = 0; i < n; ++i) {
            if (preds[i].test(k)) {
                preds[i].bit_or(preds[k], nwords);
            }
            if (succs[i].test(k)) {
                succs[i].bit_or(succs[k], nwords);
            }
        }
    }

    // Cycle check in precedence graph
    has_cycle = false;
    for (int i = 0; i < n; ++i) {
        if (preds[i].test(i)) {
            has_cycle = true;
            return;
        }
    }

    pred_count.assign(n, 0);
    succ_count.assign(n, 0);
    start_candidates.clear();
    end_candidates.clear();
    start_mask.clear();
    end_mask.clear();

    for (int i = 0; i < n; ++i) {
        pred_count[i] = preds[i].count(nwords);
        succ_count[i] = succs[i].count(nwords);
        if (pred_count[i] == 0) {
            start_candidates.push_back(i);
            start_mask.set(i);
        }
        if (succ_count[i] == 0) {
            end_candidates.push_back(i);
            end_mask.set(i);
        }
    }

    if (start_candidates.empty() || end_candidates.empty()) {
        has_cycle = true;
        return;
    }

    cost.assign(static_cast<size_t>(n) * n, INF);
    adj_out.assign(n, {});
    adj_in.assign(n, {});

    for (int u = 0; u < n; ++u) {
        for (int v = 0; v < n; ++v) {
            if (u == v) continue;
            if (raw_mat[u * n + v] < 0) continue;
            if (preds[u].test(v)) continue;
            if (succs[u].intersects(preds[v], nwords)) continue;
            if (end_candidates.size() == 1 && u == end_candidates[0]) continue;
            if (start_candidates.size() == 1 && v == start_candidates[0]) continue;
            if (n > 2 && start_candidates.size() == 1 && end_candidates.size() == 1 &&
                u == start_candidates[0] && v == end_candidates[0]) {
                continue;
            }
            cost[u * n + v] = raw_mat[u * n + v];
            adj_out[u].push_back(v);
            adj_in[v].push_back(u);
        }
    }

    for (int i = 0; i < n; ++i) {
        std::sort(adj_out[i].begin(), adj_out[i].end(), [&](int a, int b) {
            return cost[i * n + a] < cost[i * n + b];
        });
        std::sort(adj_in[i].begin(), adj_in[i].end(), [&](int a, int b) {
            return cost[a * n + i] < cost[b * n + i];
        });
    }

    min_in1.assign(n, 0);
    min_in1_src.assign(n, -1);
    min_out1.assign(n, 0);
    min_out1_dst.assign(n, -1);
    for (int i = 0; i < n; ++i) {
        if (!adj_in[i].empty()) {
            min_in1_src[i] = adj_in[i][0];
            min_in1[i] = cost[adj_in[i][0] * n + i];
        }
        if (!adj_out[i].empty()) {
            min_out1_dst[i] = adj_out[i][0];
            min_out1[i] = cost[i * n + adj_out[i][0]];
        }
    }

    std::mt19937_64 rng(0x5050534F50534F50ULL);
    zobrist.resize(n);
    for (int i = 0; i < n; ++i) zobrist[i] = rng();

    if (n >= 2) {
        compute_ap_relaxation();
        compute_hk_relaxation();
    }
}

void Instance::compute_ap_relaxation() {
    std::vector<int64_t> c = cost;
    for (int e : end_candidates) {
        for (int s : start_candidates) {
            if (e != s) c[e * n + s] = 0;
        }
    }
    std::vector<int64_t> u(n + 1, 0), v(n + 1, 0);
    std::vector<int> p(n + 1, 0), way(n + 1, 0);
    std::vector<int64_t> minv(n + 1);
    std::vector<uint8_t> used(n + 1);

    for (int i = 1; i <= n; ++i) {
        p[0] = i;
        int j0 = 0;
        std::fill(minv.begin(), minv.end(), INF * 2);
        std::fill(used.begin(), used.end(), 0);
        do {
            used[j0] = 1;
            int i0 = p[j0], j1 = 0;
            int64_t delta = INF * 2;
            const int64_t* row = &c[(i0 - 1) * n];
            int64_t ui0 = u[i0];
            for (int j = 1; j <= n; ++j) {
                if (!used[j]) {
                    int64_t cur = row[j - 1] - ui0 - v[j];
                    if (cur < minv[j]) {
                        minv[j] = cur;
                        way[j] = j0;
                    }
                    if (minv[j] < delta) {
                        delta = minv[j];
                        j1 = j;
                    }
                }
            }
            for (int j = 0; j <= n; ++j) {
                if (used[j]) {
                    u[p[j]] += delta;
                    v[j] -= delta;
                } else {
                    minv[j] -= delta;
                }
            }
            j0 = j1;
        } while (p[j0] != 0);
        do {
            int j1 = way[j0];
            p[j0] = p[j1];
            j0 = j1;
        } while (j0);
    }

    root_ap_lb = -v[0];
    u_pot.resize(n);
    v_pot.resize(n);
    for (int i = 0; i < n; ++i) {
        u_pot[i] = u[i + 1];
        v_pot[i] = v[i + 1];
    }
    rcost.assign(static_cast<size_t>(n) * n, INF);
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            if (cost[i * n + j] < INF) {
                rcost[i * n + j] = std::max<int64_t>(0, cost[i * n + j] - u_pot[i] - v_pot[j]);
            }
        }
    }
}

void Instance::compute_hk_relaxation() {
    std::vector<double> po(n, 0.0), pi(n, 0.0);
    std::vector<double> best_po(n, 0.0), best_pi(n, 0.0);
    double best_lb = -1e18;
    double step = 2.0;

    std::vector<double> min_e(n);
    std::vector<int> parent(n);
    std::vector<uint8_t> in_tree(n);
    int iters = (n <= 100) ? 500 : 200;

    for (int it = 0; it < iters; ++it) {
        std::fill(in_tree.begin(), in_tree.end(), 0);
        in_tree[0] = 1;
        for (int v = 0; v < n; ++v) {
            double c0v = (cost[0 * n + v] < INF) ? (cost[0 * n + v] - po[0] - pi[v]) : 1e15;
            double cv0 = (cost[v * n + 0] < INF) ? (cost[v * n + 0] - po[v] - pi[0]) : 1e15;
            min_e[v] = std::min(c0v, cv0);
            parent[v] = 0;
        }
        double mst_val = 0.0;
        std::vector<std::pair<int, int>> edges;
        edges.reserve(n - 1);
        bool disconnected = false;

        for (int k = 0; k < n - 1; ++k) {
            int best_v = -1;
            double best_d = 1e18;
            for (int v = 0; v < n; ++v) {
                if (!in_tree[v] && min_e[v] < best_d) {
                    best_d = min_e[v];
                    best_v = v;
                }
            }
            if (best_v == -1 || best_d >= 1e14) {
                disconnected = true;
                break;
            }
            in_tree[best_v] = 1;
            mst_val += best_d;
            edges.push_back({parent[best_v], best_v});
            for (int w = 0; w < n; ++w) {
                if (!in_tree[w]) {
                    double cbw = (cost[best_v * n + w] < INF) ? (cost[best_v * n + w] - po[best_v] - pi[w]) : 1e15;
                    double cwb = (cost[w * n + best_v] < INF) ? (cost[w * n + best_v] - po[w] - pi[best_v]) : 1e15;
                    double d = std::min(cbw, cwb);
                    if (d < min_e[w]) {
                        min_e[w] = d;
                        parent[w] = best_v;
                    }
                }
            }
        }
        if (disconnected) break;

        double sum_p = 0.0;
        if (end_candidates.size() == 1 && start_candidates.size() == 1) {
            for (int i = 0; i < n; ++i) {
                if (i != end_candidates[0]) sum_p += po[i];
                if (i != start_candidates[0]) sum_p += pi[i];
            }
        } else {
            for (int i = 0; i < n; ++i) {
                if (!end_mask.test(i)) sum_p += po[i];
                if (!start_mask.test(i)) sum_p += pi[i];
            }
        }
        double lb = mst_val + sum_p;
        if (lb > best_lb) {
            best_lb = lb;
            best_po = po;
            best_pi = pi;
        }

        std::vector<int> odeg(n, 0), ideg(n, 0);
        for (auto [u, v] : edges) {
            double cuv = (cost[u * n + v] < INF) ? (cost[u * n + v] - po[u] - pi[v]) : 1e15;
            double cvu = (cost[v * n + u] < INF) ? (cost[v * n + u] - po[v] - pi[u]) : 1e15;
            if (cuv <= cvu) {
                odeg[u]++;
                ideg[v]++;
            } else {
                odeg[v]++;
                ideg[u]++;
            }
        }
        double norm = 0.0;
        for (int i = 0; i < n; ++i) {
            double target_o = end_mask.test(i) ? 0.0 : 1.0;
            double target_i = start_mask.test(i) ? 0.0 : 1.0;
            double go = target_o - odeg[i];
            double gi = target_i - ideg[i];
            norm += go * go + gi * gi;
            po[i] += step * go;
            pi[i] += step * gi;
            if (end_candidates.size() > 1 && end_mask.test(i) && po[i] > 0.0) po[i] = 0.0;
            if (start_candidates.size() > 1 && start_mask.test(i) && pi[i] > 0.0) pi[i] = 0.0;
        }
        if (norm == 0.0) break;
        if (it % 50 == 49) step *= 0.65;
    }

    root_hk_lb = (best_lb > -1e17) ? static_cast<int64_t>(std::ceil(best_lb - 1e-6)) : 0;
    pi_out.resize(n);
    pi_in.resize(n);
    for (int i = 0; i < n; ++i) {
        pi_out[i] = std::llround(best_po[i] * HK_SCALE);
        pi_in[i] = std::llround(best_pi[i] * HK_SCALE);
    }
    sym_hk.assign(static_cast<size_t>(n) * n, INF * HK_SCALE);
    for (int u = 0; u < n; ++u) {
        for (int v = 0; v < n; ++v) {
            int64_t cuv = (cost[u * n + v] < INF) ? (cost[u * n + v] * HK_SCALE - pi_out[u] - pi_in[v]) : (INF * HK_SCALE);
            int64_t cvu = (cost[v * n + u] < INF) ? (cost[v * n + u] * HK_SCALE - pi_out[v] - pi_in[u]) : (INF * HK_SCALE);
            sym_hk[u * n + v] = std::min(cuv, cvu);
        }
    }
}

} // namespace sop