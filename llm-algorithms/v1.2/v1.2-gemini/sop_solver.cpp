/**
 * State-of-the-Art Parallel Exact Solver for the Sequential Ordering Problem (SOP)
 * Optimized for AMD Ryzen Threadripper 2990WX (32 Cores, 4 NUMA Nodes, Zen+ AVX2)
 *
 * Compile:
 *   g++ -O3 -march=znver1 -mtune=znver1 -flto -pthread -std=c++20 sop_solver.cpp -o sop_solver
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <thread>
#include <mutex>
#include <bitset>
#include <cmath>
#include <cstring>
#include <climits>
#include <iomanip>
#include <filesystem>
#include <random>
#include <queue>

#ifdef __linux__
#include <sched.h>
#include <pthread.h>
#endif

using namespace std;

static constexpr int MAX_N = 1024;
static constexpr int64_t INF_COST = 1000000000000000LL; // 10^15
using BitMask = bitset<MAX_N>;

// ============================================================================
// Command Line Options
// ============================================================================
struct Options {
    string filepath;
    double time_limit = -1.0;
    int threads = 1;
    string output_csv = "";
    bool no_header = false;
};

void print_usage(const char* prog) {
    cerr << "Usage: " << prog << " [filepath] [--time-limit SECONDS] [--threads N] [--output out.csv] [--no-header]\n";
}

bool parse_args(int argc, char** argv, Options& opts) {
    for (int i = 1; i < argc; ++i) {
        string arg = argv[i];
        if (arg == "--time-limit" && i + 1 < argc) {
            opts.time_limit = stod(argv[++i]);
        } else if (arg == "--threads" && i + 1 < argc) {
            opts.threads = stoi(argv[++i]);
            if (opts.threads < 1) opts.threads = 1;
        } else if (arg == "--output" && i + 1 < argc) {
            opts.output_csv = argv[++i];
        } else if (arg == "--no-header") {
            opts.no_header = true;
        } else if (arg[0] == '-') {
            cerr << "Unknown or incomplete option: " << arg << "\n";
            return false;
        } else if (opts.filepath.empty()) {
            opts.filepath = arg;
        } else {
            cerr << "Unexpected positional argument: " << arg << "\n";
            return false;
        }
    }
    return !opts.filepath.empty();
}

// ============================================================================
// SOP Instance Representation (Read-Only / Replicated per NUMA Domain)
// ============================================================================
struct SOPInstance {
    string name;
    int n = 0;
    vector<int64_t> orig_T; // Original matrix
    vector<int64_t> C;      // Pruned cost matrix (INF_COST for forbidden edges)
    
    vector<BitMask> pred;   // pred[i] has bit j set if j must precede i
    vector<BitMask> succ;   // succ[i] has bit j set if i must precede j
    vector<int> in_degree;  // Initial precedence in-degree
    vector<int> out_degree; // Initial precedence out-degree

    vector<int64_t> min_out; // Minimum valid outgoing edge from i
    vector<int64_t> min_in;  // Minimum valid incoming edge to i

    vector<vector<int>> adj_out; // Sorted valid outgoing neighbors
    vector<vector<int>> adj_in;  // Sorted valid incoming neighbors

    bool trivial_unsat = false;
};

// ============================================================================
// TSPLIB .sop Parser
// ============================================================================
bool parse_sop_file(const string& filepath, SOPInstance& inst) {
    ifstream in(filepath);
    if (!in.is_open()) {
        cerr << "[ERROR] Cannot open file: " << filepath << "\n";
        return false;
    }

    inst.name = filesystem::path(filepath).filename().string();
    string line;
    bool in_matrix = false;

    while (getline(in, line)) {
        // Trim leading/trailing whitespace
        size_t first = line.find_first_not_of(" \t\r\n");
        if (first == string::npos) continue;
        string trimmed = line.substr(first);

        if (trimmed.rfind("EDGE_WEIGHT_SECTION", 0) == 0) {
            in_matrix = true;
            break;
        }

        size_t colon = trimmed.find(':');
        if (colon != string::npos) {
            string key = trimmed.substr(0, colon);
            string val = trimmed.substr(colon + 1);
            key.erase(key.find_last_not_of(" \t\r\n") + 1);
            size_t vfirst = val.find_first_not_of(" \t\r\n");
            val = (vfirst == string::npos) ? "" : val.substr(vfirst);
            val.erase(val.find_last_not_of(" \t\r\n") + 1);

            if (key == "DIMENSION") {
                inst.n = stoi(val);
            }
        } else {
            stringstream ss(trimmed);
            string key;
            ss >> key;
            if (key == "DIMENSION") {
                ss >> inst.n;
            }
        }
    }

    if (!in_matrix) {
        cerr << "[ERROR] EDGE_WEIGHT_SECTION not found.\n";
        return false;
    }

    vector<int64_t> tokens;
    tokens.reserve((inst.n > 0 ? inst.n * inst.n + 2 : 10000));
    string tok;
    while (in >> tok) {
        if (tok == "EOF" || tok == "-999999999") break;
        try {
            tokens.push_back(stoll(tok));
        } catch (...) {
            break;
        }
    }

    if (tokens.empty()) {
        cerr << "[ERROR] Empty matrix section.\n";
        return false;
    }

    size_t offset = 0;
    if (inst.n <= 0) {
        inst.n = static_cast<int>(tokens[0]);
        offset = 1;
    } else if (tokens.size() >= static_cast<size_t>(inst.n * inst.n + 1) && tokens[0] == inst.n) {
        offset = 1;
    }

    int N = inst.n;
    if (N <= 0 || N > MAX_N) {
        cerr << "[ERROR] Invalid dimension N = " << N << " (MAX_N = " << MAX_N << ")\n";
        return false;
    }
    if (tokens.size() < offset + static_cast<size_t>(N) * N) {
        cerr << "[ERROR] Not enough matrix entries. Expected " << N * N << ", got " << tokens.size() - offset << "\n";
        return false;
    }

    inst.orig_T.resize(N * N);
    for (int i = 0; i < N * N; ++i) {
        inst.orig_T[i] = tokens[offset + i];
    }

    return true;
}

// ============================================================================
// Preprocessing: Transitive Closure & Precedence Arc Elimination
// ============================================================================
void preprocess_instance(SOPInstance& inst) {
    int N = inst.n;
    inst.pred.assign(N, BitMask());
    inst.succ.assign(N, BitMask());
    inst.C.assign(N * N, INF_COST);

    for (int i = 0; i < N; ++i) {
        for (int j = 0; j < N; ++j) {
            if (i == j) continue;
            int64_t val = inst.orig_T[i * N + j];
            if (val < 0) {
                // T[i][j] < 0 means i depends on j -> j must precede i
                inst.pred[i].set(j);
                inst.succ[j].set(i);
            } else {
                inst.C[i * N + j] = val;
            }
        }
    }

    // Floyd-Warshall Bitset Transitive Closure
    for (int k = 0; k < N; ++k) {
        for (int i = 0; i < N; ++i) {
            if (inst.pred[i].test(k)) {
                inst.pred[i] |= inst.pred[k];
            }
            if (inst.succ[i].test(k)) {
                inst.succ[i] |= inst.succ[k];
            }
        }
    }

    // Cycle check in precedence constraints
    for (int i = 0; i < N; ++i) {
        if (inst.pred[i].test(i)) {
            inst.trivial_unsat = true;
            return;
        }
    }

    inst.in_degree.assign(N, 0);
    inst.out_degree.assign(N, 0);
    for (int i = 0; i < N; ++i) {
        inst.in_degree[i] = inst.pred[i].count();
        inst.out_degree[i] = inst.succ[i].count();
    }

    // Arc Elimination Rules:
    // 1. If j must precede i (pred[i][j] == 1), direct edge i -> j is impossible.
    // 2. If there exists k such that i -> k -> j in precedence (succ[i][k] && pred[j][k]),
    //    then k must be visited strictly between i and j, so direct edge i -> j is impossible!
    // 3. Any node with out_degree > 0 cannot be the last node, so it cannot go directly to a sink if other nodes remain.
    // 4. A source (in_degree == 0) cannot have incoming edges if it is the unique start node.
    vector<int> sources, sinks;
    for (int i = 0; i < N; ++i) {
        if (inst.in_degree[i] == 0) sources.push_back(i);
        if (inst.out_degree[i] == 0) sinks.push_back(i);
    }
    if (sources.empty() || sinks.empty()) {
        inst.trivial_unsat = true;
        return;
    }

    int unique_start = (sources.size() == 1) ? sources[0] : -1;
    int unique_end   = (sinks.size() == 1)   ? sinks[0]   : -1;

    for (int i = 0; i < N; ++i) {
        for (int j = 0; j < N; ++j) {
            if (i == j) {
                inst.C[i * N + j] = INF_COST;
                continue;
            }
            if (unique_start != -1 && j == unique_start) {
                inst.C[i * N + j] = INF_COST;
                continue;
            }
            if (unique_end != -1 && i == unique_end) {
                inst.C[i * N + j] = INF_COST;
                continue;
            }
            if (unique_start != -1 && unique_end != -1 && N > 2 && i == unique_start && j == unique_end) {
                inst.C[i * N + j] = INF_COST;
                continue;
            }
            // Precedence conflict: j precedes i
            if (inst.pred[i].test(j)) {
                inst.C[i * N + j] = INF_COST;
                continue;
            }
            // Transitive intermediate node: exists k such that i < k < j
            if ((inst.succ[i] & inst.pred[j]).any()) {
                inst.C[i * N + j] = INF_COST;
                continue;
            }
            // If j has prerequisites other than i and i's prerequisites, and i is the unique start node, i->j directly is invalid
            if (unique_start != -1 && i == unique_start) {
                BitMask rem = inst.pred[j];
                rem.reset(i);
                if (rem.any()) {
                    inst.C[i * N + j] = INF_COST;
                    continue;
                }
            }
            // Symmetrically for unique_end
            if (unique_end != -1 && j == unique_end) {
                BitMask rem = inst.succ[i];
                rem.reset(j);
                if (rem.any()) {
                    inst.C[i * N + j] = INF_COST;
                    continue;
                }
            }
        }
    }

    inst.min_out.assign(N, INF_COST);
    inst.min_in.assign(N, INF_COST);
    inst.adj_out.assign(N, {});
    inst.adj_in.assign(N, {});

    for (int i = 0; i < N; ++i) {
        for (int j = 0; j < N; ++j) {
            if (inst.C[i * N + j] < INF_COST) {
                inst.min_out[i] = min(inst.min_out[i], inst.C[i * N + j]);
                inst.min_in[j]  = min(inst.min_in[j],  inst.C[i * N + j]);
                inst.adj_out[i].push_back(j);
                inst.adj_in[j].push_back(i);
            }
        }
        sort(inst.adj_out[i].begin(), inst.adj_out[i].end(), [&](int a, int b) {
            return inst.C[i * N + a] < inst.C[i * N + b];
        });
        sort(inst.adj_in[i].begin(), inst.adj_in[i].end(), [&](int a, int b) {
            return inst.C[a * N + i] < inst.C[b * N + i];
        });
    }
}

// ============================================================================
// Solution Validator (Recomputes Permutation, Precedence, Cost from Scratch)
// ============================================================================
bool validate_solution(const SOPInstance& inst, const vector<int>& path, int64_t reported_cost) {
    int N = inst.n;
    if (static_cast<int>(path.size()) != N) {
        cerr << "[VALIDATOR] Invalid path length: " << path.size() << " != " << N << "\n";
        return false;
    }
    vector<int> pos(N, -1);
    for (int idx = 0; idx < N; ++idx) {
        int u = path[idx];
        if (u < 0 || u >= N) {
            cerr << "[VALIDATOR] Node out of range: " << u << "\n";
            return false;
        }
        if (pos[u] != -1) {
            cerr << "[VALIDATOR] Duplicate node in path: " << u << "\n";
            return false;
        }
        pos[u] = idx;
    }

    // Check all original precedence constraints from scratch
    for (int i = 0; i < N; ++i) {
        for (int j = 0; j < N; ++j) {
            if (i != j && inst.orig_T[i * N + j] < 0) {
                // j must come before i
                if (pos[j] > pos[i]) {
                    cerr << "[VALIDATOR] Precedence violated: node " << j << " must precede " << i << "\n";
                    return false;
                }
            }
        }
    }

    int64_t exact_cost = 0;
    for (int idx = 0; idx + 1 < N; ++idx) {
        int u = path[idx];
        int v = path[idx + 1];
        int64_t edge_cost = inst.orig_T[u * N + v];
        if (edge_cost < 0) {
            cerr << "[VALIDATOR] Used negative precedence edge directly: " << u << " -> " << v << "\n";
            return false;
        }
        exact_cost += edge_cost;
    }

    if (exact_cost != reported_cost) {
        cerr << "[VALIDATOR] Cost mismatch: recomputed " << exact_cost << " != reported " << reported_cost << "\n";
        return false;
    }
    return true;
}

// ============================================================================
// Primal Heuristics: Multi-Start Randomized Greedy + SOP-Or-Opt / 2-Opt Search
// ============================================================================
int64_t compute_path_cost(const SOPInstance& inst, const vector<int>& path) {
    int N = inst.n;
    int64_t cost = 0;
    for (int i = 0; i + 1 < N; ++i) {
        int64_t c = inst.C[path[i] * N + path[i + 1]];
        if (c >= INF_COST) return INF_COST;
        cost += c;
    }
    return cost;
}

// Fast Precedence-Preserving Local Search (Relocate / Block Or-Opt + 2-Opt)
void sop_local_search(const SOPInstance& inst, vector<int>& path, int64_t& cost) {
    int N = inst.n;
    if (N <= 2) return;

    bool improved = true;
    while (improved) {
        improved = false;

        // 1. Single-node and small-block relocation (Or-Opt with block sizes 1, 2, 3)
        for (int blk = 1; blk <= min(3, N - 1); ++blk) {
            for (int i = 0; i + blk <= N; ++i) {
                // Find earliest and latest valid insertion index for block [i, i+blk-1]
                BitMask blk_preds, blk_succs;
                for (int k = 0; k < blk; ++k) {
                    blk_preds |= inst.pred[path[i + k]];
                    blk_succs |= inst.succ[path[i + k]];
                }

                int min_ins = 0; // earliest position before which all blk_preds appear
                int max_ins = N - blk; // latest position after which all blk_succs appear

                for (int p = 0; p < i; ++p) {
                    if (blk_preds.test(path[p])) min_ins = p + 1;
                }
                for (int p = N - 1; p >= i + blk; --p) {
                    if (blk_succs.test(path[p])) max_ins = p - blk;
                }

                int u_prev = (i > 0) ? path[i - 1] : -1;
                int u_first = path[i];
                int u_last = path[i + blk - 1];
                int u_next = (i + blk < N) ? path[i + blk] : -1;

                int64_t old_rem_cost = 0;
                if (u_prev != -1) old_rem_cost += inst.C[u_prev * N + u_first];
                if (u_next != -1) old_rem_cost += inst.C[u_last * N + u_next];
                int64_t new_join_cost = 0;
                if (u_prev != -1 && u_next != -1) {
                    new_join_cost = inst.C[u_prev * N + u_next];
                    if (new_join_cost >= INF_COST) continue;
                }

                int best_j = -1;
                int64_t best_delta = 0;

                for (int j = min_ins; j <= max_ins; ++j) {
                    if (j == i) continue;
                    // If inserting at j in the contracted array:
                    // Contracted array has size N - blk. Position j is between contracted[j-1] and contracted[j].
                    int v_prev = -1, v_next = -1;
                    if (j < i) {
                        v_prev = (j > 0) ? path[j - 1] : -1;
                        v_next = path[j];
                    } else {
                        v_prev = path[j + blk - 1];
                        v_next = (j + blk < N) ? path[j + blk] : -1;
                    }

                    int64_t old_ins_cost = 0;
                    if (v_prev != -1 && v_next != -1) old_ins_cost = inst.C[v_prev * N + v_next];

                    int64_t new_ins_cost = 0;
                    if (v_prev != -1) {
                        int64_t c1 = inst.C[v_prev * N + u_first];
                        if (c1 >= INF_COST) continue;
                        new_ins_cost += c1;
                    }
                    if (v_next != -1) {
                        int64_t c2 = inst.C[u_last * N + v_next];
                        if (c2 >= INF_COST) continue;
                        new_ins_cost += c2;
                    }

                    int64_t delta = (new_join_cost + new_ins_cost) - (old_rem_cost + old_ins_cost);
                    if (delta < best_delta) {
                        best_delta = delta;
                        best_j = j;
                    }
                }

                if (best_j != -1) {
                    vector<int> block(path.begin() + i, path.begin() + i + blk);
                    path.erase(path.begin() + i, path.begin() + i + blk);
                    path.insert(path.begin() + best_j, block.begin(), block.end());
                    cost += best_delta;
                    improved = true;
                    break;
                }
            }
            if (improved) break;
        }

        if (improved) continue;

        // 2. Precedence-Constrained 2-Opt (segment reversal)
        for (int i = 0; i + 1 < N; ++i) {
            BitMask seg_nodes;
            seg_nodes.set(path[i]);
            for (int j = i + 1; j < N; ++j) {
                int newly_added = path[j];
                // Reversing path[i..j] is precedence-valid iff newly_added has no predecessors in path[i..j-1]
                // (and we already know path[i..j-1] is mutually independent if we reached here!)
                if ((inst.pred[newly_added] & seg_nodes).any()) {
                    break; // Cannot extend reversal further!
                }
                seg_nodes.set(newly_added);

                // Compute cost of reversing path[i..j]
                int64_t old_sub = 0, new_sub = 0;
                if (i > 0) {
                    old_sub += inst.C[path[i - 1] * N + path[i]];
                    new_sub += inst.C[path[i - 1] * N + path[j]];
                }
                if (j + 1 < N) {
                    old_sub += inst.C[path[j] * N + path[j + 1]];
                    new_sub += inst.C[path[i] * N + path[j + 1]];
                }
                if (new_sub >= INF_COST) continue;

                bool valid_rev = true;
                for (int k = i; k < j; ++k) {
                    old_sub += inst.C[path[k] * N + path[k + 1]];
                    int64_t rev_edge = inst.C[path[k + 1] * N + path[k]];
                    if (rev_edge >= INF_COST) {
                        valid_rev = false;
                        break;
                    }
                    new_sub += rev_edge;
                }

                if (valid_rev && new_sub < old_sub) {
                    reverse(path.begin() + i, path.begin() + j + 1);
                    cost += (new_sub - old_sub);
                    improved = true;
                    break;
                }
            }
            if (improved) break;
        }
    }
}

void run_initial_heuristics(const SOPInstance& inst, vector<int>& best_path, int64_t& best_cost, int num_restarts = 64) {
    int N = inst.n;
    best_cost = INF_COST;
    best_path.clear();
    if (inst.trivial_unsat) return;

    mt19937 rng(42);

    for (int iter = 0; iter < num_restarts; ++iter) {
        vector<int> in_deg = inst.in_degree;
        vector<int> ready;
        ready.reserve(N);
        for (int i = 0; i < N; ++i) {
            if (in_deg[i] == 0) ready.push_back(i);
        }

        vector<int> path;
        path.reserve(N);
        bool ok = true;

        while (static_cast<int>(path.size()) < N) {
            if (ready.empty()) {
                ok = false;
                break;
            }
            int chosen_idx = 0;
            if (path.empty()) {
                chosen_idx = iter % ready.size();
            } else {
                int u = path.back();
                // Sort ready candidates by C[u][v]
                int best_idx = 0;
                int second_idx = 0;
                int64_t min_c = INF_COST, second_c = INF_COST;
                for (size_t idx = 0; idx < ready.size(); ++idx) {
                    int v = ready[idx];
                    int64_t c = inst.C[u * N + v];
                    // Tie-break by out_degree (unlocks more nodes)
                    int64_t score = (c >= INF_COST) ? c : (c * 1024 - inst.out_degree[v]);
                    if (score < min_c) {
                        second_c = min_c;
                        second_idx = best_idx;
                        min_c = score;
                        best_idx = idx;
                    } else if (score < second_c) {
                        second_c = score;
                        second_idx = idx;
                    }
                }
                if (iter == 0 || ready.size() == 1 || (rng() % 100) < 75 || second_c >= INF_COST * 512) {
                    chosen_idx = best_idx;
                } else {
                    chosen_idx = second_idx;
                }
            }

            int next_node = ready[chosen_idx];
            ready[chosen_idx] = ready.back();
            ready.pop_back();
            path.push_back(next_node);

            // Unlock successors
            for (int w = 0; w < N; ++w) {
                if (inst.pred[w].test(next_node) && !inst.orig_T.empty()) {
                    // Check if next_node was a direct/transitive prerequisite
                    if (--in_deg[w] == 0) {
                        ready.push_back(w);
                    }
                }
            }
        }

        if (ok) {
            int64_t c = compute_path_cost(inst, path);
            if (c < INF_COST) {
                sop_local_search(inst, path, c);
                if (c < best_cost) {
                    best_cost = c;
                    best_path = path;
                }
            }
        }
    }
}

// ============================================================================
// Dynamic Dual Ascent / Assignment Relaxation Lower Bound
// ============================================================================
struct DualBound {
    int64_t base_lb = 0;
    vector<int64_t> u_out; // Dual weight for outgoing from i
    vector<int64_t> v_in;  // Dual weight for incoming to j
};

DualBound compute_root_dual_bound(const SOPInstance& inst) {
    int N = inst.n;
    DualBound db;
    db.u_out.assign(N, 0);
    db.v_in.assign(N, 0);

    // Every node except the last must have an outgoing edge.
    // Every node except the first must have an incoming edge.
    // Standard two-phase dual ascent on C[i][j]:
    for (int i = 0; i < N; ++i) {
        if (inst.out_degree[i] == 0) {
            // Potential sink node: may not have an outgoing edge
            db.u_out[i] = 0;
        } else {
            db.u_out[i] = inst.min_out[i] < INF_COST ? inst.min_out[i] : 0;
        }
    }

    for (int j = 0; j < N; ++j) {
        if (inst.in_degree[j] == 0) {
            db.v_in[j] = 0;
        } else {
            int64_t min_red = INF_COST;
            for (int i : inst.adj_in[j]) {
                min_red = min(min_red, inst.C[i * N + j] - db.u_out[i]);
            }
            db.v_in[j] = (min_red < INF_COST) ? min_red : 0;
        }
    }

    return db;
}

// ============================================================================
// Lock-Free / Low-Contention Parallel Search Task & History Table
// ============================================================================
struct SearchTask {
    vector<int> path;
    vector<int> in_deg;
    vector<int> ready;
    BitMask visited;
    int64_t g_cost = 0;
    int64_t rem_out_lb = 0;
    int64_t rem_in_lb = 0;
    int64_t f_bound = 0;

    bool operator>(const SearchTask& other) const {
        if (f_bound != other.f_bound) return f_bound > other.f_bound;
        return path.size() < other.path.size();
    }
};

// Thread-local Dominance / History Table (Avoids cross-NUMA cache coherence traffic)
struct DominanceTable {
    static constexpr size_t TABLE_SIZE = 1 << 20; // 1M entries (~32MB per thread)
    static constexpr size_t MASK = TABLE_SIZE - 1;

    struct Entry {
        uint64_t hash_key = 0;
        int last_node = -1;
        int64_t min_cost = INF_COST;
    };

    vector<Entry> table;
    vector<uint64_t> zobrist;

    DominanceTable(int N) : table(TABLE_SIZE) {
        mt19937_64 rng(1337);
        zobrist.resize(N);
        for (int i = 0; i < N; ++i) zobrist[i] = rng();
    }

    inline bool check_and_update(uint64_t set_hash, int last_node, int64_t cost) {
        uint64_t combined = set_hash ^ (static_cast<uint64_t>(last_node + 1) * 0x9e3779b97f4a7c15ULL);
        size_t idx = combined & MASK;
        Entry& e = table[idx];
        if (e.hash_key == set_hash && e.last_node == last_node) {
            if (e.min_cost <= cost) return false; // Dominated!
            e.min_cost = cost;
            return true;
        }
        // Always replace with newer state
        e.hash_key = set_hash;
        e.last_node = last_node;
        e.min_cost = cost;
        return true;
    }
};

// ============================================================================
// NUMA-Aware Thread Pinning for AMD Threadripper 2990WX
// ============================================================================
void pin_thread_numa_aware(int thread_id, int total_threads) {
#ifdef __linux__
    int num_cpus = thread::hardware_concurrency();
    if (num_cpus <= 0) return;

    // On Threadripper 2990WX (32 cores / 64 threads across 4 dies),
    // physical cores are 0..31 and SMT siblings are 32..63.
    // Spreading threads across physical cores first maximizes memory bandwidth across all 4 dies.
    int target_cpu = thread_id % num_cpus;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(target_cpu, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#else
    (void)thread_id;
    (void)total_threads;
#endif
}

// ============================================================================
// Parallel Branch-and-Bound Solver Engine
// ============================================================================
class ParallelSOPSolver {
public:
    const SOPInstance& global_inst;
    DualBound global_db;
    Options opts;

    atomic<int64_t> global_best_cost;
    vector<int> global_best_path;
    mutex best_mutex;

    atomic<bool> timeout_flag{false};
    chrono::steady_clock::time_point start_time;

    // Work queue for initial parallel frontier distribution and dynamic work stealing
    vector<SearchTask> shared_tasks;
    atomic<size_t> task_idx{0};
    mutex queue_mutex;

    vector<uint64_t> thread_nodes;

    ParallelSOPSolver(const SOPInstance& inst, const Options& o, chrono::steady_clock::time_point st)
        : global_inst(inst), opts(o), start_time(st) {
        global_best_cost.store(INF_COST, memory_order_relaxed);
        thread_nodes.assign(opts.threads, 0);
        global_db = compute_root_dual_bound(inst);
    }

    inline bool is_timed_out(uint64_t local_nodes) {
        if (timeout_flag.load(memory_order_relaxed)) return true;
        if (opts.time_limit > 0.0 && (local_nodes & 0xFFF) == 0) {
            double elapsed = chrono::duration<double>(chrono::steady_clock::now() - start_time).count();
            if (elapsed >= opts.time_limit) {
                timeout_flag.store(true, memory_order_relaxed);
                return true;
            }
        }
        return false;
    }

    void update_best(int64_t cost, const vector<int>& path) {
        int64_t cur = global_best_cost.load(memory_order_relaxed);
        if (cost < cur) {
            lock_guard<mutex> lock(best_mutex);
            if (cost < global_best_cost.load(memory_order_relaxed)) {
                global_best_cost.store(cost, memory_order_relaxed);
                global_best_path = path;
                double elapsed = chrono::duration<double>(chrono::steady_clock::now() - start_time).count();
                cerr << "[SOLVER] New Incumbent Cost = " << cost << " at " << fixed << setprecision(3) << elapsed << "s\n";
            }
        }
    }

    // Dynamic Subproblem Lower Bound (Combines Outgoing Min-Edge + Incoming Residual Duals)
    static inline int64_t compute_dynamic_lb(
        const SOPInstance& inst,
        int u,
        int64_t g_cost,
        int64_t rem_out_lb,
        int64_t rem_in_lb,
        const BitMask& visited
    ) {
        // u's outgoing contribution is still in rem_out_lb;
        // Every unvisited node v has its incoming contribution in rem_in_lb.
        return g_cost + max(rem_out_lb, rem_in_lb);
    }

    // Tighter active-subgraph 1-tree / assignment bound for mid-depth nodes
    static inline int64_t compute_tight_active_lb(
        const SOPInstance& inst,
        int u,
        int64_t g_cost,
        const BitMask& visited,
        const vector<int>& in_deg
    ) {
        int N = inst.n;
        int64_t sum_in = 0;
        int64_t sum_out = 0;

        // Outgoing from current head u (must go to a ready unvisited node)
        int64_t min_u_out = INF_COST;
        for (int v : inst.adj_out[u]) {
            if (!visited.test(v) && in_deg[v] == 0) {
                min_u_out = inst.C[u * N + v];
                break;
            }
        }
        if (min_u_out >= INF_COST) return INF_COST;
        sum_out += min_u_out;

        // For each unvisited node v:
        for (int v = 0; v < N; ++v) {
            if (visited.test(v)) continue;

            // Incoming to v must come from either u (if v is ready) or another unvisited node w
            int64_t min_v_in = INF_COST;
            for (int w : inst.adj_in[v]) {
                if (w == u) {
                    if (in_deg[v] == 0) {
                        min_v_in = inst.C[w * N + v];
                        break;
                    }
                } else if (!visited.test(w)) {
                    min_v_in = inst.C[w * N + v];
                    break;
                }
            }
            if (min_v_in >= INF_COST) return INF_COST;
            sum_in += min_v_in;

            // Outgoing from v (if v is not a potential sink)
            if (inst.out_degree[v] > 0) {
                int64_t min_v_out = INF_COST;
                for (int w : inst.adj_out[v]) {
                    if (!visited.test(w)) {
                        min_v_out = inst.C[v * N + w];
                        break;
                    }
                }
                if (min_v_out >= INF_COST) return INF_COST;
                sum_out += min_v_out;
            }
        }

        // Also combine with dual residual bound:
        return g_cost + max(sum_in, sum_out);
    }

    // Generate initial frontier via BFS expansion to feed all 32 threads evenly
    void build_initial_frontier(SearchTask root, size_t target_size) {
        int N = global_inst.n;
        vector<SearchTask> current_level = {root};

        while (!current_level.empty() && current_level.size() < target_size) {
            if (is_timed_out(0)) break;
            vector<SearchTask> next_level;
            for (const auto& task : current_level) {
                thread_nodes[0]++;
                if (static_cast<int>(task.path.size()) == N) {
                    update_best(task.g_cost, task.path);
                    continue;
                }

                int u = task.path.empty() ? -1 : task.path.back();
                int64_t best_ub = global_best_cost.load(memory_order_relaxed);

                // Generate children from ready nodes
                struct Cand { int v; int64_t edge_c; int64_t lb; };
                vector<Cand> cands;
                cands.reserve(task.ready.size());

                for (int v : task.ready) {
                    int64_t edge_c = (u == -1) ? 0 : global_inst.C[u * N + v];
                    if (edge_c >= INF_COST) continue;

                    int64_t new_g = task.g_cost + edge_c;
                    int64_t new_rem_out = task.rem_out_lb - (u == -1 ? 0 : global_db.u_out[u]);
                    int64_t new_rem_in  = task.rem_in_lb  - global_db.v_in[v];
                    int64_t fast_lb = new_g + max(new_rem_out, new_rem_in);
                    if (fast_lb >= best_ub) continue;

                    cands.push_back({v, edge_c, fast_lb});
                }

                sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
                    return a.lb < b.lb;
                });

                for (const auto& cand : cands) {
                    int v = cand.v;
                    SearchTask child = task;
                    child.path.push_back(v);
                    child.visited.set(v);
                    child.g_cost += cand.edge_c;
                    if (u != -1) child.rem_out_lb -= global_db.u_out[u];
                    child.rem_in_lb -= global_db.v_in[v];

                    // Remove v from ready and unlock successors
                    child.ready.erase(find(child.ready.begin(), child.ready.end(), v));
                    for (int w = 0; w < N; ++w) {
                        if (!child.visited.test(w) && global_inst.pred[w].test(v)) {
                            if (--child.in_deg[w] == 0) {
                                child.ready.push_back(w);
                            }
                        }
                    }

                    int64_t tight_lb = (static_cast<int>(child.path.size()) < N)
                        ? compute_tight_active_lb(global_inst, v, child.g_cost, child.visited, child.in_deg)
                        : child.g_cost;
                    child.f_bound = max(cand.lb, tight_lb);
                    if (child.f_bound < best_ub) {
                        next_level.push_back(move(child));
                    }
                }
            }
            if (next_level.empty()) {
                current_level.clear();
                break;
            }
            current_level = move(next_level);
        }

        // Sort frontier so best-bound tasks are explored first across all threads
        sort(current_level.begin(), current_level.end(), [](const SearchTask& a, const SearchTask& b) {
            return a.f_bound < b.f_bound;
        });
        shared_tasks = move(current_level);
    }

    // Worker thread execution (maintains thread-local copy of SOPInstance for NUMA locality!)
    void worker_body(int tid) {
        pin_thread_numa_aware(tid, opts.threads);

        // Replicate read-only SOPInstance & DualBound in thread-local memory (allocated on local NUMA node)
        SOPInstance local_inst = global_inst;
        DualBound local_db = global_db;
        int N = local_inst.n;

        DominanceTable dom_table(N);
        uint64_t local_nodes = 0;

        // Thread-local DFS state
        vector<int> path;
        vector<int> in_deg;
        BitMask visited;
        uint64_t set_hash = 0;

        auto dfs = [&](auto& self, int u, int64_t g_cost, int64_t rem_out_lb, int64_t rem_in_lb, vector<int>& ready) -> void {
            if (is_timed_out(local_nodes)) return;
            local_nodes++;

            int depth = static_cast<int>(path.size());
            if (depth == N) {
                update_best(g_cost, path);
                return;
            }

            int64_t best_ub = global_best_cost.load(memory_order_relaxed);
            if (g_cost + max(rem_out_lb, rem_in_lb) >= best_ub) return;

            // Dominance check on prefix set + last node u
            if (!dom_table.check_and_update(set_hash, u, g_cost)) {
                return;
            }

            // Tight active-subgraph lower bound check
            int64_t tight_lb = compute_tight_active_lb(local_inst, u, g_cost, visited, in_deg);
            if (tight_lb >= best_ub) return;

            // Gather and sort candidate transitions
            struct Move { int v; int64_t edge_c; int64_t est; };
            Move moves[MAX_N];
            int num_moves = 0;

            int64_t next_rem_out = rem_out_lb - local_db.u_out[u];

            for (int v : ready) {
                int64_t edge_c = local_inst.C[u * N + v];
                if (edge_c >= INF_COST) continue;
                int64_t new_g = g_cost + edge_c;
                int64_t new_rem_in = rem_in_lb - local_db.v_in[v];
                int64_t fast_lb = new_g + max(next_rem_out, new_rem_in);
                if (fast_lb < best_ub) {
                    moves[num_moves++] = {v, edge_c, fast_lb};
                }
            }

            if (num_moves == 0) return;

            // Sort moves by estimated lower bound (and tie-break by out_degree)
            sort(moves, moves + num_moves, [&](const Move& a, const Move& b) {
                if (a.est != b.est) return a.est < b.est;
                return local_inst.out_degree[a.v] > local_inst.out_degree[b.v];
            });

            for (int m = 0; m < num_moves; ++m) {
                if (timeout_flag.load(memory_order_relaxed)) break;
                int64_t cur_ub = global_best_cost.load(memory_order_relaxed);
                if (moves[m].est >= cur_ub) continue;

                int v = moves[m].v;
                int64_t edge_c = moves[m].edge_c;

                // Apply move
                path.push_back(v);
                visited.set(v);
                set_hash ^= dom_table.zobrist[v];

                vector<int> next_ready;
                next_ready.reserve(ready.size() + 8);
                for (int r : ready) {
                    if (r != v) next_ready.push_back(r);
                }

                int unlocked[MAX_N];
                int num_unlocked = 0;
                for (int w = 0; w < N; ++w) {
                    if (!visited.test(w) && local_inst.pred[w].test(v)) {
                        if (--in_deg[w] == 0) {
                            next_ready.push_back(w);
                            unlocked[num_unlocked++] = w;
                        }
                    }
                }

                self(self, v, g_cost + edge_c, next_rem_out, rem_in_lb - local_db.v_in[v], next_ready);

                // Undo move
                for (int k = 0; k < num_unlocked; ++k) {
                    in_deg[unlocked[k]]++;
                }
                set_hash ^= dom_table.zobrist[v];
                visited.reset(v);
                path.pop_back();
            }
        };

        // Pull tasks dynamically from shared frontier
        while (!timeout_flag.load(memory_order_relaxed)) {
            size_t idx = task_idx.fetch_add(1, memory_order_relaxed);
            if (idx >= shared_tasks.size()) break;

            const SearchTask& task = shared_tasks[idx];
            int64_t cur_ub = global_best_cost.load(memory_order_relaxed);
            if (task.f_bound >= cur_ub) {
                local_nodes++;
                continue;
            }

            path = task.path;
            in_deg = task.in_deg;
            visited = task.visited;
            set_hash = 0;
            for (int node : path) set_hash ^= dom_table.zobrist[node];

            vector<int> ready = task.ready;
            int u = path.back();

            dfs(dfs, u, task.g_cost, task.rem_out_lb, task.rem_in_lb, ready);
        }

        thread_nodes[tid] += local_nodes;
    }

    void solve() {
        if (global_inst.trivial_unsat) return;
        int N = global_inst.n;

        if (N == 1) {
            update_best(0, {0});
            thread_nodes[0] = 1;
            return;
        }

        // Build root task
        SearchTask root;
        root.in_deg = global_inst.in_degree;
        for (int i = 0; i < N; ++i) {
            if (root.in_deg[i] == 0) root.ready.push_back(i);
            root.rem_out_lb += global_db.u_out[i];
            root.rem_in_lb  += global_db.v_in[i];
        }
        root.g_cost = 0;
        root.f_bound = max(root.rem_out_lb, root.rem_in_lb);

        // Expand frontier to at least 64 * threads tasks for smooth load balancing across all 32 cores
        size_t target_frontier = (opts.threads == 1) ? 1 : static_cast<size_t>(opts.threads * 64);
        build_initial_frontier(root, target_frontier);

        if (shared_tasks.empty() || timeout_flag.load()) return;

        if (opts.threads == 1) {
            worker_body(0);
        } else {
            vector<thread> workers;
            workers.reserve(opts.threads);
            for (int t = 0; t < opts.threads; ++t) {
                workers.emplace_back(&ParallelSOPSolver::worker_body, this, t);
            }
            for (auto& w : workers) {
                if (w.joinable()) w.join();
            }
        }
    }
};

// ============================================================================
// Main Entry Point & CSV Formatter
// ============================================================================
int main(int argc, char** argv) {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    Options opts;
    if (!parse_args(argc, argv, opts)) {
        print_usage(argv[0]);
        return 1;
    }

    auto start_time = chrono::steady_clock::now();

    SOPInstance inst;
    if (!parse_sop_file(opts.filepath, inst)) {
        return 1;
    }

    cerr << "[INFO] Loaded instance '" << inst.name << "' (N = " << inst.n << "), using " << opts.threads << " thread(s).\n";

    preprocess_instance(inst);

    vector<int> init_path;
    int64_t init_cost = INF_COST;
    if (!inst.trivial_unsat) {
        int num_restarts = (inst.n <= 100) ? 128 : 32;
        run_initial_heuristics(inst, init_path, init_cost, num_restarts);
    }

    ParallelSOPSolver solver(inst, opts, start_time);
    if (init_cost < INF_COST) {
        solver.update_best(init_cost, init_path);
    }

    solver.solve();

    auto end_time = chrono::steady_clock::now();
    double elapsed_sec = chrono::duration<double>(end_time - start_time).count();

    uint64_t total_enumerated_nodes = 0;
    for (uint64_t cnt : solver.thread_nodes) {
        total_enumerated_nodes += cnt;
    }

    bool feasible = (solver.global_best_cost.load() < INF_COST && !solver.global_best_path.empty());
    bool proven_optimal = feasible && !solver.timeout_flag.load();

    int64_t final_cost = feasible ? solver.global_best_cost.load() : -1;
    string path_str = "";

    if (feasible) {
        if (!validate_solution(inst, solver.global_best_path, final_cost)) {
            cerr << "[CRITICAL] Solution validation failed!\n";
            return 2;
        }
        cerr << "[INFO] Solution validated successfully (Cost = " << final_cost
             << ", Optimal = " << (proven_optimal ? "yes" : "no") << ").\n";

        ostringstream oss;
        for (size_t i = 0; i < solver.global_best_path.size(); ++i) {
            if (i > 0) oss << " ";
            oss << solver.global_best_path[i];
        }
        path_str = oss.str();
    } else {
        proven_optimal = false;
        cerr << "[INFO] No feasible solution found.\n";
    }

    string header_row = "Instance,Instance Size,Time,Enumerated Nodes,Best Cost,Best Path,Optimal\n";
    ostringstream data_row;
    data_row << inst.name << ","
             << inst.n << ","
             << fixed << setprecision(4) << elapsed_sec << ","
             << total_enumerated_nodes << ","
             << final_cost << ","
             << path_str << ","
             << (proven_optimal ? "yes" : "no") << "\n";

    // Output to stdout
    if (!opts.no_header) {
        cout << header_row;
    }
    cout << data_row.str();
    cout.flush();

    // Output to CSV file if requested
    if (!opts.output_csv.empty()) {
        bool write_csv_header = !opts.no_header;
        if (write_csv_header && filesystem::exists(opts.output_csv)) {
            if (filesystem::file_size(opts.output_csv) > 0) {
                write_csv_header = false;
            }
        }
        ofstream csv_out(opts.output_csv, ios::app);
        if (!csv_out.is_open()) {
            cerr << "[ERROR] Could not open output CSV file: " << opts.output_csv << "\n";
            return 1;
        }
        if (write_csv_header) {
            csv_out << header_row;
        }
        csv_out << data_row.str();
        csv_out.close();
    }

    return 0;
}