#include "heuristics.hpp"

#include <algorithm>
#include <mutex>
#include <utility>

namespace sop {

HeuristicEngine::HeuristicEngine(const Instance& inst, SharedSolverState& shared, uint64_t seed)
    : inst_(inst), shared_(shared), rng_(seed) {}

std::vector<int> HeuristicEngine::construct_randomized(double alpha) {
    int n = inst_.n;
    std::vector<int> path;
    path.reserve(n);
    Bitset1024 vis;

    for (int step = 0; step < n; ++step) {
        std::vector<int> ready;
        for (int v = 0; v < n; ++v) {
            if (!vis.test(v) && inst_.preds[v].is_subset_of(vis, inst_.nwords)) {
                if (step < n - 1 && inst_.end_candidates.size() == 1 && v == inst_.end_candidates[0]) {
                    continue;
                }
                ready.push_back(v);
            }
        }
        if (ready.empty()) return {};
        if (path.empty()) {
            int v = ready[std::uniform_int_distribution<int>(0, static_cast<int>(ready.size()) - 1)(rng_)];
            path.push_back(v);
            vis.set(v);
            continue;
        }
        int u = path.back();
        int64_t min_c = INF, max_c = 0;
        for (int v : ready) {
            int64_t c = inst_.raw_mat[u * n + v];
            if (c >= 0 && c < min_c) min_c = c;
            if (c >= 0 && c < INF / 2 && c > max_c) max_c = c;
        }
        int64_t thresh = min_c + static_cast<int64_t>(alpha * (max_c - min_c));
        std::vector<int> rcl;
        for (int v : ready) {
            int64_t c = inst_.raw_mat[u * n + v];
            if (c >= 0 && c <= thresh) rcl.push_back(v);
        }
        int chosen = rcl.empty()
                         ? ready[std::uniform_int_distribution<int>(0, static_cast<int>(ready.size()) - 1)(rng_)]
                         : rcl[std::uniform_int_distribution<int>(0, static_cast<int>(rcl.size()) - 1)(rng_)];
        path.push_back(chosen);
        vis.set(chosen);
    }
    return path;
}

void HeuristicEngine::local_search(std::vector<int>& path, int64_t& best_c, uint64_t& local_ops) {
    int n = inst_.n;
    if (n <= 2) return;
    std::vector<uint8_t> dlb3(n, 0);
    bool improved = true;

    while (improved && !shared_.is_timed_out()) {
        improved = false;
        local_ops++;

        // 1. Single-node forward & backward Or-Opt relocation O(N^2)
        for (int i = 1; i < n - 1; ++i) {
            int u = path[i];
            int prev = path[i - 1], nxt = path[i + 1];
            int64_t rem_gain = inst_.raw_mat[prev * n + u] + inst_.raw_mat[u * n + nxt] - inst_.raw_mat[prev * n + nxt];

            // Forward move
            for (int j = i + 1; j < n - 1; ++j) {
                int w = path[j];
                if (inst_.succs[u].test(w)) break;
                int w_next = path[j + 1];
                int64_t ins_c = inst_.raw_mat[w * n + u] + inst_.raw_mat[u * n + w_next] - inst_.raw_mat[w * n + w_next];
                if (ins_c < rem_gain) {
                    dlb3[prev] = dlb3[u] = dlb3[nxt] = dlb3[w] = dlb3[w_next] = 0;
                    std::rotate(path.begin() + i, path.begin() + i + 1, path.begin() + j + 1);
                    best_c += (ins_c - rem_gain);
                    improved = true;
                    goto next_pass;
                }
            }
            // Backward move
            for (int j = i - 1; j >= 1; --j) {
                int w = path[j];
                if (inst_.preds[u].test(w)) break;
                int w_prev = path[j - 1];
                int64_t ins_c = inst_.raw_mat[w_prev * n + u] + inst_.raw_mat[u * n + w] - inst_.raw_mat[w_prev * n + w];
                if (ins_c < rem_gain) {
                    dlb3[prev] = dlb3[u] = dlb3[nxt] = dlb3[w_prev] = dlb3[w] = 0;
                    std::rotate(path.begin() + j, path.begin() + i, path.begin() + i + 1);
                    best_c += (ins_c - rem_gain);
                    improved = true;
                    goto next_pass;
                }
            }
        }

        // 2. SOP-2-Opt (Precedence-constrained segment reversal) O(N^2)
        for (int i = 1; i < n - 2; ++i) {
            Bitset1024 seg;
            seg.set(path[i]);
            int64_t rev_diff = 0;
            for (int j = i + 1; j < n - 1; ++j) {
                int v = path[j];
                if (inst_.preds[v].intersects(seg, inst_.nwords)) break;
                seg.set(v);
                rev_diff += inst_.raw_mat[path[j] * n + path[j - 1]] - inst_.raw_mat[path[j - 1] * n + path[j]];
                int64_t bound_diff = inst_.raw_mat[path[i - 1] * n + path[j]] + inst_.raw_mat[path[i] * n + path[j + 1]]
                                   - inst_.raw_mat[path[i - 1] * n + path[i]] - inst_.raw_mat[path[j] * n + path[j + 1]];
                if (rev_diff + bound_diff < 0) {
                    dlb3[path[i - 1]] = dlb3[path[i]] = dlb3[path[j]] = dlb3[path[j + 1]] = 0;
                    std::reverse(path.begin() + i, path.begin() + j + 1);
                    best_c += rev_diff + bound_diff;
                    improved = true;
                    goto next_pass;
                }
            }
        }

        // 3. Don't-Look-Bits SOP-3-Exchange (Adjacent block swap of arbitrary lengths)
        for (int i = 1; i < n - 1; ++i) {
            int u_prev = path[i - 1], a_first = path[i];
            if (dlb3[u_prev]) continue;
            Bitset1024 succ_A;
            int64_t c_prev_a = inst_.raw_mat[u_prev * n + a_first];

            for (int j = i; j < n - 1; ++j) {
                int a_last = path[j];
                succ_A.bit_or(inst_.succs[a_last], inst_.nwords);
                int b_first = path[j + 1];
                if (succ_A.test(b_first)) continue;
                int64_t base_gain = c_prev_a + inst_.raw_mat[a_last * n + b_first] - inst_.raw_mat[u_prev * n + b_first];
                const int64_t* row_alast = &inst_.raw_mat[a_last * n];

                for (int k = j + 1; k < n - 1; ++k) {
                    int b_last = path[k];
                    if (k > j + 1 && succ_A.test(b_last)) break;
                    int u_next = path[k + 1];
                    int64_t k_cost = inst_.raw_mat[b_last * n + a_first] + row_alast[u_next] - inst_.raw_mat[b_last * n + u_next];
                    if (k_cost < base_gain) {
                        dlb3[u_prev] = dlb3[a_first] = dlb3[a_last] = dlb3[b_first] = dlb3[b_last] = dlb3[u_next] = 0;
                        std::rotate(path.begin() + i, path.begin() + j + 1, path.begin() + k + 1);
                        best_c += (k_cost - base_gain);
                        improved = true;
                        goto next_pass;
                    }
                }
            }
            dlb3[u_prev] = 1;
        }

        // 4. Short-Block Or-Opt for L = 2, 3, 4 (Forward & Backward)
        for (int L = 2; L <= 4; ++L) {
            for (int i = 1; i + L < n; ++i) {
                Bitset1024 b_succ, b_pred;
                for (int m = 0; m < L; ++m) {
                    b_succ.bit_or(inst_.succs[path[i + m]], inst_.nwords);
                    b_pred.bit_or(inst_.preds[path[i + m]], inst_.nwords);
                }
                int u_prev = path[i - 1], a_first = path[i], a_last = path[i + L - 1], u_next = path[i + L];
                int64_t rem_gain = inst_.raw_mat[u_prev * n + a_first] + inst_.raw_mat[a_last * n + u_next] - inst_.raw_mat[u_prev * n + u_next];
                for (int j = i + L; j < n - 1; ++j) {
                    if (b_succ.test(path[j])) break;
                    int w = path[j], w_next = path[j + 1];
                    int64_t ins_c = inst_.raw_mat[w * n + a_first] + inst_.raw_mat[a_last * n + w_next] - inst_.raw_mat[w * n + w_next];
                    if (ins_c < rem_gain) {
                        dlb3[u_prev] = dlb3[a_first] = dlb3[a_last] = dlb3[u_next] = dlb3[w] = dlb3[w_next] = 0;
                        std::rotate(path.begin() + i, path.begin() + i + L, path.begin() + j + 1);
                        best_c += (ins_c - rem_gain);
                        improved = true;
                        goto next_pass;
                    }
                }
                for (int j = i - 1; j >= 1; --j) {
                    if (b_pred.test(path[j])) break;
                    int w_prev = path[j - 1], w = path[j];
                    int64_t ins_c = inst_.raw_mat[w_prev * n + a_first] + inst_.raw_mat[a_last * n + w] - inst_.raw_mat[w_prev * n + w];
                    if (ins_c < rem_gain) {
                        dlb3[u_prev] = dlb3[a_first] = dlb3[a_last] = dlb3[u_next] = dlb3[w_prev] = dlb3[w] = 0;
                        std::rotate(path.begin() + j, path.begin() + i, path.begin() + i + L);
                        best_c += (ins_c - rem_gain);
                        improved = true;
                        goto next_pass;
                    }
                }
            }
        }
        next_pass:;
    }
}

void HeuristicEngine::ruin_and_recreate(std::vector<int>& path, int64_t& c) {
    int n = inst_.n;
    if (n <= 3) return;
    int max_k = std::min(12, std::max(3, n / 4));
    int k_rem = std::uniform_int_distribution<int>(3, max_k)(rng_);
    std::vector<int> removed;
    removed.reserve(k_rem);

    for (int r = 0; r < k_rem; ++r) {
        if (static_cast<int>(path.size()) <= 3) break;
        int idx = std::uniform_int_distribution<int>(1, static_cast<int>(path.size()) - 2)(rng_);
        removed.push_back(path[idx]);
        path.erase(path.begin() + idx);
    }
    std::shuffle(removed.begin(), removed.end(), rng_);

    for (int u : removed) {
        int m = static_cast<int>(path.size());
        int min_pos = 1, max_pos = m - 1;
        for (int i = 0; i < m; ++i) {
            int v = path[i];
            if (inst_.preds[u].test(v)) min_pos = std::max(min_pos, i + 1);
            if (inst_.succs[u].test(v)) max_pos = std::min(max_pos, i);
        }
        int best_pos = min_pos;
        int64_t best_delta = INF;
        for (int pos = min_pos; pos <= max_pos; ++pos) {
            int prev = path[pos - 1], nxt = path[pos];
            int64_t d = inst_.raw_mat[prev * n + u] + inst_.raw_mat[u * n + nxt] - inst_.raw_mat[prev * n + nxt];
            if (d < best_delta || (d == best_delta && std::uniform_int_distribution<int>(0, 1)(rng_))) {
                best_delta = d;
                best_pos = pos;
            }
        }
        path.insert(path.begin() + best_pos, u);
    }
    c = 0;
    for (int i = 0; i + 1 < n; ++i) c += inst_.raw_mat[path[i] * n + path[i + 1]];
}

uint64_t HeuristicEngine::run_portfolio(int starts, int lns_iters, double max_heur_sec) {
    uint64_t ops = 0;
    if (inst_.n == 1) {
        shared_.update_incumbent(0, {0});
        return 1;
    }
    std::vector<int> local_best_p;
    int64_t local_best_c = INF;

    for (int s = 0; s < starts && !shared_.is_timed_out(); ++s) {
        if (max_heur_sec > 0.0 && shared_.elapsed_sec() >= max_heur_sec && local_best_c < INF) break;
        double alpha = (s == 0) ? 0.0 : 0.20;
        auto p = construct_randomized(alpha);
        if (static_cast<int>(p.size()) != inst_.n) continue;
        int64_t c = 0;
        for (int i = 0; i + 1 < inst_.n; ++i) c += inst_.raw_mat[p[i] * inst_.n + p[i + 1]];
        shared_.update_incumbent(c, p);
        local_search(p, c, ops);
        shared_.update_incumbent(c, p);
        if (c < local_best_c) {
            local_best_c = c;
            local_best_p = std::move(p);
        }
    }
    if (local_best_p.empty()) return ops;

    for (int it = 0; it < lns_iters && !shared_.is_timed_out(); ++it) {
        if (max_heur_sec > 0.0 && shared_.elapsed_sec() >= max_heur_sec) break;
        int64_t glob_c = shared_.best_cost.load(std::memory_order_relaxed);
        if (glob_c < local_best_c) {
            std::lock_guard<std::mutex> lk(shared_.incumbent_mtx);
            local_best_c = shared_.best_cost.load(std::memory_order_relaxed);
            local_best_p = shared_.best_path;
        }
        auto cand_p = local_best_p;
        int64_t cand_c = local_best_c;
        ruin_and_recreate(cand_p, cand_c);
        local_search(cand_p, cand_c, ops);
        if (cand_c < local_best_c) {
            local_best_c = cand_c;
            local_best_p = cand_p;
            shared_.update_incumbent(cand_c, cand_p);
        }
    }
    return ops;
}

} // namespace sop