/*
 * File: validator.cpp
 * Purpose: Implements independent postprocessing validation of the computed SOP
 *          path (permutation completeness, precedence constraint satisfaction,
 *          and from-scratch cost recomputation against the unreduced input
 *          matrix) and writes standardized CSV output to stdout and optional
 *          output files.
 */

#include "validator.hpp"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace sop {

/*
 * Algorithm: Independent Three-Stage Certificate Validator
 * Part: Verifies (1) Bijective permutation of {0..N-1}, (2) All raw matrix
 *       precedences T[i][j] < 0 satisfy pos[j] < pos[i], and (3) Recomputed
 *       consecutive transition cost sum matches reported Best Cost.
 */
bool Validator::validate_solution(const SopInstance& inst,
                                  const SolverResult& res,
                                  std::string& error_msg) {
    if (res.best_cost < 0 || res.best_path.empty()) {
        error_msg = "No feasible solution to validate.";
        return false;
    }

    const int n = inst.n;
    if (static_cast<int>(res.best_path.size()) != n) {
        error_msg = "Invalid path length: expected " + std::to_string(n) +
                    ", got " + std::to_string(res.best_path.size());
        return false;
    }

    // Stage 1: Permutation check
    std::vector<int> pos(n, -1);
    for (int idx = 0; idx < n; ++idx) {
        int u = res.best_path[idx];
        if (u < 0 || u >= n) {
            error_msg = "Vertex index out of bounds [0, N-1]: " + std::to_string(u);
            return false;
        }
        if (pos[u] != -1) {
            error_msg = "Duplicate vertex in path: node " + std::to_string(u);
            return false;
        }
        pos[u] = idx;
    }

    // Stage 2: Precedence constraint check against raw matrix
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            if (inst.raw_matrix[i][j] < 0) {
                // Node i depends on node j => j must appear before i
                if (pos[j] >= pos[i]) {
                    error_msg = "Precedence violation: node " + std::to_string(j) +
                                " must precede node " + std::to_string(i) +
                                ", but pos[" + std::to_string(j) + "]=" + std::to_string(pos[j]) +
                                " >= pos[" + std::to_string(i) + "]=" + std::to_string(pos[i]);
                    return false;
                }
            }
        }
    }

    // Stage 3: Independent cost recomputation from scratch
    int64_t recomputed_cost = 0;
    for (int idx = 0; idx < n - 1; ++idx) {
        int a = res.best_path[idx];
        int b = res.best_path[idx + 1];
        int64_t w = inst.raw_matrix[a][b];
        if (w < 0) {
            error_msg = "Path traverses negative (precedence) entry T[" +
                        std::to_string(a) + "][" + std::to_string(b) + "]";
            return false;
        }
        recomputed_cost += w;
    }

    if (recomputed_cost != res.best_cost) {
        error_msg = "Cost mismatch: reported " + std::to_string(res.best_cost) +
                    ", recomputed " + std::to_string(recomputed_cost);
        return false;
    }

    return true;
}

/*
 * Algorithm: Conditional Header CSV Serializer
 * Part: Formats the solver result row and emits it to stdout (with header unless
 *       --no-header) and appends to --output file (writing header only if new/empty
 *       and not --no-header).
 */
void Validator::write_csv_output(const SolverOptions& opts, const SolverResult& res) {
    const std::string header =
        "Instance,Instance Size,Time,Enumerated Nodes,Best Cost,Best Path,Optimal";

    std::ostringstream path_ss;
    if (res.best_cost >= 0 && !res.best_path.empty()) {
        for (size_t i = 0; i < res.best_path.size(); ++i) {
            if (i > 0) path_ss << ' ';
            path_ss << res.best_path[i];
        }
    }

    std::ostringstream row_ss;
    row_ss << res.instance_name << ','
           << res.instance_size << ','
           << std::fixed << std::setprecision(6) << res.elapsed_seconds << ','
           << res.enumerated_nodes << ','
           << res.best_cost << ','
           << path_ss.str() << ','
           << (res.optimal ? "yes" : "no");

    const std::string data_row = row_ss.str();

    // Write to stdout
    if (!opts.no_header) {
        std::cout << header << '\n';
    }
    std::cout << data_row << '\n';
    std::cout.flush();

    // Write/append to CSV file if --output is provided
    if (!opts.output_csv_path.empty()) {
        bool file_needs_header = false;
        if (!opts.no_header) {
            if (!std::filesystem::exists(opts.output_csv_path) ||
                std::filesystem::file_size(opts.output_csv_path) == 0) {
                file_needs_header = true;
            }
        }
        std::ofstream out_csv(opts.output_csv_path, std::ios::out | std::ios::app);
        if (!out_csv.is_open()) {
            std::cerr << "[Error] Failed to open output CSV file: " << opts.output_csv_path << "\n";
            return;
        }
        if (file_needs_header) {
            out_csv << header << '\n';
        }
        out_csv << data_row << '\n';
        out_csv.flush();
    }
}

} // namespace sop
