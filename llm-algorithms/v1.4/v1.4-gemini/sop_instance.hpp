#ifndef SOP_INSTANCE_HPP
#define SOP_INSTANCE_HPP

#include "bitset.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace sop {

/**
 * @brief SOP Instance representation, TSPLIB (.sop) parser, transitive closure
 *        preprocessor, Hungarian AP relaxation, and Directed Held-Karp MST relaxation.
 */
struct Instance {
    std::string filepath;
    std::string filename;
    std::string header_name;
    int n = 0;
    int nwords = 0;

    std::vector<int64_t> raw_mat;       // Raw N x N TSPLIB matrix
    std::vector<int64_t> cost;          // Cleaned cost matrix (INF if forbidden)
    std::vector<Bitset1024> preds;      // preds[i]: bitset of nodes j that MUST precede i
    std::vector<Bitset1024> succs;      // succs[i]: bitset of nodes j that MUST follow i
    std::vector<int> pred_count;        // Number of predecessors of i
    std::vector<int> succ_count;        // Number of successors of i
    std::vector<std::vector<int>> adj_out;
    std::vector<std::vector<int>> adj_in;
    std::vector<int> start_candidates;
    std::vector<int> end_candidates;
    Bitset1024 start_mask;
    Bitset1024 end_mask;
    std::vector<uint64_t> zobrist;

    // Assignment Problem (AP) Relaxation dual potentials and reduced costs
    int64_t root_ap_lb = 0;
    std::vector<int64_t> u_pot;
    std::vector<int64_t> v_pot;
    std::vector<int64_t> rcost;

    // Directed Held-Karp Minimum Spanning Tree (HK-MST) Lagrangian potentials
    static constexpr int64_t HK_SCALE = 1000;
    int64_t root_hk_lb = 0;
    std::vector<int64_t> pi_out;
    std::vector<int64_t> pi_in;
    std::vector<int64_t> sym_hk;

    // Static 1st-minimum incoming/outgoing costs & sources
    std::vector<int64_t> min_in1;
    std::vector<int> min_in1_src;
    std::vector<int64_t> min_out1;
    std::vector<int> min_out1_dst;

    bool has_cycle = false;

    static std::string extract_basename(const std::string& p);

    bool parse(const std::string& path);
    void preprocess();
    void compute_ap_relaxation();
    void compute_hk_relaxation();

    // Fast inline Prim's MST on unvisited nodes (nodes where vis.test(w) == false)
    inline int64_t mst_complement(const Bitset1024& vis) const noexcept {
        int rem_nodes[MAX_NODES];
        int m = 0;
        for (int w = 0; w < n; ++w) {
            if (!vis.test(w)) rem_nodes[m++] = w;
        }
        if (m <= 1) return 0;
        int64_t min_e[MAX_NODES];
        int first = rem_nodes[0];
        const int64_t* row0 = &sym_hk[first * n];
        for (int i = 1; i < m; ++i) min_e[i] = row0[rem_nodes[i]];

        int64_t mst_sum = 0;
        for (int step = 1; step < m; ++step) {
            int best_idx = step;
            int64_t best_d = min_e[step];
            for (int i = step + 1; i < m; ++i) {
                if (min_e[i] < best_d) {
                    best_d = min_e[i];
                    best_idx = i;
                }
            }
            if (best_d >= (INF / 2) * HK_SCALE) return INF * HK_SCALE;
            mst_sum += best_d;
            int chosen = rem_nodes[best_idx];
            rem_nodes[best_idx] = rem_nodes[step];
            min_e[best_idx] = min_e[step];
            const int64_t* row_c = &sym_hk[chosen * n];
            for (int i = step + 1; i < m; ++i) {
                int64_t d = row_c[rem_nodes[i]];
                if (d < min_e[i]) min_e[i] = d;
            }
        }
        return mst_sum;
    }
};

} // namespace sop

#endif // SOP_INSTANCE_HPP