// Executable entry point: parse options, establish the whole-instance wall-clock
// deadline, install cooperative signal cancellation, solve, and emit one CSV row.
#include "sop/cli.hpp"
#include "sop/solver.hpp"

#include <iostream>
#include <stdexcept>

namespace {
void handle_signal(int signal) { sop::interrupted.store(signal, std::memory_order_relaxed); }
}
int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--help") { std::cout << sop::usage(); return 0; }
    if (argc == 2 && std::string(argv[1]) == "--version") { std::cout << "sop_exact 1.0.0 (C++17)\n"; return 0; }
    sop::Options options;
    try { options = sop::parse_options(argc, argv); }
    catch (const std::exception& e) { sop::log(std::string("argument error: ") + e.what()); std::cerr << sop::usage(); return 2; }
    auto start = sop::Clock::now();
    sop::Control control(start, options.time_limit);
    std::signal(SIGINT, handle_signal); std::signal(SIGTERM, handle_signal);
    sop::Instance input;
    try { input = sop::Instance::read(options.input); }
    catch (const std::exception& e) { sop::log(std::string("input error: ") + e.what()); return 2; }
    try {
        auto result = sop::solve(input, options, control);
        result.seconds = control.elapsed(); // Include solver-state destruction before serialization.
        if (!result.optimal && !result.infeasible) sop::log("result is not proven optimal");
        sop::write_result(options, result);
    } catch (const std::exception& e) { sop::log(std::string("solver/output error: ") + e.what()); return 3; }
    return 0;
}
