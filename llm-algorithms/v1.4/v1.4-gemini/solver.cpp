#include "solver.hpp"

#include "heuristics.hpp"
#include "history_table.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <utility>

namespace sop {

ExactSOPSolver::ExactSOPSolver(const Instance& inst, SharedSolverState& shared, const NumaTopology& numa, int num_threads)
    : inst_(inst), shared_(shared), numa_(numa), num_threads_(std::max(1, num_threads)) {}

ExactSOPSolver::SolveResult ExactSOPSolver::solve() {
    SolveResult res;
    if (inst_.has_cycle || inst_.n <= 0) {
        return res;
    }
    if (inst_.n == 1) {
        shared_.update_incumbent(0, {0});
        res.best_cost = 0;
        res.best_path = {0};
        res.enumerated_nodes = 1;
        res.optimal = true;
        return res;
    }

    // Step 1: Run multi-threaded initial heuristic portfolio to get a tight Upper Bound (UB)
    double heur_budget = shared_.has_time_limit
                             ? std::min(2.0, shared_.time_limit_sec * 0.25)
                             : ((inst_.n <= 60) ? 0.15 : 1.5);
    int base_starts = (inst_.n <= 60) ? 20 : 15;
    int base_lns = (inst_.n <= 60) ? 1500 : 350;

    std::vector<uint64_t> thread_counts(num_threads_, 0);
    {
        std::vector<std::thread> workers;
        for (int t = 0; t < num_threads_; ++t) {
            workers.emplace_back([&, t]() {
                numa_.pin_current_thread(t);
                HeuristicEngine he(inst_, shared_, 42ULL + static_cast<uint64_t>(t) * 1337ULL);
                int st = (t == 0) ? base_starts : std::max(6, base_starts / num_threads_);
                int lns = (t == 0) ? base_lns : std::max(100, base_lns / num_threads_);
                thread_counts[t] += he.run_portfolio(st, lns, heur_budget);
            });
        }
        for (auto& w : workers) w.join();
    }

    // Check if root lower bound already proves optimality of the heuristic incumbent
    int64_t root_lb = std::max(inst_.root_ap_lb, inst_.root_hk_lb);
    if (shared_.best_cost.load() <= root_lb) {
        res.best_cost = shared_.best_cost.load();
        res.best_path = shared_.best_path;
        for (uint64_t c : thread_counts) res.enumerated_nodes += c;
        res.optimal = true;
        return res;
    }

    // Step 2: Try Bidirectional Meet-in-the-Middle Bounded DP (Parallel Layer Expansion)
    bool mitm_completed = false;
    if (inst_.n <= 128 && !shared_.is_timed_out()) {
        mitm_completed = run_parallel_bidirectional_mitm(thread_counts);
    }

    // Step 3: If MITM layer width exceeded threshold (e.g., on large N=200..1000 instances)
    // and time remains, transition seamlessly into Parallel Work-Stealing Branch-and-Bound
    // coupled with background LNS intensification
    if (!mitm_completed && !shared_.is_timed_out()) {
        bool bnb_completed = run_parallel_bnb_and_lns(thread_counts);
        mitm_completed = bnb_completed;
    }

    for (uint64_t c : thread_counts) res.enumerated_nodes += c;
    int64_t final_cost = shared_.best_cost.load();
    if (final_cost < INF && !shared_.best_path.empty()) {
        res.best_cost = final_cost;
        res.best_path = shared_.best_path;
        res.optimal = mitm_completed && !shared_.is_timed_out();
    } else {
        res.best_cost = -1;
        res.best_path.clear();
        res.optimal = false;
    }
    return res;
}

bool ExactSOPSolver::reconstruct_subpath_dfs(
    int u, int target_end, const Bitset1024& allowed_mask, int target_len,
    Bitset1024& vis, int64_t cur_cost, int64_t target_cost, std::vector<int>& path,
    std::unordered_map<uint64_t, int64_t>& memo) {
    if (static_cast<int>(path.size()) == target_len) {
        return (u == target_end && cur_cost == target_cost);
    }
    uint64_t h = static_cast<uint64_t>(u) * 0x9e3779b97f4a7c15ULL;
    for (int w = 0; w < inst_.nwords; ++w) {
        h ^= vis.w[w] + 0x9e3779b97f4a7c15ULL * (w + 1) + (h << 6) + (h >> 2);
    }
    auto it = memo.find(h);
    if (it != memo.end() && it->second <= cur_cost) return false;
    memo[h] = cur_cost;

    int64_t rem_in = 0;
    for (int w = 0; w < inst_.n; ++w) {
        if (!allowed_mask.test(w) || vis.test(w)) continue;
        int64_t bi = inst_.cost[u * inst_.n + w];
        for (int p : inst_.adj_in[w]) {
            if (bi <= inst_.cost[p * inst_.n + w]) break;
            if (allowed_mask.test(p) && !vis.test(p)) {
                bi = inst_.cost[p * inst_.n + w];
                break;
            }
        }
        if (bi >= INF) return false;
        rem_in += bi;
    }
    if (cur_cost + rem_in > target_cost) return false;

    for (int v : inst_.adj_out[u]) {
        if (!allowed_mask.test(v) || vis.test(v)) continue;
        if (static_cast<int>(path.size()) + 1 < target_len && v == target_end) continue;
        if (!inst_.preds[v].is_subset_of(vis, inst_.nwords)) continue;
        int64_t ec = inst_.cost[u * inst_.n + v];
        if (cur_cost + ec > target_cost) continue;
        vis.set(v);
        path.push_back(v);
        if (reconstruct_subpath_dfs(v, target_end, allowed_mask, target_len, vis, cur_cost + ec, target_cost, path, memo)) {
            return true;
        }
        path.pop_back();
        vis.reset(v);
    }
    return false;
}

bool ExactSOPSolver::run_parallel_bidirectional_mitm(std::vector<uint64_t>& thread_counts) {
    LayerMap cur_fwd, cur_bwd;
    for (int s : inst_.start_candidates) {
        Bitset1024 m;
        m.set(s);
        cur_fwd[m].push_back({s, 0});
    }
    for (int e : inst_.end_candidates) {
        Bitset1024 m;
        m.set(e);
        cur_bwd[m].push_back({e, 0});
    }

    int f_depth = 1, b_depth = 1;
    const size_t MAX_LAYER_MASKS = 350000;

    while (f_depth + b_depth < inst_.n) {
        if (shared_.is_timed_out()) return false;
        if (cur_fwd.empty() || cur_bwd.empty()) {
            return true;
        }
        if (cur_fwd.size() > MAX_LAYER_MASKS && cur_bwd.size() > MAX_LAYER_MASKS) {
            return false;
        }

        int64_t ub = shared_.best_cost.load(std::memory_order_relaxed);
        bool expand_fwd = (cur_fwd.size() <= cur_bwd.size());

        if (expand_fwd) {
            std::vector<const LayerMap::value_type*> items;
            items.reserve(cur_fwd.size());
            for (const auto& kv : cur_fwd) items.push_back(&kv);

            std::vector<LayerMap> local_next(num_threads_);
            std::atomic<size_t> next_idx{0};

            auto worker_fn = [&](int tid) {
                numa_.pin_current_thread(tid);
                uint64_t local_enum = 0;
                auto& my_map = local_next[tid];
                const size_t chunk = 64;

                while (!shared_.is_timed_out()) {
                    size_t start_i = next_idx.fetch_add(chunk, std::memory_order_relaxed);
                    if (start_i >= items.size()) break;
                    size_t end_i = std::min(items.size(), start_i + chunk);

                    for (size_t idx = start_i; idx < end_i; ++idx) {
                        const Bitset1024& mask = items[idx]->first;
                        const auto& ends = items[idx]->second;

                        int64_t mst_val = inst_.mst_complement(mask);
                        if (mst_val >= (INF / 2) * Instance::HK_SCALE) continue;

                        int64_t sum_pi = 0;
                        for (int w = 0; w < inst_.n; ++w) {
                            if (!mask.test(w)) {
                                if (!inst_.end_mask.test(w)) sum_pi += inst_.pi_out[w];
                                sum_pi += inst_.pi_in[w];
                            }
                        }

                        int64_t sum_base_out = 0, sum_rbase_out = 0;
                        int64_t sum_base_in = 0, sum_rbase_in = 0;
                        int64_t max_end_bo = 0, max_end_rbo = 0;
                        int64_t base_in[MAX_NODES], rbase_in[MAX_NODES];
                        int inf_in_count = 0, inf_in_node = -1;
                        bool dead = false;

                        for (int w = 0; w < inst_.n; ++w) {
                            if (mask.test(w)) continue;
                            int64_t bi = INF, rbi = INF;
                            for (int p : inst_.adj_in[w]) {
                                if (!mask.test(p)) {
                                    bi = std::min(bi, inst_.cost[p * inst_.n + w]);
                                    rbi = std::min(rbi, inst_.rcost[p * inst_.n + w]);
                                }
                            }
                            base_in[w] = bi;
                            rbase_in[w] = rbi;
                            if (bi >= INF) {
                                inf_in_count++;
                                inf_in_node = w;
                                if (inf_in_count > 1) { dead = true; break; }
                            } else {
                                sum_base_in += bi;
                                sum_rbase_in += rbi;
                            }

                            if (inst_.end_candidates.size() == 1 && w == inst_.end_candidates[0]) {
                                continue;
                            }
                            int64_t bo = INF, rbo = INF;
                            for (int q : inst_.adj_out[w]) {
                                if (!mask.test(q)) {
                                    bo = std::min(bo, inst_.cost[w * inst_.n + q]);
                                    rbo = std::min(rbo, inst_.rcost[w * inst_.n + q]);
                                }
                            }
                            if (bo >= INF) {
                                if (!inst_.end_mask.test(w)) { dead = true; break; }
                                bo = 0; rbo = 0;
                            }
                            sum_base_out += bo;
                            sum_rbase_out += rbo;
                            if (inst_.end_candidates.size() > 1 && inst_.end_mask.test(w)) {
                                max_end_bo = std::max(max_end_bo, bo);
                                max_end_rbo = std::max(max_end_rbo, rbo);
                            }
                        }
                        if (dead) continue;
                        sum_base_out -= max_end_bo;
                        sum_rbase_out -= max_end_rbo;

                        std::vector<int> ready_v;
                        if (inf_in_count == 1) {
                            int v = inf_in_node;
                            if (inst_.preds[v].is_subset_of(mask, inst_.nwords) &&
                                !(f_depth < inst_.n - 1 && inst_.end_candidates.size() == 1 && v == inst_.end_candidates[0])) {
                                ready_v.push_back(v);
                            }
                        } else {
                            for (int v = 0; v < inst_.n; ++v) {
                                if (mask.test(v)) continue;
                                if (inst_.preds[v].is_subset_of(mask, inst_.nwords) &&
                                    !(f_depth < inst_.n - 1 && inst_.end_candidates.size() == 1 && v == inst_.end_candidates[0])) {
                                    ready_v.push_back(v);
                                }
                            }
                        }
                        if (ready_v.empty()) continue;

                        int64_t pot_mask = (inst_.start_candidates.size() == 1) ? -inst_.v_pot[inst_.start_candidates[0]] : 0;
                        for (int x = 0; x < inst_.n; ++x) {
                            if (mask.test(x)) pot_mask += inst_.u_pot[x] + inst_.v_pot[x];
                        }

                        for (int v : ready_v) {
                            int64_t hk_rem = (mst_val + sum_pi - inst_.pi_in[v] + Instance::HK_SCALE - 10) / Instance::HK_SCALE;
                            int64_t rem_in = (inf_in_count == 1) ? sum_base_in : (sum_base_in - base_in[v]);
                            int64_t lb_orig = std::max({hk_rem, rem_in, sum_base_out});
                            int64_t rem_rin = (inf_in_count == 1) ? sum_rbase_in : (sum_rbase_in - rbase_in[v]);
                            int64_t lb_rc = std::max(rem_rin, sum_rbase_out);

                            Bitset1024 nmask = mask;
                            nmask.set(v);

                            for (const auto& [u, pcost] : ends) {
                                local_enum++;
                                int64_t ec = inst_.cost[u * inst_.n + v];
                                if (ec >= INF) continue;
                                int64_t new_cost = pcost + ec;
                                if (new_cost + lb_orig > ub) continue;
                                if (inst_.start_candidates.size() == 1) {
                                    int64_t prcost = pcost - (pot_mask - inst_.u_pot[u]);
                                    if (inst_.root_ap_lb + prcost + inst_.rcost[u * inst_.n + v] + lb_rc > ub) continue;
                                }
                                auto& vec = my_map[nmask];
                                bool found = false;
                                for (auto& pr : vec) {
                                    if (pr.first == v) {
                                        if (new_cost < pr.second) pr.second = new_cost;
                                        found = true;
                                        break;
                                    }
                                }
                                if (!found) vec.push_back({v, new_cost});
                            }
                        }
                    }
                }
                thread_counts[tid] += local_enum;
            };

            if (num_threads_ == 1 || items.size() < 256) {
                worker_fn(0);
                cur_fwd = std::move(local_next[0]);
            } else {
                std::vector<std::thread> workers;
                for (int t = 0; t < num_threads_; ++t) workers.emplace_back(worker_fn, t);
                for (auto& w : workers) w.join();
                cur_fwd = std::move(local_next[0]);
                for (int t = 1; t < num_threads_; ++t) {
                    for (auto& kv : local_next[t]) {
                        auto& dst = cur_fwd[kv.first];
                        for (auto& [v, c] : kv.second) {
                            bool found = false;
                            for (auto& pr : dst) {
                                if (pr.first == v) {
                                    if (c < pr.second) pr.second = c;
                                    found = true;
                                    break;
                                }
                            }
                            if (!found) dst.push_back({v, c});
                        }
                    }
                }
            }
            f_depth++;
        } else {
            std::vector<const LayerMap::value_type*> items;
            items.reserve(cur_bwd.size());
            for (const auto& kv : cur_bwd) items.push_back(&kv);

            std::vector<LayerMap> local_next(num_threads_);
            std::atomic<size_t> next_idx{0};

            auto worker_fn = [&](int tid) {
                numa_.pin_current_thread(tid);
                uint64_t local_enum = 0;
                auto& my_map = local_next[tid];
                const size_t chunk = 64;

                while (!shared_.is_timed_out()) {
                    size_t start_i = next_idx.fetch_add(chunk, std::memory_order_relaxed);
                    if (start_i >= items.size()) break;
                    size_t end_i = std::min(items.size(), start_i + chunk);

                    for (size_t idx = start_i; idx < end_i; ++idx) {
                        const Bitset1024& smask = items[idx]->first;
                        const auto& starts = items[idx]->second;

                        int64_t mst_val = inst_.mst_complement(smask);
                        if (mst_val >= (INF / 2) * Instance::HK_SCALE) continue;

                        int64_t sum_pi = 0;
                        for (int w = 0; w < inst_.n; ++w) {
                            if (!smask.test(w)) {
                                sum_pi += inst_.pi_out[w];
                                if (!inst_.start_mask.test(w)) sum_pi += inst_.pi_in[w];
                            }
                        }

                        int64_t sum_base_in = 0, sum_rbase_in = 0;
                        int64_t sum_base_out = 0, sum_rbase_out = 0;
                        int64_t max_start_bi = 0, max_start_rbi = 0;
                        int64_t base_out[MAX_NODES], rbase_out[MAX_NODES];
                        int inf_out_count = 0, inf_out_node = -1;
                        bool dead = false;

                        for (int w = 0; w < inst_.n; ++w) {
                            if (smask.test(w)) continue;
                            int64_t bo = INF, rbo = INF;
                            for (int q : inst_.adj_out[w]) {
                                if (!smask.test(q)) {
                                    bo = std::min(bo, inst_.cost[w * inst_.n + q]);
                                    rbo = std::min(rbo, inst_.rcost[w * inst_.n + q]);
                                }
                            }
                            base_out[w] = bo;
                            rbase_out[w] = rbo;
                            if (bo >= INF) {
                                inf_out_count++;
                                inf_out_node = w;
                                if (inf_out_count > 1) { dead = true; break; }
                            } else {
                                sum_base_out += bo;
                                sum_rbase_out += rbo;
                            }

                            if (inst_.start_candidates.size() == 1 && w == inst_.start_candidates[0]) {
                                continue;
                            }
                            int64_t bi = INF, rbi = INF;
                            for (int p : inst_.adj_in[w]) {
                                if (!smask.test(p)) {
                                    bi = std::min(bi, inst_.cost[p * inst_.n + w]);
                                    rbi = std::min(rbi, inst_.rcost[p * inst_.n + w]);
                                }
                            }
                            if (bi >= INF) {
                                if (!inst_.start_mask.test(w)) { dead = true; break; }
                                bi = 0; rbi = 0;
                            }
                            sum_base_in += bi;
                            sum_rbase_in += rbi;
                            if (inst_.start_candidates.size() > 1 && inst_.start_mask.test(w)) {
                                max_start_bi = std::max(max_start_bi, bi);
                                max_start_rbi = std::max(max_start_rbi, rbi);
                            }
                        }
                        if (dead) continue;
                        sum_base_in -= max_start_bi;
                        sum_rbase_in -= max_start_rbi;

                        std::vector<int> ready_u;
                        if (inf_out_count == 1) {
                            int u = inf_out_node;
                            if (inst_.succs[u].is_subset_of(smask, inst_.nwords) &&
                                !(b_depth < inst_.n - 1 && inst_.start_candidates.size() == 1 && u == inst_.start_candidates[0])) {
                                ready_u.push_back(u);
                            }
                        } else {
                            for (int u = 0; u < inst_.n; ++u) {
                                if (smask.test(u)) continue;
                                if (inst_.succs[u].is_subset_of(smask, inst_.nwords) &&
                                    !(b_depth < inst_.n - 1 && inst_.start_candidates.size() == 1 && u == inst_.start_candidates[0])) {
                                    ready_u.push_back(u);
                                }
                            }
                        }
                        if (ready_u.empty()) continue;

                        int64_t pot_smask = (inst_.end_candidates.size() == 1) ? -inst_.u_pot[inst_.end_candidates[0]] : 0;
                        for (int x = 0; x < inst_.n; ++x) {
                            if (smask.test(x)) pot_smask += inst_.u_pot[x] + inst_.v_pot[x];
                        }

                        for (int u : ready_u) {
                            int64_t hk_rem = (mst_val + sum_pi - inst_.pi_out[u] + Instance::HK_SCALE - 10) / Instance::HK_SCALE;
                            int64_t rem_out = (inf_out_count == 1) ? sum_base_out : (sum_base_out - base_out[u]);
                            int64_t lb_orig = std::max({hk_rem, sum_base_in, rem_out});
                            int64_t rem_rout = (inf_out_count == 1) ? sum_rbase_out : (sum_rbase_out - rbase_out[u]);
                            int64_t lb_rc = std::max(sum_rbase_in, rem_rout);

                            Bitset1024 nmask = smask;
                            nmask.set(u);

                            for (const auto& [v, scost] : starts) {
                                local_enum++;
                                int64_t ec = inst_.cost[u * inst_.n + v];
                                if (ec >= INF) continue;
                                int64_t new_cost = scost + ec;
                                if (new_cost + lb_orig > ub) continue;
                                if (inst_.end_candidates.size() == 1) {
                                    int64_t srcost = scost - (pot_smask - inst_.v_pot[v]);
                                    if (inst_.root_ap_lb + srcost + inst_.rcost[u * inst_.n + v] + lb_rc > ub) continue;
                                }
                                auto& vec = my_map[nmask];
                                bool found = false;
                                for (auto& pr : vec) {
                                    if (pr.first == u) {
                                        if (new_cost < pr.second) pr.second = new_cost;
                                        found = true;
                                        break;
                                    }
                                }
                                if (!found) vec.push_back({u, new_cost});
                            }
                        }
                    }
                }
                thread_counts[tid] += local_enum;
            };

            if (num_threads_ == 1 || items.size() < 256) {
                worker_fn(0);
                cur_bwd = std::move(local_next[0]);
            } else {
                std::vector<std::thread> workers;
                for (int t = 0; t < num_threads_; ++t) workers.emplace_back(worker_fn, t);
                for (auto& w : workers) w.join();
                cur_bwd = std::move(local_next[0]);
                for (int t = 1; t < num_threads_; ++t) {
                    for (auto& kv : local_next[t]) {
                        auto& dst = cur_bwd[kv.first];
                        for (auto& [u, c] : kv.second) {
                            bool found = false;
                            for (auto& pr : dst) {
                                if (pr.first == u) {
                                    if (c < pr.second) pr.second = c;
                                    found = true;
                                    break;
                                }
                            }
                            if (!found) dst.push_back({u, c});
                        }
                    }
                }
            }
            b_depth++;
        }
    }

    if (shared_.is_timed_out()) return false;

    Bitset1024 all_mask;
    for (int i = 0; i < inst_.n; ++i) all_mask.set(i);

    int64_t best_found = shared_.best_cost.load();
    Bitset1024 best_fmask, best_bmask;
    int best_u = -1, best_v = -1;
    int64_t best_fc = 0, best_bc = 0;

    for (const auto& kv : cur_fwd) {
        const Bitset1024& fmask = kv.first;
        Bitset1024 bmask;
        for (int w = 0; w < inst_.nwords; ++w) {
            bmask.w[w] = (~fmask.w[w]) & all_mask.w[w];
        }
        auto it = cur_bwd.find(bmask);
        if (it != cur_bwd.end()) {
            for (const auto& [u, fc] : kv.second) {
                for (const auto& [v, bc] : it->second) {
                    thread_counts[0]++;
                    int64_t ec = inst_.cost[u * inst_.n + v];
                    if (ec < INF && fc + ec + bc < best_found) {
                        best_found = fc + ec + bc;
                        best_fmask = fmask;
                        best_bmask = bmask;
                        best_u = u;
                        best_v = v;
                        best_fc = fc;
                        best_bc = bc;
                    }
                }
            }
        }
    }

    if (best_u != -1 && best_found < shared_.best_cost.load()) {
        std::vector<int> fpath, bpath;
        std::unordered_map<uint64_t, int64_t> memo_f, memo_b;
        for (int s : inst_.start_candidates) {
            if (!best_fmask.test(s)) continue;
            Bitset1024 vis;
            vis.set(s);
            fpath = {s};
            if (reconstruct_subpath_dfs(s, best_u, best_fmask, f_depth, vis, 0, best_fc, fpath, memo_f)) break;
        }
        Bitset1024 vis_b = best_fmask;
        vis_b.set(best_v);
        bpath = {best_v};
        for (int e : inst_.end_candidates) {
            if (!best_bmask.test(e)) continue;
            if (reconstruct_subpath_dfs(best_v, e, best_bmask, b_depth, vis_b, 0, best_bc, bpath, memo_b)) break;
        }
        if (static_cast<int>(fpath.size()) == f_depth && static_cast<int>(bpath.size()) == b_depth) {
            fpath.insert(fpath.end(), bpath.begin(), bpath.end());
            shared_.update_incumbent(best_found, fpath);
        }
    }
    return true;
}

bool ExactSOPSolver::run_parallel_bnb_and_lns(std::vector<uint64_t>& thread_counts) {
    ShardedHistoryTable hist(1 << 20);

    int64_t tot_in = 0, tot_out = 0;
    for (int i = 0; i < inst_.n; ++i) {
        tot_in += inst_.min_in1[i];
        if (!inst_.end_mask.test(i)) tot_out += inst_.min_out1[i];
    }

    std::vector<BnBTask> tasks;
    for (int s : inst_.start_candidates) {
        Bitset1024 vis1;
        vis1.set(s);
        int64_t r_in1 = tot_in - inst_.min_in1[s];
        int64_t r_out1 = tot_out - (inst_.end_mask.test(s) ? 0 : inst_.min_out1[s]);

        for (int v : inst_.adj_out[s]) {
            if (vis1.test(v)) continue;
            if (!inst_.preds[v].is_subset_of(vis1, inst_.nwords)) continue;
            if (inst_.n > 2 && inst_.end_candidates.size() == 1 && v == inst_.end_candidates[0]) continue;
            int64_t ec = inst_.cost[s * inst_.n + v];
            int64_t r_in2 = r_in1 - inst_.min_in1[v];
            int64_t r_out2 = r_out1 - (inst_.end_mask.test(v) ? 0 : inst_.min_out1[v]);
            if (ec + std::max(r_in2, r_out2) >= shared_.best_cost.load()) continue;

            BnBTask t;
            t.prefix = {s, v};
            t.vis = vis1;
            t.vis.set(v);
            t.hash = inst_.zobrist[s] ^ inst_.zobrist[v];
            t.prefix_cost = ec;
            t.rem_min_in = r_in2;
            t.rem_min_out = r_out2;
            tasks.push_back(std::move(t));
        }
    }

    std::sort(tasks.begin(), tasks.end(), [](const BnBTask& a, const BnBTask& b) {
        return a.prefix_cost < b.prefix_cost;
    });

    std::atomic<size_t> task_idx{0};
    std::atomic<bool> bnb_finished{false};

    auto compute_dyn_lb = [&](int u, const Bitset1024& vis, int64_t prefix_cost) -> int64_t {
        int n = inst_.n;
        int64_t sum_in = 0, sum_out = 0;

        int64_t u_out = INF;
        for (int v : inst_.adj_out[u]) {
            if (!vis.test(v)) { u_out = inst_.cost[u * n + v]; break; }
        }
        if (u_out >= INF) return INF;
        sum_out += u_out;

        for (int w = 0; w < n; ++w) {
            if (vis.test(w)) continue;
            int s1 = inst_.min_in1_src[w];
            int64_t best_in = INF;
            if (s1 != -1 && (!vis.test(s1) || s1 == u)) {
                best_in = inst_.min_in1[w];
            } else {
                best_in = inst_.cost[u * n + w];
                for (int p : inst_.adj_in[w]) {
                    if (best_in <= inst_.cost[p * n + w]) break;
                    if (!vis.test(p)) {
                        best_in = inst_.cost[p * n + w];
                        break;
                    }
                }
            }
            if (best_in >= INF) return INF;
            sum_in += best_in;

            if (!inst_.end_mask.test(w)) {
                int d1 = inst_.min_out1_dst[w];
                int64_t best_out = INF;
                if (d1 != -1 && !vis.test(d1)) {
                    best_out = inst_.min_out1[w];
                } else {
                    for (int nxt : inst_.adj_out[w]) {
                        if (!vis.test(nxt)) {
                            best_out = inst_.cost[w * n + nxt];
                            break;
                        }
                    }
                }
                if (best_out >= INF) return INF;
                sum_out += best_out;
            }
        }
        return prefix_cost + std::max(sum_in, sum_out);
    };

    auto bnb_worker = [&](int tid) {
        numa_.pin_current_thread(tid);
        uint64_t local_nodes = 0;
        std::vector<int> cur_path;
        cur_path.reserve(inst_.n);

        auto dfs = [&](auto& self, int u, int depth, Bitset1024& vis, uint64_t h,
                       int64_t pcost, int64_t r_in, int64_t r_out) -> void {
            local_nodes++;
            if ((local_nodes & 4095) == 0 && shared_.is_timed_out()) return;

            int64_t cur_ub = shared_.best_cost.load(std::memory_order_relaxed);
            if (depth == inst_.n) {
                if (pcost < cur_ub) {
                    shared_.update_incumbent(pcost, cur_path);
                }
                return;
            }
            if (pcost + std::max(r_in, r_out + inst_.min_out1[u]) >= cur_ub) return;
            if (!hist.check_and_update(h, u, vis, pcost, inst_.nwords)) return;

            if (compute_dyn_lb(u, vis, pcost) >= cur_ub) return;

            for (int v : inst_.adj_out[u]) {
                if (vis.test(v)) continue;
                if (!inst_.preds[v].is_subset_of(vis, inst_.nwords)) continue;
                if (depth < inst_.n - 1 && inst_.end_candidates.size() == 1 && v == inst_.end_candidates[0]) continue;

                int64_t ec = inst_.cost[u * inst_.n + v];
                int64_t nxt_in = r_in - inst_.min_in1[v];
                int64_t nxt_out = r_out - (inst_.end_mask.test(v) ? 0 : inst_.min_out1[v]);
                if (pcost + ec + std::max(nxt_in, nxt_out) >= shared_.best_cost.load(std::memory_order_relaxed)) {
                    continue;
                }

                vis.set(v);
                cur_path.push_back(v);
                self(self, v, depth + 1, vis, h ^ inst_.zobrist[v], pcost + ec, nxt_in, nxt_out);
                cur_path.pop_back();
                vis.reset(v);
                if ((local_nodes & 4095) == 0 && shared_.is_timed_out()) return;
            }
        };

        while (!shared_.is_timed_out()) {
            size_t idx = task_idx.fetch_add(1, std::memory_order_relaxed);
            if (idx >= tasks.size()) break;
            BnBTask t = tasks[idx];
            cur_path = t.prefix;
            dfs(dfs, cur_path.back(), static_cast<int>(cur_path.size()), t.vis, t.hash,
                t.prefix_cost, t.rem_min_in, t.rem_min_out);
        }
        thread_counts[tid] += local_nodes;
    };

    int lns_threads = (num_threads_ >= 4) ? std::max(1, num_threads_ / 4) : 0;
    int bnb_threads = num_threads_ - lns_threads;

    std::vector<std::thread> workers;
    for (int t = 0; t < bnb_threads; ++t) {
        workers.emplace_back(bnb_worker, t);
    }
    for (int t = bnb_threads; t < num_threads_; ++t) {
        workers.emplace_back([&, t]() {
            numa_.pin_current_thread(t);
            HeuristicEngine he(inst_, shared_, 99991ULL + static_cast<uint64_t>(t) * 7919ULL);
            uint64_t ops = 0;
            std::vector<int> local_p;
            int64_t local_c = INF;
            {
                std::lock_guard<std::mutex> lk(shared_.incumbent_mtx);
                local_c = shared_.best_cost.load();
                local_p = shared_.best_path;
            }
            while (!bnb_finished.load(std::memory_order_relaxed) && !shared_.is_timed_out()) {
                int64_t glob_c = shared_.best_cost.load(std::memory_order_relaxed);
                if (glob_c < local_c) {
                    std::lock_guard<std::mutex> lk(shared_.incumbent_mtx);
                    local_c = shared_.best_cost.load();
                    local_p = shared_.best_path;
                }
                auto cand_p = local_p;
                int64_t cand_c = local_c;
                he.ruin_and_recreate(cand_p, cand_c);
                he.local_search(cand_p, cand_c, ops);
                if (cand_c < local_c) {
                    local_c = cand_c;
                    local_p = cand_p;
                    shared_.update_incumbent(cand_c, cand_p);
                }
            }
            thread_counts[t] += ops;
        });
    }

    for (int t = 0; t < bnb_threads; ++t) {
        workers[t].join();
    }
    bnb_finished.store(true, std::memory_order_relaxed);
    for (int t = bnb_threads; t < num_threads_; ++t) {
        workers[t].join();
    }

    return !shared_.is_timed_out();
}

} // namespace sop