/**
 * State-of-the-Art Parallel Branch-and-Bound Solver for the Sequential Ordering Problem (SOP)
 * Optimized for 32-Core AMD Ryzen Threadripper 2990WX (WX-series 4-Die NUMA Topology)
 *
 * Implements techniques from:
 * 1. Shobaki & Jamal (2015) - Bidirectional Edge DLBs, History Utilization
 * 2. Gonggiatgul, Shobaki, & Muyan-Ozelik (2023) - Pool-based Parallel B&B, Parallel History
 *    Domination with Thread Stop, Block Memory Allocators, Density-aware Work Stealing, Thread Restart
 * 3. Shobaki et al. (ICPP-W 2023) - Hybrid Cooperative Heuristic + Exact B&B Search
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <algorithm>
#include <chrono>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <cmath>
#include <iomanip>
#include <cstring>
#include <random>
#include <memory>
#include <filesystem>
#include <limits>
#include <sched.h>
#include <pthread.h>

using namespace std;

// Constants
constexpr int64_t INF = 1000000000000000LL;
constexpr int MAX_N = 1024;

// ============================================================================
// Bitset for fast N <= 1024 set operations & Zobrist hashing
// ============================================================================
struct Bitset1024 {
    uint64_t words[16];

    Bitset1024() { memset(words, 0, sizeof(words)); }

    inline void set(int b) { words[b >> 6] |= (1ULL << (b & 63)); }
    inline void reset(int b) { words[b >> 6] &= ~(1ULL << (b & 63)); }
    inline bool test(int b) const { return (words[b >> 6] >> (b & 63)) & 1ULL; }

    inline bool is_subset_of(const Bitset1024& other, int num_words) const {
        for (int i = 0; i < num_words; ++i) {
            if ((words[i] & ~other.words[i]) != 0) return false;
        }
        return true;
    }

    inline bool intersects(const Bitset1024& other, int num_words) const {
        for (int i = 0; i < num_words; ++i) {
            if ((words[i] & other.words[i]) != 0) return true;
        }
        return false;
    }

    inline bool operator==(const Bitset1024& o) const {
        return memcmp(words, o.words, sizeof(words)) == 0;
    }
};

// ============================================================================
// Problem Instance Representation & Preprocessing
// ============================================================================
struct AdjEdge {
    int to;
    int64_t weight;
};

struct SOPInstance {
    string name;
    string filepath;
    int n = 0;
    int num_words = 0;
    vector<vector<int64_t>> cost;

    // Precedence bitsets (Transitive Closure)
    vector<Bitset1024> pred_bits; // pred_bits[i] has bit j set if j must precede i
    vector<Bitset1024> succ_bits; // succ_bits[i] has bit j set if i must precede j
    vector<int> in_degree;
    vector<int> out_degree;
    vector<vector<int>> imm_succ; // Immediate successors in transitive reduction
    vector<vector<int>> imm_pred;

    // Sorted valid outgoing and incoming edges per node for O(1) amortized Edge DLB
    vector<vector<AdjEdge>> sorted_out;
    vector<vector<AdjEdge>> sorted_in;

    vector<int> roots;
    vector<int> leaves;
    double precedence_density = 0.0;

    // Zobrist Hash Keys
    vector<uint64_t> zobrist_node;
    vector<uint64_t> zobrist_last;

    bool parse(const string& path) {
        filepath = path;
        name = filesystem::path(path).filename().string();
        ifstream in(path);
        if (!in.is_open()) return false;

        string line;
        bool in_matrix = false;
        while (getline(in, line)) {
            // Trim whitespace
            size_t first = line.find_first_not_of(" \t\r\n");
            if (first == string::npos) continue;
            string trimmed = line.substr(first);

            if (trimmed.rfind("EDGE_WEIGHT_SECTION", 0) == 0) {
                in_matrix = true;
                break;
            }
            if (trimmed.rfind("DIMENSION", 0) == 0) {
                size_t colon = trimmed.find(':');
                string val_str = (colon != string::npos) ? trimmed.substr(colon + 1) : trimmed.substr(9);
                stringstream ss(val_str);
                ss >> n;
            }
        }

        if (!in_matrix && n <= 0) return false;

        // Read matrix tokens
        vector<int64_t> tokens;
        tokens.reserve(n > 0 ? n * n + 2 : 10000);
        int64_t val;
        while (in >> val) {
            tokens.push_back(val);
            if (n > 0 && (int)tokens.size() == n * n + 1) break;
        }

        if (tokens.empty()) return false;
        if (n <= 0) {
            n = (int) tokens[0];
        }

        size_t offset = 0;
        if ((int)tokens.size() >= n * n + 1 && tokens[0] == n) {
            offset = 1;
        } else if ((int)tokens.size() < n * n) {
            return false;
        }

        if (n > MAX_N) {
            cerr << "[Error] Dimension N=" << n << " exceeds compiled MAX_N=" << MAX_N << "\n";
            return false;
        }

        num_words = (n + 63) / 64;
        cost.assign(n, vector<int64_t>(n, 0));
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                cost[i][j] = tokens[offset + i * n + j];
            }
        }

        preprocess();
        return true;
    }

    void preprocess() {
        pred_bits.assign(n, Bitset1024());
        succ_bits.assign(n, Bitset1024());

        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                if (i != j && cost[i][j] < 0) {
                    // T[i][j] < 0 means i depends on j -> j must come before i
                    pred_bits[i].set(j);
                    succ_bits[j].set(i);
                }
            }
        }

        // Floyd-Warshall Transitive Closure using Bitsets
        for (int k = 0; k < n; ++k) {
            for (int i = 0; i < n; ++i) {
                if (pred_bits[i].test(k)) {
                    for (int w = 0; w < num_words; ++w) {
                        pred_bits[i].words[w] |= pred_bits[k].words[w];
                    }
                }
            }
        }
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                if (pred_bits[i].test(j)) {
                    succ_bits[j].set(i);
                    cost[i][j] = -1; // Enforce precedence in matrix
                }
            }
        }

        // Compute transitive reduction (immediate predecessors/successors) & degrees
        in_degree.assign(n, 0);
        out_degree.assign(n, 0);
        imm_succ.assign(n, {});
        imm_pred.assign(n, {});

        int total_relations = 0;
        for (int u = 0; u < n; ++u) {
            for (int v = 0; v < n; ++v) {
                if (u != v && succ_bits[u].test(v)) {
                    total_relations++;
                    // Check if there is an intermediate node w such that u -> w -> v
                    if (!succ_bits[u].intersects(pred_bits[v], num_words)) {
                        imm_succ[u].push_back(v);
                        imm_pred[v].push_back(u);
                        in_degree[v]++;
                        out_degree[u]++;
                    }
                }
            }
        }

        for (int i = 0; i < n; ++i) {
            if (in_degree[i] == 0) roots.push_back(i);
            if (out_degree[i] == 0) leaves.push_back(i);
        }

        precedence_density = (n > 1) ? (double)total_relations / (double)(n * (n - 1) / 2.0) : 0.0;

        // Build sorted valid outgoing and incoming edges per node
        // An edge (u, v) is valid only if:
        // 1. u != v
        // 2. v does not precede u (!pred_bits[u].test(v))
        // 3. There is no intermediate node w with u -> w -> v
        // 4. u is not a forced unique end node (unless n == 1), v is not a forced unique start node
        sorted_out.assign(n, {});
        sorted_in.assign(n, {});
        for (int u = 0; u < n; ++u) {
            for (int v = 0; v < n; ++v) {
                if (u == v) continue;
                if (cost[u][v] < 0) continue;
                if (pred_bits[u].test(v)) continue;
                if (succ_bits[u].intersects(pred_bits[v], num_words)) continue;
                // Furthermore, v can immediately follow u ONLY IF all predecessors of v
                // are not successors of u! If u -> w and w -> v, caught above.
                sorted_out[u].push_back({v, cost[u][v]});
                sorted_in[v].push_back({u, cost[u][v]});
            }
            sort(sorted_out[u].begin(), sorted_out[u].end(), [](const AdjEdge& a, const AdjEdge& b) {
                return a.weight < b.weight;
            });
            sort(sorted_in[u].begin(), sorted_in[u].end(), [](const AdjEdge& a, const AdjEdge& b) {
                return a.weight < b.weight;
            });
        }

        // Initialize Zobrist hashing
        mt19937_64 rng(0x9E3779B97F4A7C15ULL);
        zobrist_node.resize(n);
        zobrist_last.resize(n);
        for (int i = 0; i < n; ++i) {
            zobrist_node[i] = rng();
            zobrist_last[i] = rng();
        }
    }
};

// ============================================================================
// NUMA-Aware Thread Affinity & Per-Thread Block Memory Allocator
// ============================================================================
void pin_thread_to_core(int thread_idx, int total_threads) {
#ifdef __linux__
    unsigned int num_cpus = thread::hardware_concurrency();
    if (num_cpus == 0) num_cpus = 32;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    // Spread across physical cores / NUMA dies smoothly
    int target_cpu = thread_idx % num_cpus;
    CPU_SET(target_cpu, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#endif
}

struct HistoryNode {
    Bitset1024 visited;
    int last_vertex;
    atomic<int64_t> prefix_cost;
    atomic<int64_t> lower_bound; // Best known lower bound on total cost (or suffix)
    atomic<int> active_tid;      // -1 if explored, >= 0 if currently being explored by a thread
    HistoryNode* next;           // Intrusive linked list for hash bucket
};

// Thread-local block allocator (Paper 3 Section 5.3) to avoid lock contention & syscalls
class ThreadBlockAllocator {
    static constexpr size_t BLOCK_SIZE = 4096;
    vector<HistoryNode*> blocks;
    size_t current_idx = BLOCK_SIZE;
    size_t total_allocated = 0;

public:
    ThreadBlockAllocator() = default;
    ~ThreadBlockAllocator() {
        for (auto* b : blocks) delete[] b;
    }

    HistoryNode* allocate() {
        if (current_idx >= BLOCK_SIZE) {
            blocks.push_back(new HistoryNode[BLOCK_SIZE]);
            current_idx = 0;
        }
        total_allocated++;
        return &blocks.back()[current_idx++];
    }

    size_t bytes_used() const {
        return blocks.size() * BLOCK_SIZE * sizeof(HistoryNode);
    }
};

// ============================================================================
// Concurrent Lock-Striped History Table with Stop-Request Buffer (Paper 3 Sec 5)
// ============================================================================
struct StopRequest {
    atomic<uint64_t> hash_key{0};
    atomic<int64_t> new_prefix_cost{INF};
    atomic<int> target_tid{-1};
};

class ParallelHistoryTable {
    size_t num_buckets;
    size_t num_locks;
    vector<HistoryNode*> buckets;
    vector<mutex> locks;
    int num_words;
    atomic<size_t> total_entries{0};
    size_t max_entries;

public:
    static constexpr int STOP_BUF_SIZE = 128;
    StopRequest stop_buffer[STOP_BUF_SIZE];
    atomic<int> stop_write_idx{0};

    ParallelHistoryTable(size_t buckets_pow2, int words, size_t max_mem_bytes = 8ULL * 1024 * 1024 * 1024)
        : num_buckets(buckets_pow2), num_locks(buckets_pow2 / 16),
          buckets(buckets_pow2, nullptr), locks(buckets_pow2 / 16), num_words(words) {
        max_entries = max_mem_bytes / sizeof(HistoryNode);
    }

    inline size_t get_lock_idx(size_t bucket_idx) const {
        return bucket_idx & (num_locks - 1);
    }

    bool memory_80_percent_full() const {
        return total_entries.load(memory_order_relaxed) >= (max_entries * 8) / 10;
    }

    bool memory_full() const {
        return total_entries.load(memory_order_relaxed) >= max_entries;
    }

    void post_stop_request(uint64_t hash_key, int64_t better_prefix_cost, int active_tid) {
        int idx = stop_write_idx.fetch_add(1, memory_order_relaxed) & (STOP_BUF_SIZE - 1);
        stop_buffer[idx].hash_key.store(hash_key, memory_order_relaxed);
        stop_buffer[idx].new_prefix_cost.store(better_prefix_cost, memory_order_relaxed);
        stop_buffer[idx].target_tid.store(active_tid, memory_order_release);
    }

    // Returns true if current node must be explored, false if dominated/pruned
    bool check_and_insert(uint64_t hash_key, const Bitset1024& visited, int last_v,
                          int64_t prefix_cost, int64_t current_lb, int64_t best_cost,
                          int tid, int depth, int n, ThreadBlockAllocator& alloc,
                          HistoryNode*& out_node) {
        out_node = nullptr;
        size_t b_idx = hash_key & (num_buckets - 1);
        size_t l_idx = get_lock_idx(b_idx);

        lock_guard<mutex> guard(locks[l_idx]);
        HistoryNode* curr = buckets[b_idx];
        while (curr != nullptr) {
            if (curr->last_vertex == last_v &&
                memcmp(curr->visited.words, visited.words, num_words * sizeof(uint64_t)) == 0) {
                // Found similar node! Apply History Utilization & Parallel Domination (Paper 1 Alg 3 & Paper 3 Sec 5.1)
                int64_t hist_prefix = curr->prefix_cost.load(memory_order_relaxed);
                int64_t hist_lb = curr->lower_bound.load(memory_order_relaxed);

                if (prefix_cost >= hist_prefix) {
                    return false; // Dominated by history node! Prune immediately.
                }

                // Current node has strictly smaller prefix cost than history node
                int64_t imp = hist_prefix - prefix_cost;
                int prev_tid = curr->active_tid.load(memory_order_relaxed);

                if (prev_tid == -1 && imp <= hist_lb - best_cost) {
                    // Even with the prefix improvement, cannot beat global best_cost
                    return false;
                }

                // Update history entry with new dominant prefix cost
                curr->prefix_cost.store(prefix_cost, memory_order_relaxed);
                if (hist_lb - imp > current_lb) {
                    curr->lower_bound.store(hist_lb - imp, memory_order_relaxed);
                } else {
                    curr->lower_bound.store(current_lb, memory_order_relaxed);
                }

                if (prev_tid >= 0 && prev_tid != tid) {
                    // Another thread is actively exploring the dominated node -> send stop signal!
                    post_stop_request(hash_key, prefix_cost, prev_tid);
                }
                curr->active_tid.store(tid, memory_order_relaxed);
                out_node = curr;
                return true;
            }
            curr = curr->next;
        }

        // Not found in history table. Insert if memory policy permits (Paper 3 Sec 5.2)
        if (!memory_full() && (!memory_80_percent_full() || depth <= (n * 3) / 4)) {
            HistoryNode* node = alloc.allocate();
            node->visited = visited;
            node->last_vertex = last_v;
            node->prefix_cost.store(prefix_cost, memory_order_relaxed);
            node->lower_bound.store(current_lb, memory_order_relaxed);
            node->active_tid.store(tid, memory_order_relaxed);
            node->next = buckets[b_idx];
            buckets[b_idx] = node;
            total_entries.fetch_add(1, memory_order_relaxed);
            out_node = node;
        }
        return true;
    }
};

// ============================================================================
// Assignment / Hungarian Algorithm (MCPM Relaxation Lower Bound - Paper 2 & 3)
// ============================================================================
struct HungarianSolver {
    int n;
    vector<int64_t> u, v;
    vector<int> p, way;
    vector<int64_t> minv;
    vector<bool> used;

    HungarianSolver(int n_nodes) : n(n_nodes), u(n_nodes + 1, 0), v(n_nodes + 1, 0),
                                   p(n_nodes + 1, 0), way(n_nodes + 1, 0),
                                   minv(n_nodes + 1, 0), used(n_nodes + 1, false) {}

    // Computes MCPM lower bound on the remaining unvisited subgraph + linking from last_v
    int64_t solve_subproblem(const SOPInstance& inst, const vector<int>& rem_out, const vector<int>& rem_in) {
        int m = (int)rem_out.size();
        if (m == 0) return 0;

        fill(u.begin(), u.begin() + m + 1, 0);
        fill(v.begin(), v.begin() + m + 1, 0);
        fill(p.begin(), p.begin() + m + 1, 0);

        for (int i = 1; i <= m; ++i) {
            p[0] = i;
            int j0 = 0;
            fill(minv.begin(), minv.begin() + m + 1, INF);
            fill(used.begin(), used.begin() + m + 1, false);
            int u_node = rem_out[i - 1];

            do {
                used[j0] = true;
                int i0 = p[j0];
                int64_t delta = INF;
                int j1 = 0;
                int row_node = (i0 == 0) ? u_node : rem_out[i0 - 1];

                for (int j = 1; j <= m; ++j) {
                    if (!used[j]) {
                        int col_node = rem_in[j - 1];
                        int64_t c = INF;
                        if (row_node == -1 || col_node == -1) {
                            // Dummy sink->source return edge has cost 0
                            c = 0;
                        } else if (row_node != col_node && inst.cost[row_node][col_node] >= 0 &&
                                   !inst.succ_bits[row_node].intersects(inst.pred_bits[col_node], inst.num_words)) {
                            c = inst.cost[row_node][col_node];
                        }
                        int64_t cur = c - u[i0] - v[j];
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
                for (int j = 0; j <= m; ++j) {
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
        return -v[0];
    }
};

// ============================================================================
// Bidirectional Edge-Based Lower Bound Calculator (Paper 1 Sec 5.2 & 5.4)
// ============================================================================
inline int64_t compute_bidirectional_edge_dlb(
    const SOPInstance& inst,
    const Bitset1024& visited,
    int last_v,
    int first_v,
    int64_t prefix_cost,
    int& out_last_moew)
{
    int n = inst.n;
    int64_t sum_out = 0;
    int64_t max_leaf_out = 0;
    out_last_moew = 0;

    for (int u = 0; u < n; ++u) {
        if (visited.test(u) && u != last_v) continue; // Outgoing edge already fixed in prefix
        int64_t moew = INF;
        for (const auto& e : inst.sorted_out[u]) {
            if (!visited.test(e.to)) {
                moew = e.weight;
                break;
            }
        }
        if (moew == INF) {
            if (inst.out_degree[u] == 0) {
                moew = 0; // Leaf node can terminate the path
            } else {
                return INF; // Dead end
            }
        }
        if (u == last_v) out_last_moew = (int)moew;
        sum_out += moew;
        if (inst.out_degree[u] == 0 && moew > max_leaf_out) {
            max_leaf_out = moew;
        }
    }
    int64_t oe_dlb = prefix_cost + sum_out - max_leaf_out;

    int64_t sum_in = 0;
    int64_t max_root_in = 0;
    for (int v = 0; v < n; ++v) {
        if (visited.test(v)) continue; // Incoming edge already fixed in prefix
        int64_t miew = INF;
        for (const auto& e : inst.sorted_in[v]) {
            if (!visited.test(e.to) || e.to == last_v) {
                miew = e.weight;
                break;
            }
        }
        if (miew == INF) {
            if (first_v == -1 && inst.in_degree[v] == 0) {
                miew = 0;
            } else {
                return INF;
            }
        }
        sum_in += miew;
        if (first_v == -1 && inst.in_degree[v] == 0 && miew > max_root_in) {
            max_root_in = miew;
        }
    }
    int64_t ie_dlb = prefix_cost + sum_in - (first_v == -1 ? max_root_in : 0);

    return max(oe_dlb, ie_dlb);
}

// ============================================================================
// Search Tree Node Structure for Global Pool & Work Stealing Local Pools
// ============================================================================
struct SearchNode {
    vector<int> path;
    Bitset1024 visited;
    vector<int> in_deg;
    vector<int> ready;
    int64_t prefix_cost = 0;
    int64_t lb = 0;
    uint64_t hash_key = 0;
    int primary_subspace = -1;
};

// ============================================================================
// Precedence-Aware Local Search & Metaheuristic Engine (Paper 2: LKH-style)
// ============================================================================
class SOPHeuristicEngine {
    const SOPInstance& inst;

public:
    SOPHeuristicEngine(const SOPInstance& instance) : inst(instance) {}

    int64_t evaluate_path(const vector<int>& path) const {
        int64_t total = 0;
        for (size_t i = 0; i + 1 < path.size(); ++i) {
            total += inst.cost[path[i]][path[i + 1]];
        }
        return total;
    }

    // Nearest-Neighbor Constructive Heuristic from a given root (Paper 1 Alg 1)
    pair<int64_t, vector<int>> nearest_neighbor(int start_root, mt19937_64* rng = nullptr) const {
        int n = inst.n;
        vector<int> deg = inst.in_degree;
        Bitset1024 vis;
        vector<int> ready;
        ready.reserve(n);

        vector<int> path;
        path.reserve(n);

        int curr = start_root;
        vis.set(curr);
        path.push_back(curr);

        for (int r : inst.roots) {
            if (r != curr) ready.push_back(r);
        }

        for (int step = 1; step < n; ++step) {
            for (int v : inst.imm_succ[curr]) {
                if (--deg[v] == 0) {
                    ready.push_back(v);
                }
            }
            if (ready.empty()) return {INF, {}};

            int best_idx = -1;
            int64_t best_w = INF;
            int second_idx = -1;
            int64_t second_w = INF;

            for (int i = 0; i < (int)ready.size(); ++i) {
                int cand = ready[i];
                int64_t w = inst.cost[curr][cand];
                if (w >= 0 && w < best_w) {
                    second_w = best_w;
                    second_idx = best_idx;
                    best_w = w;
                    best_idx = i;
                } else if (w >= 0 && w < second_w) {
                    second_w = w;
                    second_idx = i;
                }
            }

            if (best_idx == -1) best_idx = 0;
            // Slight randomization if rng provided
            if (rng && second_idx != -1 && ((*rng)() % 100 < 15)) {
                best_idx = second_idx;
            }

            curr = ready[best_idx];
            ready[best_idx] = ready.back();
            ready.pop_back();
            vis.set(curr);
            path.push_back(curr);
        }
        return {evaluate_path(path), path};
    }

    // SOP-3-Opt / Or-Opt / Relocate Local Search preserving precedence constraints
    void local_search_improve(vector<int>& path, int64_t& cost, const atomic<bool>& stop_flag) const {
        int n = inst.n;
        if (n <= 3) return;

        bool improved = true;
        while (improved && !stop_flag.load(memory_order_relaxed)) {
            improved = false;

            // 1. Precedence-Constrained Node & Segment Relocate (Or-Opt k = 1, 2, 3)
            for (int seg_len = 1; seg_len <= 3; ++seg_len) {
                for (int i = 0; i + seg_len <= n; ++i) {
                    if (stop_flag.load(memory_order_relaxed)) return;

                    // Find valid range [min_pos, max_pos] where segment path[i..i+seg_len-1] can move
                    int min_pos = 0;
                    int max_pos = n - seg_len;

                    // Cannot move left past any predecessor of any node in segment
                    for (int pos = i - 1; pos >= 0; --pos) {
                        int u = path[pos];
                        bool conflict = false;
                        for (int k = 0; k < seg_len; ++k) {
                            if (inst.pred_bits[path[i + k]].test(u)) {
                                conflict = true;
                                break;
                            }
                        }
                        if (conflict) {
                            min_pos = pos + 1;
                            break;
                        }
                    }

                    // Cannot move right past any successor of any node in segment
                    for (int pos = i + seg_len; pos < n; ++pos) {
                        int u = path[pos];
                        bool conflict = false;
                        for (int k = 0; k < seg_len; ++k) {
                            if (inst.succ_bits[path[i + k]].test(u)) {
                                conflict = true;
                                break;
                            }
                        }
                        if (conflict) {
                            max_pos = pos - seg_len;
                            break;
                        }
                    }

                    int u_prev = (i > 0) ? path[i - 1] : -1;
                    int u_first = path[i];
                    int u_last = path[i + seg_len - 1];
                    int u_next = (i + seg_len < n) ? path[i + seg_len] : -1;

                    int64_t rem_delta = 0;
                    if (u_prev != -1) rem_delta -= inst.cost[u_prev][u_first];
                    if (u_next != -1) rem_delta -= inst.cost[u_last][u_next];
                    if (u_prev != -1 && u_next != -1) rem_delta += inst.cost[u_prev][u_next];

                    int best_j = -1;
                    int64_t best_gain = 0;

                    for (int j = min_pos; j <= max_pos; ++j) {
                        if (j == i) continue;
                        // If moving to position j, the segment is inserted before index j (if j < i)
                        // or after index j + seg_len - 1 (if j > i)
                        int ins_prev = (j < i) ? ((j > 0) ? path[j - 1] : -1) : path[j + seg_len - 1];
                        int ins_next = (j < i) ? path[j] : ((j + seg_len < n) ? path[j + seg_len] : -1);

                        int64_t add_delta = rem_delta;
                        if (ins_prev != -1 && ins_next != -1) add_delta -= inst.cost[ins_prev][ins_next];
                        if (ins_prev != -1) add_delta += inst.cost[ins_prev][u_first];
                        if (ins_next != -1) add_delta += inst.cost[u_last][ins_next];

                        if (add_delta < best_gain) {
                            best_gain = add_delta;
                            best_j = j;
                        }
                    }

                    if (best_j != -1) {
                        vector<int> seg(path.begin() + i, path.begin() + i + seg_len);
                        path.erase(path.begin() + i, path.begin() + i + seg_len);
                        path.insert(path.begin() + best_j, seg.begin(), seg.end());
                        cost += best_gain;
                        improved = true;
                        break;
                    }
                }
                if (improved) break;
            }

            if (improved) continue;

            // 2. Precedence-Constrained 2-Opt (Subpath Reversal)
            // A subpath path[i..j] can be reversed iff no pair (a, b) in [i..j] has a precedence constraint
            for (int i = 0; i < n - 1; ++i) {
                if (stop_flag.load(memory_order_relaxed)) return;
                Bitset1024 forbidden;
                for (int w = 0; w < inst.num_words; ++w) {
                    forbidden.words[w] = inst.succ_bits[path[i]].words[w];
                }

                for (int j = i + 1; j < n; ++j) {
                    int vj = path[j];
                    if (forbidden.test(vj)) break; // Cannot reverse across [i..j] or any larger j
                    for (int w = 0; w < inst.num_words; ++w) {
                        forbidden.words[w] |= inst.succ_bits[vj].words[w];
                    }

                    // Compute cost of reversing path[i..j]
                    // Note: SOP is asymmetric! Reversing path[i..j] changes all internal directed edges too!
                    int64_t old_sub = 0, new_sub = 0;
                    if (i > 0) {
                        old_sub += inst.cost[path[i - 1]][path[i]];
                        new_sub += inst.cost[path[i - 1]][path[j]];
                    }
                    if (j + 1 < n) {
                        old_sub += inst.cost[path[j]][path[j + 1]];
                        new_sub += inst.cost[path[i]][path[j + 1]];
                    }
                    for (int k = i; k < j; ++k) {
                        old_sub += inst.cost[path[k]][path[k + 1]];
                        new_sub += inst.cost[path[k + 1]][path[k]];
                    }
                    if (new_sub < old_sub) {
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

    // Precedence-preserving Double-Bridge / Topological Perturbation for Iterated Local Search
    void perturb(vector<int>& path, int64_t& cost, mt19937_64& rng) const {
        int n = inst.n;
        if (n < 6) return;
        int num_kick = min(n / 4, 12);
        for (int k = 0; k < num_kick; ++k) {
            int i = rng() % n;
            int min_pos = i, max_pos = i;
            for (int pos = i - 1; pos >= 0; --pos) {
                if (inst.pred_bits[path[i]].test(path[pos])) break;
                min_pos = pos;
            }
            for (int pos = i + 1; pos < n; ++pos) {
                if (inst.succ_bits[path[i]].test(path[pos])) break;
                max_pos = pos;
            }
            if (max_pos > min_pos) {
                int target = min_pos + (rng() % (max_pos - min_pos + 1));
                if (target != i) {
                    int val = path[i];
                    path.erase(path.begin() + i);
                    path.insert(path.begin() + target, val);
                }
            }
        }
        cost = evaluate_path(path);
    }
};

// ============================================================================
// Master Parallel Branch-and-Bound Solver (Papers 1, 2, 3)
// ============================================================================
class ParallelSOPSolver {
    const SOPInstance& inst;
    int num_threads;
    double time_limit;
    chrono::steady_clock::time_point start_time;

    // Shared Best Solution (Write-protected, atomic read for cost - Paper 3 Sec 5.3)
    atomic<int64_t> best_cost{INF};
    vector<int> best_path;
    mutex best_sol_mutex;

    // Termination & Status Flags
    atomic<bool> stop_search{false};
    atomic<bool> timed_out{false};
    int64_t static_lower_bound = 0;

    // Global Pool (Paper 3 Sec 3.4 & 4.2)
    vector<SearchNode> unexplored_pool;
    vector<SearchNode> promising_pool;
    mutex global_pool_mutex;

    // Per-Thread State for Work Stealing & Thread Restart
    struct WorkerState {
        mutex local_pool_mutex;
        vector<SearchNode> local_pool;
        atomic<uint64_t> enumerated_nodes{0};
        atomic<int> best_updates_in_period{0};
        atomic<int> lb_depth_updates_in_period{0};
        atomic<bool> is_idle{false};
        atomic<bool> restart_requested{false};
        int tid = 0;
    };
    vector<unique_ptr<WorkerState>> workers;
    atomic<int> active_workers_count{0};

    // Pre-update mode tracking (Paper 3 Sec 4.2)
    atomic<int64_t> global_lowest_lb{INF};
    atomic<int> global_lowest_lb_depth{0};

    // Shared History Table
    unique_ptr<ParallelHistoryTable> history_table;

public:
    ParallelSOPSolver(const SOPInstance& instance, int threads, double t_limit)
        : inst(instance), num_threads(max(1, threads)), time_limit(t_limit) {
        start_time = chrono::steady_clock::now();
        // Size history table to 2^22 buckets (4M buckets)
        history_table = make_unique<ParallelHistoryTable>(1ULL << 22, inst.num_words);
    }

    inline double elapsed_seconds() const {
        return chrono::duration<double>(chrono::steady_clock::now() - start_time).count();
    }

    inline bool check_timeout() {
        if (time_limit > 0.0 && elapsed_seconds() >= time_limit) {
            timed_out.store(true, memory_order_relaxed);
            stop_search.store(true, memory_order_relaxed);
            return true;
        }
        return false;
    }

    void update_best_solution(int64_t cost, const vector<int>& path, int tid = -1) {
        if (cost >= best_cost.load(memory_order_relaxed)) return;
        lock_guard<mutex> lock(best_sol_mutex);
        if (cost < best_cost.load(memory_order_relaxed)) {
            best_cost.store(cost, memory_order_relaxed);
            best_path = path;
            if (tid >= 0 && tid < (int)workers.size()) {
                workers[tid]->best_updates_in_period.fetch_add(1, memory_order_relaxed);
            }
            if (cost <= static_lower_bound) {
                stop_search.store(true, memory_order_relaxed);
            }
        }
    }

    // Compute Static Lower Bound (Bidirectional Edge SLB + MCPM Hungarian SLB + MST SLB)
    int64_t compute_static_lb() {
        Bitset1024 empty_vis;
        int dummy_moew = 0;
        int64_t edge_slb = compute_bidirectional_edge_dlb(inst, empty_vis, -1, -1, 0, dummy_moew);

        // Hungarian MCPM SLB
        vector<int> rem_out, rem_in;
        for (int i = 0; i < inst.n; ++i) {
            rem_out.push_back(i);
            rem_in.push_back(i);
        }
        // Add 1 dummy node (-1) to allow open Hamiltonian path of n-1 edges
        HungarianSolver hungarian(inst.n);
        int64_t mcpm_slb = 0;
        if (inst.n <= 600) {
            // Replace sink->source with 0-cost dummy
            vector<int> r_out = rem_out;
            vector<int> r_in = rem_in;
            if (inst.roots.size() == 1 && inst.leaves.size() == 1 && inst.roots[0] != inst.leaves[0]) {
                int s = inst.roots[0], t = inst.leaves[0];
                r_out.erase(find(r_out.begin(), r_out.end(), t));
                r_in.erase(find(r_in.begin(), r_in.end(), s));
                r_out.push_back(-1);
                r_in.push_back(-1);
                mcpm_slb = hungarian.solve_subproblem(inst, r_out, r_in);
            }
        }

        return max(edge_slb, mcpm_slb);
    }

    // Build initial Global Pool via BFS splitting (Paper 3 Sec 3.4)
    void initialize_global_pool(int num_bb_threads) {
        vector<SearchNode> current_level;

        for (int r : inst.roots) {
            SearchNode node;
            node.path = {r};
            node.visited.set(r);
            node.in_deg = inst.in_degree;
            for (int r2 : inst.roots) {
                if (r2 != r) node.ready.push_back(r2);
            }
            for (int v : inst.imm_succ[r]) {
                if (--node.in_deg[v] == 0) node.ready.push_back(v);
            }
            node.prefix_cost = 0;
            node.hash_key = inst.zobrist_node[r] ^ inst.zobrist_last[r];
            node.primary_subspace = r;
            int dummy = 0;
            node.lb = compute_bidirectional_edge_dlb(inst, node.visited, r, r, 0, dummy);
            if (node.lb < best_cost.load()) {
                current_level.push_back(std::move(node));
            }
        }

        int depth = 1;
        while (!current_level.empty() && (int)current_level.size() < num_bb_threads * 4 && depth < inst.n - 1) {
            if (check_timeout()) break;
            vector<SearchNode> next_level;
            for (const auto& parent : current_level) {
                int last_u = parent.path.back();
                for (int v : parent.ready) {
                    int64_t edge_w = inst.cost[last_u][v];
                    if (edge_w < 0) continue;

                    SearchNode child;
                    child.path = parent.path;
                    child.path.push_back(v);
                    child.visited = parent.visited;
                    child.visited.set(v);
                    child.in_deg = parent.in_deg;
                    child.prefix_cost = parent.prefix_cost + edge_w;
                    child.primary_subspace = (depth == 1) ? v : parent.primary_subspace;
                    child.hash_key = (parent.hash_key ^ inst.zobrist_last[last_u])
                                     ^ inst.zobrist_node[v] ^ inst.zobrist_last[v];

                    for (int r_cand : parent.ready) {
                        if (r_cand != v) child.ready.push_back(r_cand);
                    }
                    for (int succ : inst.imm_succ[v]) {
                        if (--child.in_deg[succ] == 0) child.ready.push_back(succ);
                    }

                    int dummy = 0;
                    child.lb = compute_bidirectional_edge_dlb(inst, child.visited, v, child.path[0], child.prefix_cost, dummy);
                    if (child.lb < best_cost.load(memory_order_relaxed)) {
                        next_level.push_back(std::move(child));
                    }
                }
            }
            current_level = std::move(next_level);
            depth++;
        }

        // Sort global pool for Diversity across primary subspaces first, then ascending LB (Paper 3 Sec 3.4)
        // Group by primary_subspace, sort each group by LB ascending, then interleave round-robin
        // Note: since pop_back() takes from the back of unexplored_pool, we reverse at the end.
        unordered_map<int, vector<SearchNode>> groups;
        for (auto& node : current_level) {
            groups[node.primary_subspace].push_back(std::move(node));
        }
        for (auto& kv : groups) {
            sort(kv.second.begin(), kv.second.end(), [](const SearchNode& a, const SearchNode& b) {
                return a.lb < b.lb;
            });
        }

        vector<SearchNode> interleaved;
        bool added = true;
        size_t idx = 0;
        while (added) {
            added = false;
            for (auto& kv : groups) {
                if (idx < kv.second.size()) {
                    interleaved.push_back(std::move(kv.second[idx]));
                    added = true;
                }
            }
            idx++;
        }
        // Reverse so pop_back() pops the lowest LB / highest priority diverse nodes first
        reverse(interleaved.begin(), interleaved.end());
        unexplored_pool = std::move(interleaved);
    }

    // Dynamic Work Stealing (Paper 3 Sec 4.1 & Appendix A)
    bool steal_work(int thief_tid, SearchNode& out_node) {
        int num_bb = (int)workers.size();
        if (num_bb <= 1) return false;

        vector<int> candidates;
        for (int i = 0; i < num_bb; ++i) {
            if (i != thief_tid && !workers[i]->is_idle.load(memory_order_relaxed)) {
                candidates.push_back(i);
            }
        }
        if (candidates.empty()) return false;

        thread_local mt19937 rng(random_device{}());
        shuffle(candidates.begin(), candidates.end(), rng);

        for (int victim_tid : candidates) {
            unique_lock<mutex> lock(workers[victim_tid]->local_pool_mutex, try_to_lock);
            if (!lock.owns_lock()) continue;

            auto& vpool = workers[victim_tid]->local_pool;
            if (vpool.empty()) continue;

            // Formula from Paper 3 Sec 4.1: steal k * (1 - p) nodes
            int k = (int)vpool.size();
            int steal_cnt = max(1, (int)ceil(k * (1.0 - inst.precedence_density)));
            steal_cnt = min(steal_cnt, k);

            // Take from the front (shallowest / most promising nodes in victim's local pool)
            out_node = std::move(vpool.front());
            vpool.erase(vpool.begin());

            if (steal_cnt > 1) {
                lock_guard<mutex> thief_lock(workers[thief_tid]->local_pool_mutex);
                for (int s = 1; s < steal_cnt; ++s) {
                    workers[thief_tid]->local_pool.push_back(std::move(vpool.front()));
                    vpool.erase(vpool.begin());
                }
            }
            return true;
        }
        return false;
    }

    // B&B Worker Thread Routine
    void bb_worker_proc(int tid) {
        pin_thread_to_core(tid, num_threads);
        ThreadBlockAllocator block_alloc;
        HungarianSolver hungarian(inst.n);
        WorkerState& state = *workers[tid];

        int local_pool_depth_threshold = max(64, (inst.n * 3) / 4);

        // Active path stack for tracking stop requests
        vector<uint64_t> active_hashes;
        active_hashes.reserve(inst.n);

        // Recursive DFS / BestFS search with local pool offloading
        auto explore_subtree = [&](auto& self, SearchNode& curr) -> void {
            if (stop_search.load(memory_order_relaxed)) return;

            uint64_t n_enum = state.enumerated_nodes.fetch_add(1, memory_order_relaxed) + 1;
            if ((n_enum & 1023) == 0) {
                if (check_timeout()) return;
            }

            int depth = (int)curr.path.size();
            if (depth == inst.n) {
                update_best_solution(curr.prefix_cost, curr.path, tid);
                return;
            }

            int64_t cur_best = best_cost.load(memory_order_relaxed);
            if (curr.lb >= cur_best) return;

            // Check History Table (History Utilization + Parallel Domination)
            int last_v = curr.path.back();
            HistoryNode* hist_entry = nullptr;
            if (!history_table->check_and_insert(curr.hash_key, curr.visited, last_v,
                                                 curr.prefix_cost, curr.lb, cur_best,
                                                 tid, depth, inst.n, block_alloc, hist_entry)) {
                return;
            }

            // Optional tighter MCPM Hungarian Lower Bound at shallow depths when close to pruning
            if (inst.n <= 250 && depth <= inst.n / 3 && curr.lb + (cur_best - curr.lb) / 4 >= cur_best) {
                vector<int> r_out, r_in;
                r_out.reserve(inst.n - depth + 1);
                r_in.reserve(inst.n - depth + 1);
                r_out.push_back(last_v);
                for (int u = 0; u < inst.n; ++u) {
                    if (!curr.visited.test(u)) {
                        r_out.push_back(u);
                        r_in.push_back(u);
                    }
                }
                r_in.push_back(-1); // Dummy sink
                int64_t mcpm_lb = curr.prefix_cost + hungarian.solve_subproblem(inst, r_out, r_in);
                if (mcpm_lb > curr.lb) {
                    curr.lb = mcpm_lb;
                    if (hist_entry) hist_entry->lower_bound.store(mcpm_lb, memory_order_relaxed);
                }
                if (curr.lb >= cur_best) {
                    if (hist_entry) hist_entry->active_tid.store(-1, memory_order_relaxed);
                    return;
                }
            }

            // Pre-update mode tracking (Paper 3 Sec 4.2)
            int64_t g_low = global_lowest_lb.load(memory_order_relaxed);
            if (curr.lb < g_low) {
                global_lowest_lb.store(curr.lb, memory_order_relaxed);
                global_lowest_lb_depth.store(depth, memory_order_relaxed);
                state.lb_depth_updates_in_period.fetch_add(1, memory_order_relaxed);
            } else if (curr.lb == g_low && depth > global_lowest_lb_depth.load(memory_order_relaxed)) {
                global_lowest_lb_depth.store(depth, memory_order_relaxed);
                state.lb_depth_updates_in_period.fetch_add(1, memory_order_relaxed);
            }

            // Periodically check Stop-Request Buffer (Paper 3 Sec 5.1)
            if ((n_enum & 255) == 0) {
                for (int s = 0; s < ParallelHistoryTable::STOP_BUF_SIZE; ++s) {
                    if (history_table->stop_buffer[s].target_tid.load(memory_order_acquire) == tid) {
                        uint64_t stop_hash = history_table->stop_buffer[s].hash_key.load(memory_order_relaxed);
                        for (uint64_t h : active_hashes) {
                            if (h == stop_hash) {
                                history_table->stop_buffer[s].target_tid.store(-1, memory_order_relaxed);
                                if (hist_entry) hist_entry->active_tid.store(-1, memory_order_relaxed);
                                return; // Abort dominated subtree!
                            }
                        }
                    }
                }
            }

            // Generate and evaluate children in Best-First Order (Paper 3 Sec 3.2)
            struct CandChild {
                int v;
                int64_t prefix_cost;
                int64_t lb;
                uint64_t hash_key;
            };
            vector<CandChild> children;
            children.reserve(curr.ready.size());

            for (int v : curr.ready) {
                int64_t w = inst.cost[last_v][v];
                if (w < 0) continue;

                int64_t c_prefix = curr.prefix_cost + w;
                if (c_prefix >= best_cost.load(memory_order_relaxed)) continue;

                curr.visited.set(v);
                int dummy = 0;
                int64_t c_lb = compute_bidirectional_edge_dlb(inst, curr.visited, v, curr.path[0], c_prefix, dummy);
                curr.visited.reset(v);

                if (c_lb < best_cost.load(memory_order_relaxed)) {
                    uint64_t c_hash = (curr.hash_key ^ inst.zobrist_last[last_v])
                                      ^ inst.zobrist_node[v] ^ inst.zobrist_last[v];
                    children.push_back({v, c_prefix, c_lb, c_hash});
                }
            }

            if (children.empty()) {
                if (hist_entry) hist_entry->active_tid.store(-1, memory_order_relaxed);
                return;
            }

            // Sort children by ascending LB (tie-break by edge weight / prefix cost)
            sort(children.begin(), children.end(), [](const CandChild& a, const CandChild& b) {
                if (a.lb != b.lb) return a.lb < b.lb;
                return a.prefix_cost < b.prefix_cost;
            });

            active_hashes.push_back(curr.hash_key);

            // If multiple threads and depth < threshold, push sibling children to local_pool for work stealing
            if (num_threads > 1 && depth < local_pool_depth_threshold && children.size() > 1) {
                {
                    lock_guard<mutex> l_lock(state.local_pool_mutex);
                    // Push in reverse order so the best siblings are at the back or front consistently
                    for (size_t i = 1; i < children.size(); ++i) {
                        const auto& ch = children[i];
                        SearchNode sib;
                        sib.path = curr.path;
                        sib.path.push_back(ch.v);
                        sib.visited = curr.visited;
                        sib.visited.set(ch.v);
                        sib.in_deg = curr.in_deg;
                        sib.prefix_cost = ch.prefix_cost;
                        sib.lb = ch.lb;
                        sib.hash_key = ch.hash_key;
                        sib.primary_subspace = curr.primary_subspace;
                        for (int r_cand : curr.ready) {
                            if (r_cand != ch.v) sib.ready.push_back(r_cand);
                        }
                        for (int succ : inst.imm_succ[ch.v]) {
                            if (--sib.in_deg[succ] == 0) sib.ready.push_back(succ);
                        }
                        state.local_pool.push_back(std::move(sib));
                    }
                }

                // Immediately explore the most promising child (children[0]) in place
                {
                    const auto& best_ch = children[0];
                    SearchNode next_node;
                    next_node.path = curr.path;
                    next_node.path.push_back(best_ch.v);
                    next_node.visited = curr.visited;
                    next_node.visited.set(best_ch.v);
                    next_node.in_deg = curr.in_deg;
                    next_node.prefix_cost = best_ch.prefix_cost;
                    next_node.lb = best_ch.lb;
                    next_node.hash_key = best_ch.hash_key;
                    next_node.primary_subspace = curr.primary_subspace;
                    for (int r_cand : curr.ready) {
                        if (r_cand != best_ch.v) next_node.ready.push_back(r_cand);
                    }
                    for (int succ : inst.imm_succ[best_ch.v]) {
                        if (--next_node.in_deg[succ] == 0) next_node.ready.push_back(succ);
                    }
                    self(self, next_node);
                }
            } else {
                // In-place backtracking for single thread or deep subtrees (zero allocation overhead)
                for (const auto& ch : children) {
                    if (stop_search.load(memory_order_relaxed) || state.restart_requested.load(memory_order_relaxed)) break;
                    if (ch.lb >= best_cost.load(memory_order_relaxed)) continue;

                    // Step forward
                    curr.path.push_back(ch.v);
                    curr.visited.set(ch.v);
                    int64_t prev_prefix = curr.prefix_cost;
                    int64_t prev_lb = curr.lb;
                    uint64_t prev_hash = curr.hash_key;
                    curr.prefix_cost = ch.prefix_cost;
                    curr.lb = ch.lb;
                    curr.hash_key = ch.hash_key;

                    vector<int> prev_ready = curr.ready;
                    curr.ready.clear();
                    for (int r_cand : prev_ready) {
                        if (r_cand != ch.v) curr.ready.push_back(r_cand);
                    }
                    for (int succ : inst.imm_succ[ch.v]) {
                        if (--curr.in_deg[succ] == 0) curr.ready.push_back(succ);
                    }

                    self(self, curr);

                    // Backtrack
                    for (int succ : inst.imm_succ[ch.v]) {
                        curr.in_deg[succ]++;
                    }
                    curr.ready = std::move(prev_ready);
                    curr.hash_key = prev_hash;
                    curr.lb = prev_lb;
                    curr.prefix_cost = prev_prefix;
                    curr.visited.reset(ch.v);
                    curr.path.pop_back();
                }
            }

            active_hashes.pop_back();
            if (hist_entry) {
                hist_entry->active_tid.store(-1, memory_order_relaxed);
            }
        };

        // Main Worker Loop: Local Pool -> Global Pool -> Work Stealing (Paper 3 Sec 4.1 & 4.2)
        while (!stop_search.load(memory_order_relaxed)) {
            if (check_timeout()) break;

            // Handle thread restart signal if flagged by coordinator (Paper 3 Sec 4.2)
            if (state.restart_requested.load(memory_order_relaxed)) {
                state.restart_requested.store(false, memory_order_relaxed);
                vector<SearchNode> moved_nodes;
                {
                    lock_guard<mutex> l_lock(state.local_pool_mutex);
                    moved_nodes = std::move(state.local_pool);
                    state.local_pool.clear();
                }
                if (!moved_nodes.empty()) {
                    lock_guard<mutex> g_lock(global_pool_mutex);
                    for (auto& mn : moved_nodes) {
                        if (mn.lb < best_cost.load(memory_order_relaxed)) {
                            unexplored_pool.push_back(std::move(mn));
                        }
                    }
                }
            }

            SearchNode next_task;
            bool has_task = false;

            // 1. Check thread's own local pool (LIFO / most recent deepest sibling first)
            {
                lock_guard<mutex> l_lock(state.local_pool_mutex);
                while (!state.local_pool.empty()) {
                    SearchNode cand = std::move(state.local_pool.back());
                    state.local_pool.pop_back();
                    if (cand.lb < best_cost.load(memory_order_relaxed)) {
                        next_task = std::move(cand);
                        has_task = true;
                        break;
                    }
                }
            }

            // 2. Check Global Pool (Promising Pool or Unexplored Pool)
            if (!has_task) {
                lock_guard<mutex> g_lock(global_pool_mutex);
                // 50% exploitation (promising_pool) / 50% exploration (unexplored_pool)
                bool prefer_promising = (tid % 2 == 0);
                auto pop_valid = [&](vector<SearchNode>& pool) -> bool {
                    while (!pool.empty()) {
                        SearchNode cand = std::move(pool.back());
                        pool.pop_back();
                        if (cand.lb < best_cost.load(memory_order_relaxed)) {
                            next_task = std::move(cand);
                            return true;
                        }
                    }
                    return false;
                };

                if (prefer_promising) {
                    has_task = pop_valid(promising_pool) || pop_valid(unexplored_pool);
                } else {
                    has_task = pop_valid(unexplored_pool) || pop_valid(promising_pool);
                }
            }

            // 3. Try Work Stealing from active victim threads
            if (!has_task) {
                has_task = steal_work(tid, next_task);
            }

            if (has_task) {
                explore_subtree(explore_subtree, next_task);
            } else {
                // Mark worker idle and check global termination condition
                state.is_idle.store(true, memory_order_relaxed);
                if (active_workers_count.fetch_sub(1, memory_order_acq_rel) == 1) {
                    // Double check pools under lock before declaring completion
                    lock_guard<mutex> g_lock(global_pool_mutex);
                    bool any_left = !unexplored_pool.empty() || !promising_pool.empty();
                    if (!any_left) {
                        for (auto& w : workers) {
                            lock_guard<mutex> l_lock(w->local_pool_mutex);
                            if (!w->local_pool.empty()) {
                                any_left = true;
                                break;
                            }
                        }
                    }
                    if (!any_left) {
                        stop_search.store(true, memory_order_release);
                        break;
                    }
                }
                // Brief pause before re-checking for stolen work
                this_thread::sleep_for(chrono::microseconds(50));
                active_workers_count.fetch_add(1, memory_order_relaxed);
                state.is_idle.store(false, memory_order_relaxed);
            }
        }
    }

    // Cooperative Metaheuristic Thread Routine (Paper 2: LKH / ILS + Thread Restart Coordinator)
    void heuristic_and_coordinator_proc(int tid) {
        pin_thread_to_core(tid, num_threads);
        SOPHeuristicEngine engine(inst);
        mt19937_64 rng(0xC0FFEE123456789ULL + tid);

        vector<int> cur_path;
        int64_t cur_cost = INF;
        {
            lock_guard<mutex> lock(best_sol_mutex);
            cur_path = best_path;
            cur_cost = best_cost.load(memory_order_relaxed);
        }

        auto last_restart_check = chrono::steady_clock::now();
        const double restart_period_sec = 25.0; // Paper 3 Appendix B: 25s optimal for SOPLIB

        while (!stop_search.load(memory_order_relaxed)) {
            if (check_timeout()) break;

            // Sync with B&B's shared best solution (Paper 2 Sec 4.4)
            int64_t shared_best = best_cost.load(memory_order_relaxed);
            if (shared_best < cur_cost) {
                lock_guard<mutex> lock(best_sol_mutex);
                cur_path = best_path;
                cur_cost = best_cost.load(memory_order_relaxed);
            }

            // Perturb and apply SOP local search
            vector<int> trial_path = cur_path;
            int64_t trial_cost = cur_cost;
            engine.perturb(trial_path, trial_cost, rng);
            engine.local_search_improve(trial_path, trial_cost, stop_search);

            if (trial_cost < best_cost.load(memory_order_relaxed)) {
                update_best_solution(trial_cost, trial_path, -1);
                cur_path = trial_path;
                cur_cost = trial_cost;
            } else if (rng() % 100 < 20) {
                // Occasional fresh randomized Nearest-Neighbor start
                int r = inst.roots[rng() % inst.roots.size()];
                auto [nn_c, nn_p] = engine.nearest_neighbor(r, &rng);
                if (!nn_p.empty()) {
                    engine.local_search_improve(nn_p, nn_c, stop_search);
                    if (nn_c < best_cost.load(memory_order_relaxed)) {
                        update_best_solution(nn_c, nn_p, -1);
                    }
                    cur_path = std::move(nn_p);
                    cur_cost = nn_c;
                }
            }

            // Check Thread Restart Period (Paper 3 Sec 4.2)
            double since_restart = chrono::duration<double>(chrono::steady_clock::now() - last_restart_check).count();
            if (since_restart >= restart_period_sec && workers.size() >= 4) {
                last_restart_check = chrono::steady_clock::now();
                trigger_thread_restart();
            }
        }
    }

    // Dynamic Thread Restart (Paper 3 Sec 4.2)
    void trigger_thread_restart() {
        int num_bb = (int)workers.size();
        int top_k = (num_bb >= 31) ? 4 : ((num_bb >= 15) ? 2 : 1);

        struct ThreadRank {
            int tid;
            int score;
        };
        vector<ThreadRank> ranks(num_bb);
        bool any_sol_updates = false;
        for (int i = 0; i < num_bb; ++i) {
            int u = workers[i]->best_updates_in_period.exchange(0, memory_order_relaxed);
            int l = workers[i]->lb_depth_updates_in_period.exchange(0, memory_order_relaxed);
            if (u > 0) any_sol_updates = true;
            ranks[i] = {i, u * 1000 + l};
        }

        sort(ranks.begin(), ranks.end(), [](const ThreadRank& a, const ThreadRank& b) {
            return a.score > b.score;
        });

        // Top-k Most Promising threads share half their local pool into global promising_pool
        for (int i = 0; i < min(top_k, num_bb); ++i) {
            if (ranks[i].score == 0) continue;
            int m_tid = ranks[i].tid;
            vector<SearchNode> shared;
            {
                lock_guard<mutex> l_lock(workers[m_tid]->local_pool_mutex);
                auto& lpool = workers[m_tid]->local_pool;
                size_t share_cnt = lpool.size() / 2;
                for (size_t s = 0; s < share_cnt; ++s) {
                    shared.push_back(std::move(lpool.front()));
                    lpool.erase(lpool.begin());
                }
            }
            if (!shared.empty()) {
                lock_guard<mutex> g_lock(global_pool_mutex);
                for (auto& sn : shared) {
                    promising_pool.push_back(std::move(sn));
                }
            }
        }

        // Non-promising threads (score == 0) are requested to restart
        for (int i = top_k; i < num_bb; ++i) {
            if (ranks[i].score == 0) {
                workers[ranks[i].tid]->restart_requested.store(true, memory_order_relaxed);
            }
        }
    }

    // Main Solve Entry Point
    void solve(int64_t& out_cost, vector<int>& out_path, uint64_t& out_nodes, bool& out_optimal, double& out_time) {
        if (inst.n == 0) {
            out_cost = -1;
            out_path.clear();
            out_nodes = 0;
            out_optimal = false;
            out_time = elapsed_seconds();
            return;
        }
        if (inst.n == 1) {
            out_cost = 0;
            out_path = {0};
            out_nodes = 1;
            out_optimal = true;
            out_time = elapsed_seconds();
            return;
        }

        // 1. Construct initial solution via Nearest-Neighbor + Local Search across all root nodes
        SOPHeuristicEngine heur(inst);
        for (int r : inst.roots) {
            auto [c, p] = heur.nearest_neighbor(r);
            if (!p.empty() && c < best_cost.load()) {
                heur.local_search_improve(p, c, stop_search);
                update_best_solution(c, p);
            }
        }

        // 2. Compute Static Lower Bound (SLB)
        static_lower_bound = compute_static_lb();
        cerr << "[Info] Instance: " << inst.name << " (N=" << inst.n
             << ", Density=" << fixed << setprecision(3) << inst.precedence_density
             << ") | Initial Heuristic Cost: " << best_cost.load()
             << " | Static LB: " << static_lower_bound << "\n";

        if (best_cost.load() <= static_lower_bound) {
            out_cost = best_cost.load();
            out_path = best_path;
            out_nodes = 1;
            out_optimal = true;
            out_time = elapsed_seconds();
            return;
        }

        if (check_timeout()) {
            out_cost = (best_cost.load() < INF) ? best_cost.load() : -1;
            out_path = (best_cost.load() < INF) ? best_path : vector<int>{};
            out_nodes = 1;
            out_optimal = false;
            out_time = elapsed_seconds();
            return;
        }

        // 3. Determine thread allocation (Paper 2 Sec 4.4: 1 thread for LKH/ILS when threads > 1, rest for B&B)
        int num_bb_threads = (num_threads > 1) ? (num_threads - 1) : 1;
        bool use_heur_thread = (num_threads > 1);

        workers.resize(num_bb_threads);
        for (int i = 0; i < num_bb_threads; ++i) {
            workers[i] = make_unique<WorkerState>();
            workers[i]->tid = i;
        }

        // 4. Initialize Global Pool via BFS
        initialize_global_pool(num_bb_threads);
        if (unexplored_pool.empty() && !timed_out.load()) {
            // All root children were pruned against best_cost! Proven optimal immediately.
            out_cost = best_cost.load();
            out_path = best_path;
            out_nodes = inst.roots.size();
            out_optimal = true;
            out_time = elapsed_seconds();
            return;
        }

        // 5. Launch Parallel B&B Workers + Optional Cooperative Heuristic Thread
        active_workers_count.store(num_bb_threads, memory_order_relaxed);
        vector<thread> threads_vec;
        for (int i = 0; i < num_bb_threads; ++i) {
            threads_vec.emplace_back(&ParallelSOPSolver::bb_worker_proc, this, i);
        }
        if (use_heur_thread) {
            threads_vec.emplace_back(&ParallelSOPSolver::heuristic_and_coordinator_proc, this, num_bb_threads);
        }

        for (auto& th : threads_vec) {
            if (th.joinable()) th.join();
        }

        // 6. Aggregate Statistics
        uint64_t total_enum = 0;
        for (const auto& w : workers) {
            total_enum += w->enumerated_nodes.load(memory_order_relaxed);
        }
        out_nodes = max<uint64_t>(1, total_enum);
        out_time = elapsed_seconds();

        if (best_cost.load() < INF && !best_path.empty()) {
            out_cost = best_cost.load();
            out_path = best_path;
            out_optimal = !timed_out.load();
        } else {
            out_cost = -1;
            out_path.clear();
            out_optimal = false;
        }
    }
};

// ============================================================================
// Strict Solution Validator (Permutation, Precedence, Cost Recomputed)
// ============================================================================
bool validate_solution(const SOPInstance& inst, const vector<int>& path, int64_t reported_cost) {
    int n = inst.n;
    if ((int)path.size() != n) {
        cerr << "[Validation Failed] Path length " << path.size() << " != N (" << n << ")\n";
        return false;
    }

    vector<int> pos(n, -1);
    for (int i = 0; i < n; ++i) {
        int u = path[i];
        if (u < 0 || u >= n) {
            cerr << "[Validation Failed] Node index out of bounds: " << u << "\n";
            return false;
        }
        if (pos[u] != -1) {
            cerr << "[Validation Failed] Duplicate node in path: " << u << "\n";
            return false;
        }
        pos[u] = i;
    }

    // Check all precedence constraints from original matrix
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            if (i != j && inst.cost[i][j] < 0) {
                // i depends on j -> j must appear before i
                if (pos[j] > pos[i]) {
                    cerr << "[Validation Failed] Precedence violated: node " << j
                         << " (pos " << pos[j] << ") must precede node " << i
                         << " (pos " << pos[i] << ")\n";
                    return false;
                }
            }
        }
    }

    // Recompute cost from scratch
    int64_t recomputed_cost = 0;
    for (int i = 0; i + 1 < n; ++i) {
        int u = path[i];
        int v = path[i + 1];
        int64_t w = inst.cost[u][v];
        if (w < 0) {
            cerr << "[Validation Failed] Negative edge traversed between " << u << " and " << v << "\n";
            return false;
        }
        recomputed_cost += w;
    }

    if (recomputed_cost != reported_cost) {
        cerr << "[Validation Failed] Recomputed cost (" << recomputed_cost
             << ") != reported cost (" << reported_cost << ")\n";
        return false;
    }

    return true;
}

// ============================================================================
// Command-Line Interface & CSV Output Formatter
// ============================================================================
int main(int argc, char* argv[]) {
    ios::sync_with_stdio(false);

    string filepath;
    double time_limit = 0.0; // Default: no time limit
    int threads = 1;         // Default: single-threaded
    string output_csv;
    bool no_header = false;

    for (int i = 1; i < argc; ++i) {
        string arg = argv[i];
        if (arg == "--time-limit" && i + 1 < argc) {
            time_limit = stod(argv[++i]);
        } else if (arg == "--threads" && i + 1 < argc) {
            threads = stoi(argv[++i]);
        } else if (arg == "--output" && i + 1 < argc) {
            output_csv = argv[++i];
        } else if (arg == "--no-header") {
            no_header = true;
        } else if (arg[0] != '-' && filepath.empty()) {
            filepath = arg;
        } else {
            cerr << "[Warning] Unknown or misplaced argument: " << arg << "\n";
        }
    }

    if (filepath.empty()) {
        cerr << "Usage: " << argv[0]
             << " [filepath] [--time-limit SECONDS] [--threads N] [--output out.csv] [--no-header]\n";
        return 1;
    }

    SOPInstance instance;
    if (!instance.parse(filepath)) {
        cerr << "[Error] Failed to parse SOP instance file: " << filepath << "\n";
        return 1;
    }

    ParallelSOPSolver solver(instance, threads, time_limit);
    int64_t best_cost = -1;
    vector<int> best_path;
    uint64_t enumerated_nodes = 0;
    bool optimal = false;
    double elapsed_time = 0.0;

    solver.solve(best_cost, best_path, enumerated_nodes, optimal, elapsed_time);

    if (best_cost != -1 && !best_path.empty()) {
        if (!validate_solution(instance, best_path, best_cost)) {
            cerr << "[Error] Solution failed validity check!\n";
            return 2;
        }
        cerr << "[Success] Valid solution verified (Cost=" << best_cost
             << ", Optimal=" << (optimal ? "yes" : "no")
             << ", Time=" << fixed << setprecision(4) << elapsed_time << "s"
             << ", Nodes=" << enumerated_nodes << ")\n";
    } else {
        cerr << "[Info] No feasible solution found before timeout.\n";
    }

    // Format Best Path string (0-based space-separated)
    ostringstream path_ss;
    for (size_t i = 0; i < best_path.size(); ++i) {
        if (i > 0) path_ss << " ";
        path_ss << best_path[i];
    }

    const string header_row = "Instance,Instance Size,Time,Enumerated Nodes,Best Cost,Best Path,Optimal";
    ostringstream data_row;
    data_row << instance.name << ","
             << instance.n << ","
             << fixed << setprecision(6) << elapsed_time << ","
             << enumerated_nodes << ","
             << best_cost << ","
             << path_ss.str() << ","
             << (optimal ? "yes" : "no");

    // 1. Print to stdout
    if (!no_header) {
        cout << header_row << "\n";
    }
    cout << data_row.str() << "\n";
    cout.flush();

    // 2. Write/append to CSV file if --output is given
    if (!output_csv.empty()) {
        bool write_file_header = !no_header;
        if (write_file_header && filesystem::exists(output_csv)) {
            error_code ec;
            auto fsize = filesystem::file_size(output_csv, ec);
            if (!ec && fsize > 0) {
                write_file_header = false; // File exists and is non-empty
            }
        }

        ofstream out_file(output_csv, ios::app);
        if (!out_file.is_open()) {
            cerr << "[Error] Could not open output CSV file for writing: " << output_csv << "\n";
            return 1;
        }
        if (write_file_header) {
            out_file << header_row << "\n";
        }
        out_file << data_row.str() << "\n";
    }

    return 0;
}
