/*
 * File: main.cpp
 * Purpose: Entry point for the Sequential Ordering Problem (SOP) exact solver.
 *          Parses command-line arguments:
 *            prog [filepath] [--time-limit SECONDS] [--threads N] [--output out.csv] [--no-header]
 *          Orchestrates parsing, NUMA-aware parallel Branch-and-Bound solving,
 *          independent solution verification, stderr logging, and CSV output.
 */

#include "parser.hpp"
#include "solver.hpp"
#include "types.hpp"
#include "validator.hpp"

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void print_usage(const char* prog_name) {
    std::cerr << "Usage: " << prog_name
              << " [filepath] [--time-limit SECONDS] [--threads N] [--output out.csv] [--no-header]\n";
}

bool parse_cli(int argc, char* argv[], sop::SolverOptions& opts) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--time-limit") {
            if (i + 1 >= argc) {
                std::cerr << "[Error] --time-limit requires a numeric argument.\n";
                return false;
            }
            opts.time_limit_sec = std::stod(argv[++i]);
        } else if (arg == "--threads") {
            if (i + 1 >= argc) {
                std::cerr << "[Error] --threads requires an integer argument.\n";
                return false;
            }
            opts.threads = std::max(1, std::stoi(argv[++i]));
        } else if (arg == "--output") {
            if (i + 1 >= argc) {
                std::cerr << "[Error] --output requires a file path argument.\n";
                return false;
            }
            opts.output_csv_path = argv[++i];
        } else if (arg == "--no-header") {
            opts.no_header = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "[Error] Unknown option: " << arg << "\n";
            return false;
        } else {
            if (opts.filepath.empty()) {
                opts.filepath = arg;
            } else {
                std::cerr << "[Error] Multiple input files specified: " << opts.filepath
                          << " and " << arg << "\n";
                return false;
            }
        }
    }

    if (opts.filepath.empty()) {
        std::cerr << "[Error] Missing input .sop filepath.\n";
        print_usage(argv[0]);
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    std::ios::sync_with_stdio(false);

    sop::SolverOptions opts;
    if (!parse_cli(argc, argv, opts)) {
        return 1;
    }

    try {
        auto run_start = std::chrono::steady_clock::now();

        // 1. Parse TSPLIB .sop instance
        sop::SopInstance inst = sop::SopParser::parse(opts.filepath);
        std::cerr << "[Info] Loaded instance '" << inst.filename << "' (N=" << inst.n
                  << ", threads=" << opts.threads;
        if (opts.time_limit_sec > 0.0) {
            std::cerr << ", time_limit=" << opts.time_limit_sec << "s";
        }
        std::cerr << ")\n";

        // 2. Run Exact Solver
        sop::SopSolver solver(inst, opts);
        sop::SolverResult res = solver.solve();

        auto run_end = std::chrono::steady_clock::now();
        res.elapsed_seconds = std::chrono::duration<double>(run_end - run_start).count();

        // 3. Independent Postprocessing Validity Check
        if (res.best_cost >= 0 && !res.best_path.empty()) {
            std::string err;
            res.valid = sop::Validator::validate_solution(inst, res, err);
            if (!res.valid) {
                std::cerr << "[Error] Solution validation failed: " << err << "\n";
                return 2;
            }
            std::cerr << "[Validation] Path verified valid (permutation=OK, precedences=OK, "
                      << "recomputed_cost=" << res.best_cost << ", optimal="
                      << (res.optimal ? "yes" : "no") << ", time=" << std::fixed
                      << std::setprecision(6) << res.elapsed_seconds << "s)\n";
        } else {
            std::cerr << "[Info] No feasible solution found before termination.\n";
        }

        // 4. Emit CSV output
        sop::Validator::write_csv_output(opts, res);
        return 0;

    } catch (const std::exception& ex) {
        std::cerr << "[Fatal Error] " << ex.what() << "\n";
        return 1;
    }
}
