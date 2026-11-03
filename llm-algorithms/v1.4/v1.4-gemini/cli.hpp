#ifndef SOP_CLI_HPP
#define SOP_CLI_HPP

#include <algorithm>
#include <string>

namespace sop {

/**
 * @brief Command-Line Interface argument parser supporting:
 *        prog [filepath] [--time-limit SECONDS] [--threads N] [--output out.csv] [--no-header]
 */
struct CLIOptions {
    std::string input_path;
    double time_limit = 0.0;
    int threads = 1;
    std::string output_csv;
    bool no_header = false;
    bool valid = true;
    std::string error_msg;

    static CLIOptions parse(int argc, char* argv[]) {
        CLIOptions opts;
        if (argc < 2) {
            opts.valid = false;
            opts.error_msg = "Usage: " + std::string(argv[0]) +
                             " [filepath] [--time-limit SECONDS] [--threads N] [--output out.csv] [--no-header]";
            return opts;
        }
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--time-limit" || arg == "-t") {
                if (i + 1 >= argc) {
                    opts.valid = false;
                    opts.error_msg = "Missing value for --time-limit";
                    return opts;
                }
                opts.time_limit = std::stod(argv[++i]);
            } else if (arg == "--threads" || arg == "-j") {
                if (i + 1 >= argc) {
                    opts.valid = false;
                    opts.error_msg = "Missing value for --threads";
                    return opts;
                }
                opts.threads = std::max(1, std::stoi(argv[++i]));
            } else if (arg == "--output" || arg == "-o") {
                if (i + 1 >= argc) {
                    opts.valid = false;
                    opts.error_msg = "Missing value for --output";
                    return opts;
                }
                opts.output_csv = argv[++i];
            } else if (arg == "--no-header") {
                opts.no_header = true;
            } else if (!arg.empty() && arg[0] == '-') {
                opts.valid = false;
                opts.error_msg = "Unknown flag: " + arg;
                return opts;
            } else if (opts.input_path.empty()) {
                opts.input_path = arg;
            } else {
                opts.valid = false;
                opts.error_msg = "Unexpected positional argument: " + arg;
                return opts;
            }
        }
        if (opts.input_path.empty()) {
            opts.valid = false;
            opts.error_msg = "Missing input .sop filepath";
        }
        return opts;
    }
};

} // namespace sop

#endif // SOP_CLI_HPP