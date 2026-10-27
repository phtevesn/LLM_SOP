#include "cli.hpp"
#include "numa_topology.hpp"
#include "shared_state.hpp"
#include "solver.hpp"
#include "sop_instance.hpp"
#include "validator.hpp"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

int main(int argc, char* argv[]) {
    using namespace sop;
    auto wall_start = std::chrono::steady_clock::now();

    CLIOptions opts = CLIOptions::parse(argc, argv);
    if (!opts.valid) {
        std::cerr << opts.error_msg << "\n";
        return 1;
    }

    Instance inst;
    if (!inst.parse(opts.input_path)) {
        return 1;
    }

    NumaTopology numa;
    numa.detect(opts.threads);

    std::cerr << "[Info] Loaded instance '" << inst.filename << "' (N=" << inst.n
              << ", starts=" << inst.start_candidates.size()
              << ", ends=" << inst.end_candidates.size()
              << ", AP_LB=" << inst.root_ap_lb
              << ", HK_LB=" << inst.root_hk_lb
              << ", threads=" << opts.threads
              << ", NUMA_nodes=" << numa.nodes.size() << ")\n";

    SharedSolverState shared(inst, opts.time_limit);
    ExactSOPSolver solver(inst, shared, numa, opts.threads);
    auto result = solver.solve();

    // Perform strict from-scratch validity check on the final path
    if (result.best_cost >= 0 && !result.best_path.empty()) {
        std::string val_err;
        if (!Validator::validate(inst, result.best_path, result.best_cost, val_err)) {
            std::cerr << "[CRITICAL VALIDATION FAILURE] " << val_err << "\n";
            result.best_cost = -1;
            result.best_path.clear();
            result.optimal = false;
        } else {
            std::cerr << "[Validator] Final path verified valid (permutation, all precedences, exact cost="
                      << result.best_cost << ", optimal=" << (result.optimal ? "yes" : "no") << ").\n";
        }
    } else {
        result.best_cost = -1;
        result.best_path.clear();
        result.optimal = false;
        std::cerr << "[Info] No feasible solution found within limits.\n";
    }

    auto wall_end = std::chrono::steady_clock::now();
    double total_wall_sec = std::chrono::duration<double>(wall_end - wall_start).count();

    // Format Best Path space-separated 0-based indices
    std::ostringstream path_ss;
    for (size_t i = 0; i < result.best_path.size(); ++i) {
        if (i > 0) path_ss << ' ';
        path_ss << result.best_path[i];
    }

    std::ostringstream row_ss;
    row_ss << inst.filename << ","
           << inst.n << ","
           << std::fixed << std::setprecision(6) << total_wall_sec << ","
           << result.enumerated_nodes << ","
           << result.best_cost << ","
           << path_ss.str() << ","
           << (result.optimal ? "yes" : "no") << "\n";

    const std::string header = "Instance,Instance Size,Time,Enumerated Nodes,Best Cost,Best Path,Optimal\n";
    const std::string row_str = row_ss.str();

    // Print to stdout (always includes header unless --no-header is given)
    if (!opts.no_header) {
        std::cout << header;
    }
    std::cout << row_str << std::flush;

    // Write/append to CSV file if --output is given
    if (!opts.output_csv.empty()) {
        bool file_empty_or_new = true;
        {
            std::ifstream check_in(opts.output_csv, std::ios::binary | std::ios::ate);
            if (check_in.is_open() && check_in.tellg() > 0) {
                file_empty_or_new = false;
            }
        }
        std::ofstream out_f(opts.output_csv, std::ios::out | std::ios::app);
        if (!out_f.is_open()) {
            std::cerr << "[Error] Could not open output CSV file: " << opts.output_csv << "\n";
            return 1;
        }
        if (file_empty_or_new && !opts.no_header) {
            out_f << header;
        }
        out_f << row_str;
    }

    return 0;
}