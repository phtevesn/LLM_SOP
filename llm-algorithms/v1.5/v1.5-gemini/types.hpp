/*
 * File: types.hpp
 * Purpose: Defines core data structures, constants, and the fixed-capacity
 *          64-bit word-packed BitSet1024 structure for representing precedence
 *          posets and visited vertex subsets for SOP instances up to N <= 1000.
 */

#ifndef SOP_TYPES_HPP
#define SOP_TYPES_HPP

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace sop {

// Large sentinel cost representing a forbidden or infeasible transition.
constexpr int64_t INF_COST = 1000000000000LL;
constexpr int INT_INF = 500000000;
constexpr int MAX_SOP_N = 1024;
constexpr int BITSET_WORDS = MAX_SOP_N / 64; // 16 words of 64 bits = 1024 bits

/*
 * Algorithm: Word-Packed Bit-Parallel Set Representation (BitSet1024)
 * Part: Fixed-capacity 1024-bit set supporting O(N/64) subset checks,
 *       intersections, unions, and hardware trailing-zero bit iteration.
 */
struct alignas(64) BitSet1024 {
    std::array<uint64_t, BITSET_WORDS> words{};

    BitSet1024() { clear(); }

    inline void clear() noexcept {
        std::memset(words.data(), 0, sizeof(words));
    }

    inline void set(int bit) noexcept {
        words[bit >> 6] |= (1ULL << (bit & 63));
    }

    inline void reset(int bit) noexcept {
        words[bit >> 6] &= ~(1ULL << (bit & 63));
    }

    inline bool test(int bit) const noexcept {
        return (words[bit >> 6] >> (bit & 63)) & 1ULL;
    }

    // Algorithm: Bit-Parallel Subset Inclusion Test
    // Part: Checks whether (*this) is a subset of `other` (i.e., (*this & ~other) == 0)
    inline bool is_subset_of(const BitSet1024& other, int num_words) const noexcept {
        for (int i = 0; i < num_words; ++i) {
            if ((words[i] & ~other.words[i]) != 0ULL) {
                return false;
            }
        }
        return true;
    }

    // Algorithm: Bit-Parallel Disjointness Test
    // Part: Checks whether (*this) and `other` share any set bit
    inline bool intersects(const BitSet1024& other, int num_words) const noexcept {
        for (int i = 0; i < num_words; ++i) {
            if ((words[i] & other.words[i]) != 0ULL) {
                return true;
            }
        }
        return false;
    }

    inline void bitwise_or(const BitSet1024& other, int num_words) noexcept {
        for (int i = 0; i < num_words; ++i) {
            words[i] |= other.words[i];
        }
    }

    inline int count(int num_words) const noexcept {
        int c = 0;
        for (int i = 0; i < num_words; ++i) {
            c += __builtin_popcountll(words[i]);
        }
        return c;
    }

    inline bool operator==(const BitSet1024& other) const noexcept {
        return std::memcmp(words.data(), other.words.data(), sizeof(words)) == 0;
    }
};

struct WeightedEdge {
    int cost;
    int to;
    bool operator<(const WeightedEdge& other) const noexcept {
        if (cost != other.cost) return cost < other.cost;
        return to < other.to;
    }
};

/*
 * Structure: SopInstance
 * Purpose: Holds both the original raw TSPLIB SOP instance and the preprocessed
 *          graph, precedence bitsets, position windows, and orientation mapping.
 */
struct SopInstance {
    std::string name;
    std::string filename;
    std::string type;
    std::string comment;
    int n = 0;
    int num_words = 0;

    // Original raw matrix T as parsed from the .sop file (for independent validation).
    // T[i][j] >= 0 is transition cost i -> j; T[i][j] < 0 means j must precede i.
    std::vector<std::vector<int64_t>> raw_matrix;

    // Working reduced cost matrix (after arc elimination and optional transposition).
    // Forbidden arcs are set to INT_INF.
    std::vector<std::vector<int>> cost;

    // Precedence matrix and bitsets in working orientation:
    // prec[u][v] = true iff u must precede v (u < v).
    std::vector<std::vector<uint8_t>> prec;
    std::vector<BitSet1024> pred_bits; // pred_bits[u] has bit v set iff v < u
    std::vector<BitSet1024> succ_bits; // succ_bits[u] has bit v set iff u < v
    std::vector<int> in_degree;        // number of predecessors of u
    std::vector<int> out_degree;       // number of successors of u

    // Earliest and latest feasible 0-based path positions for each node u
    std::vector<int> earliest_pos;
    std::vector<int> latest_pos;

    // Candidate start nodes (in_degree == 0) and terminal nodes (out_degree == 0)
    std::vector<int> start_nodes;
    std::vector<int> end_nodes;
    int unique_start = -1; // >= 0 if a single start node is forced
    int unique_end = -1;   // >= 0 if a single end node is forced

    // Flag indicating whether the working instance was transposed (reversed)
    // during preprocessing due to backward order-ideal asymmetry.
    bool is_transposed = false;
    bool is_feasible = true;
    int eliminated_arcs = 0;
    double precedence_density = 0.0;

    // Map working node index back to original 0..n-1 index
    inline int to_original_node(int u) const noexcept {
        return is_transposed ? (n - 1 - u) : u;
    }
};

struct SolverOptions {
    std::string filepath;
    double time_limit_sec = 0.0; // <= 0 means no time limit
    int threads = 1;
    std::string output_csv_path;
    bool no_header = false;
};

struct SolverResult {
    std::string instance_name;
    int instance_size = 0;
    double elapsed_seconds = 0.0;
    uint64_t enumerated_nodes = 0;
    int64_t best_cost = -1;
    std::vector<int> best_path;
    bool optimal = false;
    bool valid = false;
};

} // namespace sop

#endif // SOP_TYPES_HPP
